"""Verified port sync: injected I/O only; never touches production paths."""
import copy
import fcntl
import importlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from contextlib import redirect_stdout, redirect_stderr
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vpn_recovery.common import Paths
from vpn_recovery.control import ControlClient, ControlError
try:
    port_sync = importlib.import_module('vpn_recovery.port_sync')
except ImportError:
    port_sync = None

GEN = {'container_id': 'a'*64, 'pid': 42, 'started_at': '2026-11-14T00:00:00Z', 'tunnel_ifindex': 2}


class FakeControl:
    def __init__(self):
        self.generation = copy.deepcopy(GEN)
        self.port = 4567
        self.on_port = None
        self.reads = 0

    def observe_generation(self, container_id=None, *, deadline=None):
        return copy.deepcopy(self.generation)

    def get(self, path, container_id, *, deadline=None):
        assert path == '/v1/portforward'
        self.reads += 1
        if self.on_port:
            self.on_port(self)
        return {'port': self.port}


class FakeQbit:
    def __init__(self):
        self.prefs = {'listen_port': 1234, 'upnp': False, 'random_port': False}
        self.calls = []
        self.on_request = None
        self.fail = None
        self.ignore_write = False

    def request(self, method, path, payload=None, *, timeout=5):
        self.calls.append((method, path, payload, timeout))
        if self.on_request:
            self.on_request(method, path)
        if self.fail:
            if isinstance(self.fail, Exception):
                raise self.fail
            return self.fail
        if method == 'POST':
            if not self.ignore_write:
                self.prefs.update(payload)
            return 200, ''
        return 200, json.dumps(self.prefs)


class PortSyncTests(unittest.TestCase):
    def test_intent_changes_while_preferences_read_blocks_subsequent_post(self):
        control,qbit=FakeControl(),FakeQbit()
        allowed=[True]
        qbit.on_request=lambda *_: allowed.__setitem__(0,False)
        result=port_sync.PortSynchronizer(self.paths,control,qbit).sync(precondition=lambda:allowed[0])
        self.assertEqual(result['status'],'deferred')
        self.assertFalse(any(c[0]=='POST' for c in qbit.calls))

    def test_under_lock_intent_precondition_blocks_write(self):
        control, qbit = FakeControl(), FakeQbit()
        sync = port_sync.PortSynchronizer(self.paths, control, qbit)
        result = sync.sync(precondition=lambda: False)
        self.assertEqual(result['status'], 'deferred')
        self.assertFalse(any(call[0] == 'POST' for call in qbit.calls))

    def setUp(self):
        self.assertIsNotNone(port_sync, 'verified port synchronizer is not implemented')
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.paths = Paths(root, root/'services', root/'config', root/'state', root/'run', root/'compose.lock')
        self.paths.compose_lock.touch()
        self.control, self.qbit = FakeControl(), FakeQbit()
        self.sync = port_sync.PortSynchronizer(self.paths, self.control, self.qbit)

    def test_current_correct_port_is_read_only(self):
        self.qbit.prefs['listen_port'] = self.control.port
        result = self.sync.sync()
        self.assertEqual(result['status'], 'synced')
        self.assertEqual(result['generation'], GEN)
        self.assertEqual(result['qbit_port'], 4567)
        self.assertTrue(result['checked_at'])
        self.assertEqual([c[0] for c in self.qbit.calls], ['GET'])

    def test_wrong_port_writes_then_verifies_readback(self):
        result = self.sync.sync(expected_generation=GEN)
        self.assertEqual(result['status'], 'synced')
        self.assertEqual([c[:2] for c in self.qbit.calls], [
            ('GET', '/api/v2/app/preferences'), ('POST', '/api/v2/app/setPreferences'),
            ('GET', '/api/v2/app/preferences')])
        self.assertEqual(self.qbit.calls[1][2], {'listen_port': 4567, 'upnp': False, 'random_port': False})
        self.assertTrue(all(0 < c[3] <= 5 for c in self.qbit.calls))

    def test_upnp_and_random_port_are_corrected_even_when_port_matches(self):
        for flag in ('upnp', 'random_port'):
            with self.subTest(flag=flag):
                self.qbit.prefs.update(listen_port=4567, upnp=False, random_port=False)
                self.qbit.prefs[flag] = True
                self.assertEqual(self.sync.sync()['status'], 'synced')
                self.assertFalse(self.qbit.prefs[flag])

    def test_failed_http_invalid_json_and_timeout_never_report_synced(self):
        for failure in [(403, 'secret-password'), (500, 'secret-password'), (200, 'not-json'),
                        (200, '[]'), TimeoutError('secret-password')]:
            with self.subTest(failure=repr(failure)):
                self.qbit.fail = failure
                out = io.StringIO()
                with redirect_stdout(out), redirect_stderr(out):
                    result = self.sync.sync()
                self.assertEqual(result['status'], 'failed')
                self.assertNotIn('secret-password', json.dumps(result)+out.getvalue())

    def test_write_http_failure_and_mismatched_readback_fail(self):
        def fail_write(method, path):
            if method == 'POST':
                self.qbit.fail = (403, 'credential')
        self.qbit.on_request = fail_write
        self.assertEqual(self.sync.sync()['status'], 'failed')
        self.qbit.fail, self.qbit.on_request = None, None
        self.qbit.ignore_write = True
        self.assertEqual(self.sync.sync()['status'], 'failed')

    def test_generation_or_port_change_before_write_invalidates_without_write(self):
        for change in ('container_id', 'tunnel_ifindex', 'port'):
            with self.subTest(change=change):
                self.set_control_original()
                def change_on_get(method, path):
                    if method == 'GET':
                        if change == 'port':
                            self.control.port = 9876
                        else:
                            self.control.generation[change] = 'b'*64 if change == 'container_id' else 3
                self.qbit.on_request = change_on_get
                self.assertEqual(self.sync.sync()['status'], 'changed')
                self.assertFalse(any(c[0] == 'POST' for c in self.qbit.calls))

    def set_control_original(self):
        self.control.generation = copy.deepcopy(GEN)
        self.control.port = 4567
        self.qbit.calls.clear()
        self.qbit.prefs['listen_port'] = 1234

    def test_generation_or_port_change_after_write_never_reports_synced(self):
        for change in ('tunnel_ifindex', 'port'):
            self.set_control_original()
            def change_on_write(method, path):
                if method == 'POST':
                    if change == 'port':
                        self.control.port += 1
                    else:
                        self.control.generation['tunnel_ifindex'] += 1
            self.qbit.on_request = change_on_write
            self.assertEqual(self.sync.sync()['status'], 'changed')

    def test_generation_change_during_forwarded_port_read_is_changed(self):
        self.control.on_port = lambda c: c.generation.update(tunnel_ifindex=c.generation['tunnel_ifindex']+1)
        self.assertEqual(self.sync.sync()['status'], 'changed')
        self.assertFalse(self.qbit.calls)

    def test_name_replacement_during_final_port_read_rejects_still_running_old_container(self):
        # Recreate can briefly leave the former container alive under another
        # name. An explicit old-ID inspection cannot prove current ownership.
        current = [copy.deepcopy(GEN)]
        def generation(container_id=None, *, deadline=None):
            return copy.deepcopy(GEN if container_id == GEN['container_id'] else current[0])
        self.control.observe_generation = generation
        def replace_on_final_read(control):
            if control.reads == 3:
                current[0] = dict(GEN, container_id='b'*64)
        self.control.on_port = replace_on_final_read
        self.assertEqual(self.sync.sync()['status'], 'changed')

    def test_expected_generation_is_required_to_match(self):
        expected = dict(GEN, tunnel_ifindex=10)
        self.assertEqual(self.sync.sync(expected)['status'], 'changed')
        self.assertFalse(self.qbit.calls)

    def test_malformed_ports_rejected_and_zero_is_no_port(self):
        for value in (-1, 65536, '4567', True, 1.5, None):
            self.control.port = value
            self.assertEqual(self.sync.sync()['status'], 'unavailable')
        self.control.port = 0
        self.assertEqual(self.sync.sync()['status'], 'no_port')
        self.assertFalse(self.qbit.calls)
        self.control.port = 4567
        for value in (-1, 65536, '1234', True, None):
            self.qbit.prefs['listen_port'] = value
            self.assertEqual(self.sync.sync()['status'], 'failed')

    def test_observe_is_read_only_and_vpn_survives_qbit_failure(self):
        self.qbit.fail = (403, 'password')
        result = self.sync.observe()
        self.assertEqual(result['forwarded_port'], 4567)
        self.assertEqual(result['generation'], GEN)
        self.assertEqual(result['status'], 'failed')
        self.assertFalse(any(c[0] == 'POST' for c in self.qbit.calls))
        self.qbit.fail = None
        self.assertEqual(self.sync.observe()['status'], 'changed')
        self.assertFalse(any(c[0] == 'POST' for c in self.qbit.calls))

    def test_busy_or_missing_lock_defers_without_io(self):
        with self.paths.compose_lock.open('rb') as holder:
            fcntl.flock(holder, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertEqual(self.sync.sync()['status'], 'deferred')
        self.paths.compose_lock.unlink()
        self.assertEqual(self.sync.sync()['status'], 'deferred')
        self.assertEqual(self.control.reads, 0)
        self.assertFalse(self.qbit.calls)

    def test_aggregate_deadline_exhaustion_never_claims_synced(self):
        # Each fake command uses <5s; their sum crosses the whole-operation
        # budget. Disabling the final aggregate fence must fail this test.
        clock = [100.0]
        original = self.control.observe_generation
        def slow_generation(container_id=None, *, deadline=None):
            clock[0] += 2
            return original(container_id, deadline=deadline)
        self.control.observe_generation = slow_generation
        self.qbit.prefs['listen_port'] = 4567
        self.qbit.on_request = lambda *args: clock.__setitem__(0, clock[0]+2)
        with patch('vpn_recovery.port_sync.time.monotonic', side_effect=lambda: clock[0]):
            result = self.sync.observe()
        self.assertNotEqual(result['status'], 'synced')
        self.assertFalse(any(call[0] == 'POST' for call in self.qbit.calls))

    def test_caller_deadline_and_default_budgets_reach_every_vpn_read(self):
        clock = 100.0
        observed = []
        original = self.control.observe_generation
        def record_generation(container_id=None, *, deadline=None):
            observed.append(deadline)
            return original(container_id, deadline=deadline)
        self.control.observe_generation = record_generation
        with patch('vpn_recovery.port_sync.time.monotonic', return_value=clock):
            self.sync.observe()
            self.assertEqual(set(observed), {110.0})
            observed.clear()
            self.sync.sync()
            self.assertEqual(set(observed), {130.0})
            observed.clear()
            self.sync.sync(deadline=103.0)
            self.assertEqual(set(observed), {103.0})

    def test_port_synchronizer_has_no_torrent_operations(self):
        self.assertEqual(self.sync.sync()['status'], 'synced')
        self.assertTrue(all(c[1] in ('/api/v2/app/preferences', '/api/v2/app/setPreferences') for c in self.qbit.calls))


class GenerationTests(unittest.TestCase):
    def test_generation_includes_same_container_tunnel_identity(self):
        calls = []
        def runner(args, **kwargs):
            calls.append((args, kwargs))
            output = (GEN['container_id']+' 42 '+GEN['started_at']+' true\n' if args[1] == 'inspect'
                      else '2: tun0: <POINTOPOINT,UP,LOWER_UP> mtu 1420 state UNKNOWN\n')
            return SimpleNamespace(returncode=0, stdout=output)
        client = ControlClient(Paths(base='/nonexistent-test-only'), runner=runner)
        self.assertTrue(hasattr(client, 'observe_generation'), 'canonical generation observation is not implemented')
        self.assertEqual(client.observe_generation(), GEN)
        self.assertTrue(all(0 < kw['timeout'] <= 3 for _, kw in calls))
        self.assertTrue(any('tun0' in args for args, _ in calls))

    def test_missing_tunnel_and_container_replacement_are_rejected(self):
        for replacement in (False, True):
            count = 0
            def runner(args, **kwargs):
                nonlocal count
                count += 1
                if args[1] == 'inspect':
                    output = ('b'*64 if replacement and count > 1 else 'a'*64)+' 42 now true'
                else:
                    output = '2: tun0: <UP>' if replacement else ''
                return SimpleNamespace(returncode=0, stdout=output)
            client = ControlClient(Paths(base='/nonexistent-test-only'), runner=runner)
            self.assertTrue(hasattr(client, 'observe_generation'), 'canonical generation observation is not implemented')
            with self.assertRaises(ControlError):
                client.observe_generation()

class QbitClientTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(port_sync, 'cookie-authenticated qBit transport is not implemented')
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.env = Path(self.temp.name)/'.env'
        self.env.write_text('MDB_QBIT_PASS=password-with-$-and-&\n')

    def test_real_http_cookie_login_form_encoding_and_non_2xx_suppression(self):
        import http.server
        import threading
        import urllib.parse
        requests = []
        responses = {'status': 200}
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_POST(self):
                data = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
                requests.append((self.path, urllib.parse.parse_qs(data), self.headers.get('Cookie')))
                self.send_response(200)
                if self.path.endswith('/login'):
                    self.send_header('Set-Cookie', 'SID=sample-secret-cookie; Path=/')
                self.end_headers()
                self.wfile.write(b'Ok.' if self.path.endswith('/login') else b'')
            def do_GET(self):
                requests.append((self.path, {}, self.headers.get('Cookie')))
                self.send_response(responses['status'])
                self.end_headers()
                self.wfile.write(b'password-with-$-and-&' if responses['status'] == 403 else b'{"listen_port":4567}')
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        client = port_sync.QbitClient(self.env, base_url='http://127.0.0.1:'+str(server.server_port))
        self.assertEqual(client.request('GET', '/api/v2/app/preferences'), (200, '{"listen_port":4567}'))
        self.assertEqual(requests[0][1]['password'], ['password-with-$-and-&'])
        self.assertEqual(requests[1][2], 'SID=sample-secret-cookie')
        self.assertEqual(client.request('POST', '/api/v2/app/setPreferences', {'listen_port':4567, 'upnp':False, 'random_port':False}), (200, ''))
        self.assertEqual(json.loads(requests[-1][1]['json'][0]), {'listen_port':4567, 'upnp':False, 'random_port':False})
        responses['status'] = 403
        with self.assertRaisesRegex(Exception, '^qBit request failed or timed out$'):
            client.request('GET', '/api/v2/app/preferences')
        responses['status'] = 200
        # curl writes session cookies as expiry 0, unlike Python's Netscape
        # importer. The shell's existing session must survive the handoff.
        cookie_file = self.env.parent/'curl-cookie'
        cookie_file.write_text('# Netscape HTTP Cookie File\n#HttpOnly_127.0.0.1\tFALSE\t/\tFALSE\t0\tSID\tshell-session-cookie\n')
        client = port_sync.QbitClient(self.env.parent/'missing-env', cookie_file=cookie_file,
                                      base_url='http://127.0.0.1:'+str(server.server_port))
        before = len(requests)
        self.assertEqual(client.request('GET', '/api/v2/app/preferences')[0], 200)
        self.assertEqual(len(requests), before+1)  # no duplicate login
        self.assertEqual(requests[-1][2], 'SID=shell-session-cookie')
        with self.assertRaisesRegex(Exception, '^qBit route not allowed$'):
            client.request('GET', '/api/v2/denied')

    def test_transport_error_is_sanitized_bounded_and_credentials_not_in_argv(self):
        calls = []
        def runner(args, **kwargs):
            calls.append((args, kwargs))
            raise subprocess.TimeoutExpired(args, 5, output='password-with-$-and-&')
        client = port_sync.QbitClient(self.env, runner=runner)
        with self.assertRaisesRegex(Exception, '^qBit request failed or timed out$'):
            client.request('GET', '/api/v2/app/preferences')
        self.assertLessEqual(calls[0][1]['timeout'], 5)
        self.assertNotIn('password-with-$-and-&', repr(calls[0][0]))

    def test_expired_deadline_performs_no_control_command(self):
        import time
        calls = []
        client = ControlClient(Paths(base='/nonexistent-test-only'), runner=lambda *a, **k: calls.append(a))
        with self.assertRaises(ControlError):
            client.get('/v1/portforward', GEN['container_id'], deadline=time.monotonic()-1)
        self.assertFalse(calls)


if __name__ == '__main__':
    unittest.main()
