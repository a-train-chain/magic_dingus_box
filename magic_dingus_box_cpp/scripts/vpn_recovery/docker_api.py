"""In-process answers for the controller's fixed Docker identity queries.

The controller polls every 10 s and its fences re-check container identity
many times per tick. Each `docker inspect`/`docker ps` was a separate Go CLI
process (~20 per tick, ~10% of a Pi 5 core with the daemon work they cause).
DockerRunner is a drop-in subprocess.run replacement: it serves exactly the
argv shapes below from the Engine API unix socket and returns the same stdout
text the CLI would print; every other command (and an unreachable or
unauthorized socket) goes to the real subprocess.run unchanged.

Only identity/state fields are formatted. Inspect bodies (which include the
container environment) never leave this module, are never logged and are
never placed in an exception.
"""
import http.client
import json
import os
import shutil
import socket
import subprocess
import time
from urllib.parse import quote, urlencode

DOCKER_SOCKET = '/var/run/docker.sock'
MAX_BODY = 4*1024*1024
# The socket answers only for the CLI the box actually runs. A `docker` shim
# earlier on PATH (tests, rehearsal harnesses) or an explicit DOCKER_HOST /
# DOCKER_CONTEXT means the CLI targets something else: run it unchanged.
SYSTEM_CLI_DIRS = ('/usr/bin', '/bin', '/usr/local/bin')

_BOOL = {True: 'true', False: 'false'}


def _state(data, key):
    value = (data.get('State') or {})[key]
    return _BOOL[value] if isinstance(value, bool) else str(value)


# Exact --format strings used by runtime.py and control.py, field by field.
_FIELDS = {
    '{{.Id}}': lambda d: d['Id'],
    '{{.Name}}': lambda d: d['Name'],
    '{{.State.Pid}}': lambda d: _state(d, 'Pid'),
    '{{.State.StartedAt}}': lambda d: _state(d, 'StartedAt'),
    '{{.State.Running}}': lambda d: _state(d, 'Running'),
    '{{.State.OOMKilled}}': lambda d: _state(d, 'OOMKilled'),
    '{{.State.Status}}': lambda d: _state(d, 'Status'),
}
PS_FORMAT = '{{.Names}} {{.State}} {{.Status}}'


class _Unavailable(Exception):
    """Socket missing/refused/forbidden: use the CLI instead."""


class _UnixConnection(http.client.HTTPConnection):
    def __init__(self, path, timeout):
        super().__init__('localhost', timeout=timeout)
        self._path = path

    def connect(self):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(self.timeout)
        try:
            sock.connect(self._path)
        except BaseException:
            sock.close()
            raise
        self.sock = sock


def _inspect_formatter(fmt):
    tokens = fmt.split(' ')
    if not tokens or any(token not in _FIELDS for token in tokens):
        return None
    return lambda data: ' '.join(_FIELDS[token](data) for token in tokens)


class DockerRunner:
    def __init__(self, socket_path=DOCKER_SOCKET, fallback=subprocess.run, clock=time.monotonic):
        self.socket_path, self.fallback, self.clock = socket_path, fallback, clock

    @staticmethod
    def _system_cli():
        if os.environ.get('DOCKER_HOST') or os.environ.get('DOCKER_CONTEXT'):
            return False
        found = shutil.which('docker')
        return bool(found) and os.path.dirname(found) in SYSTEM_CLI_DIRS

    def _match(self, args):
        if not isinstance(args, list) or len(args) < 5 or args[0] != 'docker' or not self._system_cli():
            return None
        if args[1] == 'inspect' and args[2] == '--format':
            formatter = _inspect_formatter(args[3])
            names = args[4:]
            if formatter and names and all(isinstance(n, str) and n and not n.startswith('-') for n in names):
                return lambda deadline: self._inspect(formatter, names, deadline)
        if args[1:] == ['ps', '-a', '--filter', args[4], '--format', PS_FORMAT] if len(args) == 7 else False:
            key, _, value = args[4].partition('=')
            if key == 'name' and value:
                return lambda deadline: self._ps(value, deadline)
        return None

    def _get(self, path, deadline):
        remaining = deadline-self.clock()
        if remaining <= 0:
            raise subprocess.TimeoutExpired('docker', 0)
        connection = _UnixConnection(self.socket_path, remaining)
        try:
            try:
                connection.connect()
            except (FileNotFoundError, ConnectionRefusedError, PermissionError) as exc:
                raise _Unavailable() from exc
            connection.request('GET', path)
            response = connection.getresponse()
            body = response.read(MAX_BODY+1)
            if len(body) > MAX_BODY:
                raise ValueError('oversized Docker response')
            if self.clock() > deadline:
                raise subprocess.TimeoutExpired('docker', 0)
            return response.status, body
        except socket.timeout:
            raise subprocess.TimeoutExpired('docker', 0) from None
        finally:
            connection.close()

    def _inspect(self, formatter, names, deadline):
        lines, code = [], 0
        for name in names:
            status, body = self._get('/containers/'+quote(name, safe='')+'/json', deadline)
            if status == 404:
                code = 1  # the CLI prints found objects and exits 1
                continue
            if status != 200:
                raise _Unavailable()
            lines.append(formatter(json.loads(body)))
        return ''.join(line+'\n' for line in lines), code

    def _ps(self, name_filter, deadline):
        query = urlencode({'all': '1', 'filters': json.dumps({'name': [name_filter]})})
        status, body = self._get('/containers/json?'+query, deadline)
        if status != 200:
            raise _Unavailable()
        lines = []
        for entry in json.loads(body):
            names = [n[1:] if n.startswith('/') else n for n in entry.get('Names') or []]
            names = [n for n in names if '/' not in n]  # the CLI hides link aliases
            lines.append(' '.join([','.join(names), entry['State'], entry['Status']]))
        return ''.join(line+'\n' for line in lines), 0

    def __call__(self, args, *, input=None, capture_output=False, text=False, timeout=None, check=False, **kwargs):
        handler = None if input is not None or kwargs else self._match(args)
        if handler is None or not capture_output:
            return self.fallback(args, input=input, capture_output=capture_output, text=text,
                                 timeout=timeout, check=check, **kwargs)
        start = self.clock()
        deadline = start+(timeout if timeout is not None else 30)
        try:
            stdout, code = handler(deadline)
        except _Unavailable:
            remaining = None if timeout is None else deadline-self.clock()
            if remaining is not None and remaining <= 0:
                raise subprocess.TimeoutExpired(args, timeout) from None
            return self.fallback(args, input=input, capture_output=capture_output, text=text,
                                 timeout=remaining, check=check)
        except subprocess.TimeoutExpired:
            raise subprocess.TimeoutExpired(args, timeout) from None
        except Exception:
            # Malformed daemon data: report a failed command, like the CLI.
            stdout, code = '', 1
        if check and code:
            raise subprocess.CalledProcessError(code, args)
        out, err = (stdout, '') if text else (stdout.encode(), b'')
        return subprocess.CompletedProcess(args, code, out, err)
