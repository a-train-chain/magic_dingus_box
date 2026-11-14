"""Scoped Gluetun v3.41.3 control transport; never log settings or secrets.

The host's Python runs inside the inspected network namespace only. Port
8000 remains unpublished. Secrets and payloads travel over stdin, not argv.
"""
import json
import os
import re
import secrets
import stat
import subprocess
import time
from typing import Callable, Optional, Union

from .common import Paths, atomic_write, bounded_lock

PUBLIC_READS = ('/v1/portforward', '/v1/publicip/ip', '/v1/openvpn/portforwarded')
PRIVATE_ROUTES = ('/v1/vpn/settings', '/v1/vpn/status')
INSPECT_TIMEOUT_S = 3
REQUEST_TIMEOUT_S = 8
LOCK_TIMEOUT_S = .25


class ControlError(RuntimeError):
    """A sanitized failure suitable for status/error logs."""


def _auth_dir(paths):
    return paths.services/'config/gluetun/auth'


def _role_content(key):
    public = ', '.join(json.dumps('GET '+p) for p in PUBLIC_READS)
    private = ', '.join(json.dumps(method+' '+p) for p in PRIVATE_ROUTES for method in ('GET', 'PUT'))
    return ("# Managed by Magic Dingus Box; takes effect at Gluetun's next start.\n"
            '[[roles]]\nname = "mdb-local-readonly"\n'
            f'routes = [{public}]\nauth = "none"\n\n'
            '[[roles]]\nname = "mdb-vpn-recovery"\n'
            f'routes = [{private}]\nauth = "apikey"\napikey = "{key}"\n').encode()


def ensure_control_credentials(paths: Paths) -> bool:
    """Install or repair a per-box key and role; preserve an existing valid key.

    No Docker operations, restart or activation. The private lock serializes
    concurrent provisioners so key/config never acquire different credentials.
    """
    directory = _auth_dir(paths)
    directory.mkdir(parents=True, exist_ok=True)
    changed = stat.S_IMODE(directory.stat().st_mode) != 0o700
    directory.chmod(0o700)
    lock_path = directory/'.install.lock'
    fd = os.open(lock_path, os.O_CREAT | os.O_RDONLY, 0o600)
    os.close(fd)
    with bounded_lock(lock_path, 1):
        key_path = directory/'recovery.key'
        try:
            key = key_path.read_text().strip()
        except FileNotFoundError:
            key = secrets.token_hex(32)
            atomic_write(key_path, (key+'\n').encode())
            changed = True
        if not re.fullmatch(r'[0-9a-f]{64}', key):
            # Corruption is not permission for an implicit credential rotation.
            raise ControlError('invalid control credential')
        content = _role_content(key)
        config = directory/'config.toml'
        if not config.exists() or config.read_bytes() != content:
            atomic_write(config, content)
            changed = True
        for path in (key_path, config, lock_path):
            if stat.S_IMODE(path.stat().st_mode) != 0o600:
                path.chmod(0o600)
                changed = True
        # Match the services owner as the old installer did. Root provisioners
        # must not strand the operator's config tree with inaccessible files.
        owner = paths.services.stat()
        if os.geteuid() == 0:
            for path in (directory, key_path, config, lock_path):
                os.chown(path, owner.st_uid, owner.st_gid)
    return changed


# Runs using host Python after entering only the Gluetun network namespace.
# Keep the HTTP destination fixed: this is deliberately not a general proxy.
_HTTP_SCRIPT = r'''
import http.client, json, sys
try:
    request = json.load(sys.stdin)
    connection = http.client.HTTPConnection('127.0.0.1', 8000, timeout=5)
    headers = {'Content-Type': 'application/json'}
    if request['key']:
        headers['X-API-Key'] = request['key']
    body = None if request['payload'] is None else json.dumps(request['payload'])
    connection.request(request['method'], request['path'], body=body, headers=headers)
    response = connection.getresponse()
    # Never forward error bodies, which can include submitted settings.
    if 200 <= response.status < 300:
        body = response.read(1048577)
        if len(body) > 1048576:
            raise ValueError('oversized response')
        result = {'ok': True, 'status': response.status, 'body': body.decode('utf-8')}
    else:
        result = {'ok': False, 'status': response.status}
    connection.close()
except Exception:
    result = {'ok': False}
sys.stdout.write(json.dumps(result))
'''


class ControlClient:
    """Bounded, generation-fenced requests using a subprocess.run-like runner.

    PUT owns the compose lock. Callers must NOT wrap put() in that same lock;
    use precondition for intent/settings/playback checks under its lock instead.
    GET does not require a compose lock, even while setup/recovery is blocked.
    """
    def __init__(self, paths: Paths, runner=subprocess.run):
        self.paths = paths
        self.runner = runner

    def _run(self, args, *, input=None, timeout, deadline=None):
        try:
            if deadline is not None:
                timeout = min(timeout, deadline-time.monotonic())
                if timeout <= 0:
                    raise ControlError('control deadline exceeded')
            result = self.runner(args, input=input, capture_output=True, text=True,
                                 timeout=timeout, check=False)
            if result.returncode != 0:
                raise ControlError('control command failed')
            return result.stdout
        except Exception:
            # Includes TimeoutExpired/CalledProcessError carrying argv/stdout.
            raise ControlError('control command failed or timed out') from None

    def _identity(self, container_id, *, deadline=None):
        # Inspect only identity fields: a complete docker inspect includes keys.
        output = self._run(['docker', 'inspect', '--format',
                            '{{.Id}} {{.State.Pid}} {{.State.StartedAt}} {{.State.Running}}',
                            container_id], timeout=INSPECT_TIMEOUT_S, deadline=deadline)
        fields = output.split()
        if (len(fields) != 4 or fields[0] != container_id or not fields[1].isdigit()
                or int(fields[1]) <= 0 or fields[3] != 'true'):
            raise ControlError('container generation unavailable or changed')
        return tuple(fields[:3])

    def observe_generation(self, container_id=None, *, deadline=None) -> dict:
        """Canonical tunnel identity, including tun0 replacement in one container.

        Uses inspect → interface index → inspect, each command <=3 seconds.
        None resolves the current mdb_gluetun container. No lock/auth required;
        missing/replaced containers or absent tun0 raise sanitized ControlError.
        Consumers compare the complete JSON-safe dictionary, never just ID.
        """
        if container_id is None:
            output = self._run(['docker', 'inspect', '--format',
                                '{{.Id}} {{.State.Pid}} {{.State.StartedAt}} {{.State.Running}}',
                                'mdb_gluetun'], timeout=INSPECT_TIMEOUT_S, deadline=deadline)
            fields = output.split()
            if (len(fields) != 4 or not re.fullmatch(r'[0-9a-f]{64}', fields[0])
                    or not fields[1].isdigit() or int(fields[1]) <= 0 or fields[3] != 'true'):
                raise ControlError('container generation unavailable or changed')
            container_id, pid, started_at = fields[:3]
            identity = (container_id, pid, started_at)
        else:
            if not isinstance(container_id, str) or not re.fullmatch(r'[0-9a-f]{64}', container_id):
                raise ControlError('invalid container identity')
            identity = self._identity(container_id, deadline=deadline)
        output = self._run(['docker', 'exec', container_id, 'ip', '-o', 'link', 'show', 'dev', 'tun0'],
                           timeout=INSPECT_TIMEOUT_S, deadline=deadline)
        match = re.fullmatch(r'([1-9][0-9]*): tun0(?:@[^: ]+)?:[^\n]*(?:\n)?', output)
        if not match:
            raise ControlError('tunnel generation unavailable')
        if self._identity(container_id, deadline=deadline) != identity:
            raise ControlError('container generation changed')
        return {'container_id': container_id, 'pid': int(identity[1]),
                'started_at': identity[2], 'tunnel_ifindex': int(match[1])}

    def get(self, path, container_id, *, deadline=None) -> dict:
        result = self._request('GET', path, None, container_id, deadline=deadline)
        if not isinstance(result, dict):
            raise ControlError('invalid control JSON response')
        return result

    def put(self, path, payload, container_id, *,
            precondition: Optional[Callable[[], bool]] = None) -> Union[dict, str]:
        """Partial settings/status PUT; reject false/raising precondition safely.

        The callback runs immediately before transport with the compose lock
        held. It must be bounded, read-only, and return exactly True to proceed.
        """
        try:
            with bounded_lock(self.paths.compose_lock, LOCK_TIMEOUT_S):
                result = self._request('PUT', path, payload, container_id, precondition)
                if not isinstance(result, (dict, str)):
                    raise ControlError('invalid control response')
                return result
        except ControlError:
            raise
        except Exception:
            raise ControlError('control mutation deferred') from None

    @staticmethod
    def _validate_payload(path, payload):
        if path == '/v1/vpn/status':
            if (not isinstance(payload, dict) or set(payload) != {'status'}
                    or payload['status'] not in ('running', 'stopped')):
                raise ControlError('invalid VPN status mutation')
            return
        # Send just selection overrides; never round-trip a complete settings
        # GET (it contains WireGuard/OpenVPN secrets and unrelated options).
        try:
            selection = payload['provider']['server_selection']
            valid = (set(payload) == {'provider'} and set(payload['provider']) == {'server_selection'}
                     and isinstance(selection, dict) and bool(selection)
                     and not (selection.keys() - {'countries', 'hostnames'})
                     and all(isinstance(values, list) and len(values) <= 100
                             and all(isinstance(v, str) and 0 < len(v) <= 253
                                     and re.fullmatch(r"[A-Za-z0-9 .'-]+", v) for v in values)
                             for values in selection.values()))
        except (KeyError, TypeError):
            valid = False
        if not valid:
            raise ControlError('invalid partial VPN settings mutation')

    def _request(self, method, path, payload, container_id, precondition=None, *, deadline=None):
        if (method == 'GET' and path not in PUBLIC_READS + PRIVATE_ROUTES
                or method == 'PUT' and path not in PRIVATE_ROUTES):
            raise ControlError('control route not allowed')
        if not isinstance(container_id, str) or not re.fullmatch(r'[0-9a-f]{64}', container_id):
            raise ControlError('invalid container identity')
        if method == 'PUT':
            self._validate_payload(path, payload)
        identity = self._identity(container_id, deadline=deadline)
        try:
            key = (_auth_dir(self.paths)/'recovery.key').read_text().strip() if path in PRIVATE_ROUTES else ''
            if path in PRIVATE_ROUTES and not re.fullmatch(r'[0-9a-f]{64}', key):
                raise ValueError()
            request = json.dumps({'method': method, 'path': path, 'payload': payload, 'key': key})
        except Exception:
            raise ControlError('control credential or payload unavailable') from None
        # Fence the generation again after local preparation, then let the
        # controller recheck intent/revision/playback immediately before I/O.
        if self._identity(container_id, deadline=deadline) != identity:
            raise ControlError('container generation changed')
        if precondition is not None:
            try:
                allowed = precondition() is True
            except Exception:
                allowed = False
            if not allowed:
                raise ControlError('control precondition rejected')
        output = self._run(['nsenter', '--target', identity[1], '--net',
                            'python3', '-c', _HTTP_SCRIPT], input=request, timeout=REQUEST_TIMEOUT_S, deadline=deadline)
        if self._identity(container_id, deadline=deadline) != identity:
            raise ControlError('container generation changed')
        try:
            envelope = json.loads(output)
            status = envelope.get('status')
            if envelope.get('ok') is not True or type(status) is not int or not 200 <= status < 300:
                raise ValueError()
            body = envelope['body']
            if not isinstance(body, str) or len(body) > 1048576:
                raise ValueError()
            try:
                return json.loads(body)
            except ValueError:
                if method == 'PUT' and path == '/v1/vpn/settings':
                    # Pinned handler writes the settings-update outcome as text.
                    return body
                raise
        except (ValueError, TypeError, KeyError, AttributeError):
            raise ControlError('control HTTP request failed or response invalid') from None
