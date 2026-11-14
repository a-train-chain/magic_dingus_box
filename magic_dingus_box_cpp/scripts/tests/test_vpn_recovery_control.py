"""Boundary tests for protected, generation-fenced Gluetun requests."""
import contextlib
import fcntl
import io
import http.server
import threading
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    from vpn_recovery.common import Paths
    from vpn_recovery.control import ControlClient, ControlError, ensure_control_credentials
except ImportError:
    ControlClient = None


class Runner:
    """Only Docker and nsenter are external; inspect returns real Docker fields."""
    def __init__(self):
        self.calls = []
        self.identity = 'a'*64 + ' 123 2026-11-14T01:02:03Z true\n'
        self.response = {'ok': True, 'status': 200, 'body': '{"port":12345}'}
        self.after_request = None
        self.fail = None

    def __call__(self, args, **kwargs):
        self.calls.append((args, kwargs))
        if args[0] == 'docker':
            return subprocess.CompletedProcess(args, 0, self.identity, '')
        if self.fail:
            raise self.fail
        if self.after_request:
            self.after_request()
        return subprocess.CompletedProcess(args, 0, json.dumps(self.response), '')

    def requests(self):
        return [(args, kwargs) for args, kwargs in self.calls if args[0] == 'nsenter']


class ControlTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(ControlClient, 'vpn_recovery control package is not implemented')
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.paths = Paths(root, root/'services', root/'config', root/'state', root/'run', root/'compose.lock')
        self.paths.services.mkdir()
        self.paths.compose_lock.touch()
        ensure_control_credentials(self.paths)
        self.runner = Runner()
        self.client = ControlClient(self.paths, self.runner)
        self.cid = 'a'*64
        self.keyfile = self.paths.services/'config/gluetun/auth/recovery.key'

    def test_public_reads_private_mutations(self):
        import tomllib
        roles = tomllib.loads(self.keyfile.with_name('config.toml').read_text())['roles']
        public = next(r for r in roles if r['auth'] == 'none')
        private = next(r for r in roles if r['auth'] == 'apikey')
        self.assertEqual(set(public['routes']), {
            'GET /v1/portforward', 'GET /v1/publicip/ip', 'GET /v1/openvpn/portforwarded'})
        self.assertEqual(set(private['routes']), {
            'GET /v1/vpn/settings', 'PUT /v1/vpn/settings',
            'GET /v1/vpn/status', 'PUT /v1/vpn/status'})
        self.assertEqual(private['apikey'], self.keyfile.read_text().strip())
        for route in ['/v1/portforward', '/v1/publicip/ip', '/v1/openvpn/portforwarded', '/v1/vpn/status', '/v1/vpn/settings']:
            self.assertEqual(self.client.get(route, self.cid), {'port': 12345})
        with self.assertRaises(ControlError):
            self.client.put('/v1/portforward', {}, self.cid)
        with self.assertRaises(ControlError):
            self.client.get('/openvpn/actions/restart', self.cid)

    def test_key_mode_and_idempotency(self):
        key = self.keyfile.read_text()
        self.assertGreaterEqual(len(key.strip()), 64)
        config = self.keyfile.with_name('config.toml')
        before = (self.keyfile.stat().st_ino, config.stat().st_ino)
        self.assertFalse(ensure_control_credentials(self.paths))
        self.assertEqual(key, self.keyfile.read_text())
        self.assertEqual(before, (self.keyfile.stat().st_ino, config.stat().st_ino))
        for path in [self.keyfile, config]:
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.keyfile.chmod(0o644)
        self.assertTrue(ensure_control_credentials(self.paths))
        self.assertEqual(stat.S_IMODE(self.keyfile.stat().st_mode), 0o600)

    def test_partial_put_and_plain_text_outcome_secrets_only_stdin(self):
        payload = {'provider': {'server_selection': {'countries': ['Canada'], 'hostnames': ['ca-1.protonvpn.net']}}}
        self.runner.response['body'] = 'VPN settings updated'
        self.assertEqual(self.client.put('/v1/vpn/settings', payload, self.cid), 'VPN settings updated')
        args, kwargs = self.runner.requests()[0]
        self.assertEqual(args[:4], ['nsenter', '--target', '123', '--net'])
        self.assertNotIn(self.keyfile.read_text().strip(), ' '.join(args))
        self.assertEqual(json.loads(kwargs['input'])['payload'], payload)
        self.assertEqual(json.loads(kwargs['input'])['key'], self.keyfile.read_text().strip())
        self.assertNotIn('wireguard', kwargs['input'])

    def test_http_error_is_not_success(self):
        for status in [401, 403, 404, 500]:
            self.runner.response = {'ok': False, 'status': status, 'body': 'PRIVATE_SETTINGS_BODY'}
            with self.subTest(status=status), self.assertRaises(ControlError) as caught:
                self.client.put('/v1/vpn/status', {'status':'running'}, self.cid)
            self.assertNotIn('PRIVATE_SETTINGS_BODY', str(caught.exception))

    def test_put_rejects_unexpected_json_response_types(self):
        for body in ['[]', 'null', '123', 'true']:
            self.runner.response['body'] = body
            with self.subTest(body=body), self.assertRaises(ControlError):
                self.client.put('/v1/vpn/status', {'status': 'running'}, self.cid)

    def test_timeout_is_bounded(self):
        self.runner.fail = subprocess.TimeoutExpired('PRIVATE_ARG', 8, output='PRIVATE_BODY')
        with self.assertRaises(ControlError) as caught:
            self.client.get('/v1/portforward', self.cid)
        self.assertNotIn('PRIVATE', str(caught.exception))
        for args, kwargs in self.runner.calls:
            self.assertGreater(kwargs['timeout'], 0)
            self.assertLessEqual(kwargs['timeout'], 10)

    def test_generation_change_aborts_request(self):
        for changed in ['b'*64+' 123 2026-11-14T01:02:03Z true\n',
                        self.cid+' 456 2026-11-14T01:02:03Z true\n',
                        self.cid+' 123 2026-11-14T01:04:00Z true\n']:
            with self.subTest(changed=changed):
                self.runner.identity = self.cid+' 123 2026-11-14T01:02:03Z true\n'
                self.runner.after_request = lambda: setattr(self.runner, 'identity', changed)
                with self.assertRaises(ControlError):
                    self.client.put('/v1/vpn/status', {'status':'running'}, self.cid)
        self.runner.identity = 'b'*64+' 456 2026-11-14T01:04:00Z true\n'
        self.runner.calls.clear()
        with self.assertRaises(ControlError):
            self.client.get('/v1/portforward', self.cid)
        self.assertEqual(self.runner.requests(), [])

    def test_lock_busy_or_missing_never_runs_mutation(self):
        with self.paths.compose_lock.open('rb') as holder:
            fcntl.flock(holder, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(ControlError):
                self.client.put('/v1/vpn/status', {'status':'running'}, self.cid)
        self.paths.compose_lock.unlink()
        with self.assertRaises(ControlError):
            self.client.put('/v1/vpn/status', {'status':'running'}, self.cid)
        self.assertEqual(self.runner.requests(), [])
        self.assertEqual(self.client.get('/v1/portforward', self.cid), {'port':12345})

    def test_precondition_rejection_never_mutates(self):
        def reject():
            return False
        def raises():
            raise RuntimeError('PRIVATE_CALLBACK_BODY')
        for callback in [reject, raises]:
            with self.subTest(callback=callback), self.assertRaises(ControlError) as caught:
                self.client.put('/v1/vpn/status', {'status':'running'}, self.cid, precondition=callback)
            self.assertNotIn('PRIVATE', str(caught.exception))
        self.assertEqual(self.runner.requests(), [])

    def test_precondition_runs_with_compose_lock_held(self):
        def check_lock():
            with self.paths.compose_lock.open('rb') as competing:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(competing, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        self.client.put('/v1/vpn/status', {'status':'running'}, self.cid, precondition=check_lock)
        self.assertEqual(len(self.runner.requests()), 1)

    def test_invalid_partial_payload_never_runs_mutation(self):
        for payload in [{}, {'wireguard': {'private_key': 'PRIVATE_KEY'}},
                        {'provider': {'server_selection': {'unknown': True}}},
                        {'provider': {'server_selection': {'countries': 'Canada'}}}]:
            with self.subTest(payload=payload), self.assertRaises(ControlError):
                self.client.put('/v1/vpn/settings', payload, self.cid)
        self.assertEqual(self.runner.requests(), [])

    def test_http_transport_sends_auth_and_omits_error_bodies(self):
        # Substitute only the namespace boundary. The shipped child Python
        # performs real HTTP against a local server on an ephemeral port.
        received = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_PUT(handler):
                received.append((handler.path, handler.headers.get('X-API-Key'),
                                 json.loads(handler.rfile.read(int(handler.headers['Content-Length'])))))
                handler.send_response(200 if len(received) == 1 else 401)
                handler.end_headers()
                handler.wfile.write(b'updated' if len(received) == 1 else b'PRIVATE_ERROR_BODY')

            def log_message(self, *args):
                pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        def local_namespace(args, **kwargs):
            if args[0] == 'docker':
                return self.runner(args, **kwargs)
            prefix = ("import http.client\n_original = http.client.HTTPConnection\n"
                      "http.client.HTTPConnection = lambda host, port, **kw: "
                      f"_original(host, {server.server_port}, **kw)\n")
            return subprocess.run([sys.executable, '-c', prefix + args[-1]], **kwargs)
        client = ControlClient(self.paths, local_namespace)
        payload = {'provider': {'server_selection': {'countries': ['Canada']}}}
        self.assertEqual(client.put('/v1/vpn/settings', payload, self.cid), 'updated')
        self.assertEqual(received[0], ('/v1/vpn/settings', self.keyfile.read_text().strip(), payload))
        with self.assertRaises(ControlError) as caught:
            client.put('/v1/vpn/settings', payload, self.cid)
        self.assertNotIn('PRIVATE_ERROR_BODY', str(caught.exception))

    def test_redaction(self):
        key = self.keyfile.read_text().strip()
        self.runner.fail = RuntimeError('PRIVATE_SETTINGS_BODY '+key)
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured), contextlib.redirect_stderr(captured):
            with self.assertRaises(ControlError) as caught:
                self.client.get('/v1/vpn/settings', self.cid)
        self.assertNotIn(key, str(caught.exception)+captured.getvalue())
        self.assertNotIn('PRIVATE_SETTINGS_BODY', str(caught.exception)+captured.getvalue())


if __name__ == '__main__':
    unittest.main()
