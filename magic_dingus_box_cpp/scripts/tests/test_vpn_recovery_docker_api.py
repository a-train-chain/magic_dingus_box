"""DockerRunner: CLI-identical stdout from a real unix socket, CLI otherwise."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, unquote, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vpn_recovery.docker_api import DockerRunner, PS_FORMAT
from vpn_recovery.control import ControlClient
from vpn_recovery.common import Paths

GLUETUN = {'Id': 'a'*64, 'Name': '/mdb_gluetun',
           'State': {'Pid': 42, 'StartedAt': '2026-11-15T09:43:28.926046613Z', 'Running': True,
                     'OOMKilled': False, 'Status': 'running'},
           'Config': {'Env': ['WIREGUARD_PRIVATE_KEY=never-leaves-the-module']}}
QBIT = {'Id': 'b'*64, 'Name': '/mdb_qbittorrent',
        'State': {'Pid': 77, 'StartedAt': '2026-11-15T09:43:40Z', 'Running': True, 'OOMKilled': False,
                  'Status': 'running'}}
PS = [{'Names': ['/mdb_radarr'], 'State': 'running', 'Status': 'Up 7 minutes (healthy)'},
      {'Names': ['/mdb_qbittorrent', '/other/alias'], 'State': 'exited', 'Status': 'Exited (137) 2 minutes ago'}]


class FakeDaemon:
    """Minimal HTTP/1.1 Engine API over AF_UNIX; records request paths."""
    def __init__(self, delay=0, broken=False):
        self.dir = tempfile.mkdtemp(prefix='mdbd')
        self.path = os.path.join(self.dir, 'd.sock')
        self.delay, self.broken, self.requests = delay, broken, []
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(self.path); self.server.listen(8)
        self.thread = threading.Thread(target=self.serve, daemon=True); self.thread.start()

    def serve(self):
        while True:
            try: conn, _ = self.server.accept()
            except OSError: return
            with conn:
                data = b''
                while b'\r\n\r\n' not in data:
                    chunk = conn.recv(4096)
                    if not chunk: break
                    data += chunk
                target = data.split(b' ')[1].decode() if data else ''
                self.requests.append(target)
                time.sleep(self.delay)
                status, body = self.route(target)
                if self.broken: body = b'{not json'
                conn.sendall(b'HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n'
                             % (status, len(body)) + body)

    def route(self, target):
        url = urlsplit(target)
        if url.path.startswith('/containers/') and url.path.endswith('/json') and url.path != '/containers/json':
            name = unquote(url.path[len('/containers/'):-len('/json')])
            found = {'mdb_gluetun': GLUETUN, 'a'*64: GLUETUN, 'mdb_qbittorrent': QBIT}.get(name)
            return (200, json.dumps(found).encode()) if found else (404, b'{"message":"No such container"}')
        if url.path == '/containers/json':
            query = parse_qs(url.query)
            assert query['all'] == ['1'] and json.loads(query['filters'][0]) == {'name': ['mdb_']}
            return 200, json.dumps(PS).encode()
        return 500, b'{}'

    def close(self):
        self.server.close()
        try: os.unlink(self.path)
        except FileNotFoundError: pass
        os.rmdir(self.dir)


class DockerRunnerTests(unittest.TestCase):
    def setUp(self):
        patcher = patch.object(DockerRunner, '_system_cli', staticmethod(lambda: True))
        patcher.start(); self.addCleanup(patcher.stop)
        self.daemon = FakeDaemon(); self.addCleanup(self.daemon.close)
        self.cli = []
        def fallback(args, **kwargs):
            self.cli.append(args)
            return subprocess.CompletedProcess(args, 0, 'cli\n', '')
        self.runner = DockerRunner(socket_path=self.daemon.path, fallback=fallback)

    def run_(self, args, timeout=3):
        return self.runner(args, capture_output=True, text=True, timeout=timeout, check=False)

    def test_identity_formats_match_cli_text(self):
        full = self.run_(['docker', 'inspect', '--format',
                          '{{.Id}} {{.State.Pid}} {{.State.StartedAt}} {{.State.Running}} {{.State.OOMKilled}} {{.State.Status}}', 'mdb_gluetun'])
        self.assertEqual((full.returncode, full.stdout),
                         (0, 'a'*64+' 42 2026-11-15T09:43:28.926046613Z true false running\n'))
        by_id = self.run_(['docker', 'inspect', '--format', '{{.Id}} {{.State.Pid}} {{.State.StartedAt}} {{.State.Running}}', 'a'*64])
        self.assertEqual(by_id.stdout, 'a'*64+' 42 2026-11-15T09:43:28.926046613Z true\n')
        multi = self.run_(['docker', 'inspect', '--format', '{{.Name}} {{.Id}} {{.State.Pid}} {{.State.StartedAt}} {{.State.Running}}',
                           'mdb_gluetun', 'mdb_qbittorrent'])
        self.assertEqual(multi.stdout.splitlines(), ['/mdb_gluetun '+'a'*64+' 42 2026-11-15T09:43:28.926046613Z true',
                                                     '/mdb_qbittorrent '+'b'*64+' 77 2026-11-15T09:43:40Z true'])
        self.assertEqual(self.cli, [])
        self.assertNotIn('never-leaves', full.stdout+multi.stdout)

    def test_missing_container_exits_one_like_cli(self):
        result = self.run_(['docker', 'inspect', '--format', '{{.Id}} {{.State.Pid}}', 'mdb_gluetun', 'mdb_absent'])
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.cli, [])

    def test_ps_names_states_and_status(self):
        result = self.run_(['docker', 'ps', '-a', '--filter', 'name=mdb_', '--format', PS_FORMAT])
        self.assertEqual(result.stdout.splitlines(), ['mdb_radarr running Up 7 minutes (healthy)',
                                                      'mdb_qbittorrent exited Exited (137) 2 minutes ago'])

    def test_unrecognized_commands_and_options_use_the_cli(self):
        for args in (['docker', 'inspect', '--format', '{{json .Config}}', 'mdb_gluetun'],
                     ['docker', 'inspect', '--format', '{{.Id}}', '--size', 'mdb_gluetun'],
                     ['docker', 'exec', 'a'*64, 'ip', '-o', 'link'],
                     ['docker', 'ps', '-a', '--filter', 'status=running', '--format', PS_FORMAT],
                     ['docker', 'events', '--since', '1', '--until', '2'],
                     ['nsenter', '--target', '42', '--net', 'ip']):
            with self.subTest(args=args):
                self.assertEqual(self.run_(args).stdout, 'cli\n')
        self.assertEqual(len(self.cli), 6)
        self.assertEqual(self.daemon.requests, [])
        self.runner(['docker', 'inspect', '--format', '{{.Id}}', 'mdb_gluetun'], input='x', capture_output=True, text=True)
        self.assertEqual(len(self.cli), 7)

    def test_missing_socket_falls_back_to_cli(self):
        runner = DockerRunner(socket_path=self.daemon.path+'.absent', fallback=self.runner.fallback)
        result = runner(['docker', 'inspect', '--format', '{{.Id}}', 'mdb_gluetun'], capture_output=True, text=True, timeout=3)
        self.assertEqual((result.stdout, len(self.cli)), ('cli\n', 1))

    def test_slow_daemon_is_a_timeout_not_a_second_wait(self):
        slow = FakeDaemon(delay=.5); self.addCleanup(slow.close)
        runner = DockerRunner(socket_path=slow.path, fallback=self.runner.fallback)
        start = time.monotonic()
        with self.assertRaises(subprocess.TimeoutExpired) as raised:
            runner(['docker', 'inspect', '--format', '{{.Id}}', 'mdb_gluetun'], capture_output=True, text=True, timeout=.15)
        self.assertLess(time.monotonic()-start, .45)
        self.assertEqual(self.cli, [])
        self.assertNotIn('never-leaves', str(raised.exception))

    def test_malformed_daemon_data_is_a_failed_command(self):
        broken = FakeDaemon(broken=True); self.addCleanup(broken.close)
        runner = DockerRunner(socket_path=broken.path, fallback=self.runner.fallback)
        result = runner(['docker', 'inspect', '--format', '{{.Id}}', 'mdb_gluetun'], capture_output=True, text=True, timeout=3)
        self.assertEqual((result.returncode, result.stdout), (1, ''))

    def test_path_shim_or_docker_host_keeps_the_cli(self):
        patch.stopall()
        with tempfile.TemporaryDirectory() as shim:
            fake = Path(shim)/'docker'; fake.write_text('#!/bin/sh\n'); fake.chmod(0o755)
            with patch.dict(os.environ, {'PATH': shim+os.pathsep+os.environ.get('PATH', '')}):
                self.assertFalse(DockerRunner._system_cli())
        with patch('shutil.which', return_value='/usr/bin/docker'):
            self.assertTrue(DockerRunner._system_cli())
            with patch.dict(os.environ, {'DOCKER_HOST': 'tcp://elsewhere:2375'}):
                self.assertFalse(DockerRunner._system_cli())


class HostNamespaceTunnelTests(unittest.TestCase):
    def test_root_reads_tun0_from_inspected_pid_namespace(self):
        calls = []
        def runner(args, **kwargs):
            calls.append(args)
            if args[:2] == ['docker', 'inspect']:
                return subprocess.CompletedProcess(args, 0, 'a'*64+' 42 start true\n', '')
            return subprocess.CompletedProcess(args, 0, '7: tun0: <POINTOPOINT,NOARP,UP,LOWER_UP> mtu 1420\\    link/none \n', '')
        client = ControlClient(Paths(base='/nonexistent-test-only'), runner, host_namespace_reads=True)
        generation = client.observe_generation(deadline=time.monotonic()+5)
        self.assertEqual(generation['tunnel_ifindex'], 7)
        self.assertIn(['nsenter', '--target', '42', '--net', 'ip', '-o', 'link', 'show', 'dev', 'tun0'], calls)
        self.assertFalse(any(c[:2] == ['docker', 'exec'] for c in calls))
        # Same inspect -> read -> inspect fence as the docker exec path.
        self.assertEqual([c[1] for c in calls], ['inspect', '--target', 'inspect'])

    def test_unprivileged_caller_keeps_docker_exec(self):
        calls = []
        def runner(args, **kwargs):
            calls.append(args)
            out = 'a'*64+' 42 start true\n' if args[1] == 'inspect' else '7: tun0: <UP>\n'
            return subprocess.CompletedProcess(args, 0, out, '')
        ControlClient(Paths(base='/nonexistent-test-only'), runner, host_namespace_reads=False).observe_generation()
        self.assertTrue(any(c[:2] == ['docker', 'exec'] for c in calls))


if __name__ == '__main__':
    unittest.main()
