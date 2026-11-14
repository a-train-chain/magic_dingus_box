"""One bounded, verified port writer. Never starts/stops/resumes torrents.

The compose lock serializes writers with each other and lifecycle mutations.
VPN observations remain independent of qBit login and do not take this lock.
"""
import argparse
from contextlib import ExitStack
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import subprocess
import sys
import time

from .common import Paths, bounded_lock, read_env
from .control import ControlClient, LOCK_TIMEOUT_S

HTTP_TIMEOUT_S = 5
OBSERVE_TIMEOUT_S = 10
SYNC_TIMEOUT_S = 30


class PortSyncError(RuntimeError):
    """Only constant, sanitized messages may cross this boundary."""


class _Changed(PortSyncError):
    pass


class _Deferred(PortSyncError):
    pass


# A separate short-lived stdlib child enforces a TOTAL request bound, including
# cookie login, connect and body read (socket timeouts alone allow slow trickles).
# All credentials/cookies are stdin-only. No error body or headers leave child.
_QBIT_SCRIPT = r'''
import http.cookiejar, json, sys, urllib.parse, urllib.request
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None
try:
    config = json.load(sys.stdin)
    jar = http.cookiejar.MozillaCookieJar(config['cookie_file']) if config['cookie_file'] else http.cookiejar.CookieJar()
    if config['cookie_file']:
        jar.load(ignore_discard=True, ignore_expires=True)
        # curl's Netscape format writes session expiry as 0; Python treats it
        # as Unix epoch unless translated to its in-memory session marker.
        for cookie in jar:
            if cookie.expires == 0:
                cookie.expires, cookie.discard = None, True
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect(), urllib.request.HTTPCookieProcessor(jar))
    def request(method, path, data):
        data = urllib.parse.urlencode(data).encode() if data is not None else None
        req = urllib.request.Request(config['base_url']+path, data=data, method=method)
        with opener.open(req, timeout=config['timeout']) as response:
            body = response.read(1048577)
            if len(body) > 1048576:
                raise ValueError()
            return response.status, body.decode('utf-8')
    if not config['cookie_file']:
        status, body = request('POST', '/api/v2/auth/login', {'username': 'admin', 'password': config['password']})
        if status != 200 or body != 'Ok.' or not list(jar):
            raise ValueError()
    payload = {'json': json.dumps(config['payload'])} if config['payload'] is not None else None
    status, body = request(config['method'], config['path'], payload)
    result = {'status': status, 'body': body} if 200 <= status < 300 else {'status': status}
except Exception:
    result = {'error': True}
sys.stdout.write(json.dumps(result))
'''


class QbitClient:
    """Cookie-authenticated stdlib client restricted to two port preferences APIs.

    request(method, path, payload=None, *, timeout=5) -> (HTTP status, body).
    Existing curl cookies can be imported for the shell's already-authenticated
    guard cadence; controller callers authenticate from the environment file.
    Each call (including any login) is bounded by one child timeout <=5 seconds.
    """
    def __init__(self, env_file, *, cookie_file=None, base_url='http://127.0.0.1:8080', runner=subprocess.run):
        from urllib.parse import urlsplit
        parsed = urlsplit(base_url)
        if (parsed.scheme != 'http' or parsed.hostname not in ('127.0.0.1', 'localhost', '::1')
                or parsed.username or parsed.password or parsed.path not in ('', '/')
                or parsed.query or parsed.fragment):
            raise PortSyncError('invalid qBit endpoint')
        self.env_file = Path(env_file)
        self.cookie_file = str(cookie_file) if cookie_file is not None else None
        self.base_url, self.runner = base_url.rstrip('/'), runner

    def request(self, method, path, payload=None, *, timeout=HTTP_TIMEOUT_S):
        if (method, path) not in (('GET', '/api/v2/app/preferences'), ('POST', '/api/v2/app/setPreferences')):
            raise PortSyncError('qBit route not allowed')
        if method == 'POST' and (not isinstance(payload, dict) or set(payload) != {'listen_port', 'upnp', 'random_port'}
                                 or not _port(payload['listen_port']) or payload['upnp'] is not False
                                 or payload['random_port'] is not False):
            raise PortSyncError('invalid qBit port preferences')
        try:
            timeout = min(HTTP_TIMEOUT_S, float(timeout))
            if timeout <= 0:
                raise ValueError()
            env = read_env(self.env_file) if not self.cookie_file else {}
            password = env.get('MDB_QBIT_PASS') or env.get('QBITTORRENT_ADMIN_PASSWORD') or ''
            if not self.cookie_file and not password:
                raise ValueError()
            data = {'base_url': self.base_url, 'cookie_file': self.cookie_file, 'password': password,
                    'method': method, 'path': path, 'payload': payload, 'timeout': timeout}
            result = self.runner([sys.executable, '-c', _QBIT_SCRIPT], input=json.dumps(data),
                                 capture_output=True, text=True, timeout=timeout, check=False)
            if result.returncode != 0:
                raise ValueError()
            envelope = json.loads(result.stdout)
            status, body = envelope.get('status'), envelope.get('body')
            if type(status) is not int or not 200 <= status < 300 or not isinstance(body, str):
                raise ValueError()
            return status, body
        except Exception:
            raise PortSyncError('qBit request failed or timed out') from None


def _port(value, *, allow_zero=False):
    return type(value) is int and (0 if allow_zero else 1) <= value <= 65535


class PortSynchronizer:
    """Serialized sync (30s) and lock-free observe (10s), with total deadlines.

    control provides observe_generation(container_id=None, *, deadline=None)
    and get(path, container_id, *, deadline=None). qbit_transport provides
    request(method, path, payload=None, *, timeout=5) -> (status, body).
    Injected implementations must honor their deadline/timeout arguments.
    """
    def __init__(self, paths, control, qbit_transport, *,
                 observe_timeout_s=OBSERVE_TIMEOUT_S, sync_timeout_s=SYNC_TIMEOUT_S):
        if any(type(value) not in (int, float) or not math.isfinite(value) or value <= 0
               for value in (observe_timeout_s, sync_timeout_s)):
            raise ValueError('invalid port operation budget')
        self.paths, self.control, self.qbit = paths, control, qbit_transport
        self.observe_timeout_s, self.sync_timeout_s = observe_timeout_s, sync_timeout_s

    @staticmethod
    def _result():
        return {'status': 'unavailable', 'forwarded_port': None, 'qbit_port': None,
                'generation': None, 'checked_at': None, 'error': None}

    @staticmethod
    def _remaining(deadline):
        remaining = deadline-time.monotonic()
        if remaining <= 0:
            raise PortSyncError('port operation deadline exceeded')
        return min(HTTP_TIMEOUT_S, remaining)

    def _vpn(self, deadline):
        generation = self.control.observe_generation(deadline=deadline)
        port = self.control.get('/v1/portforward', generation['container_id'], deadline=deadline).get('port')
        # Resolve the service name again: a replaced former container can
        # remain alive briefly, so checking its old ID alone is insufficient.
        after = self.control.observe_generation(deadline=deadline)
        if generation != after:
            raise _Changed('VPN generation changed')
        if not _port(port, allow_zero=True):
            raise PortSyncError('forwarded port unavailable')
        return generation, port

    def _preferences(self, deadline):
        status, body = self.qbit.request('GET', '/api/v2/app/preferences', timeout=self._remaining(deadline))
        if type(status) is not int or not 200 <= status < 300:
            raise PortSyncError('qBit preferences unavailable')
        prefs = json.loads(body)
        if (not isinstance(prefs, dict) or not _port(prefs.get('listen_port'))
                or type(prefs.get('upnp')) is not bool or type(prefs.get('random_port')) is not bool):
            raise PortSyncError('invalid qBit preferences')
        return {key: prefs[key] for key in ('listen_port', 'upnp', 'random_port')}

    def _check(self, generation, port, deadline):
        after, new_port = self._vpn(deadline)
        if after != generation or new_port != port:
            raise _Changed('VPN generation or port changed')

    def _operate(self, result, deadline, mutate, expected_generation=None):
        try:
            generation, port = self._vpn(deadline)
        except _Changed:
            raise
        except Exception:
            result['error'] = 'VPN observation unavailable'
            return
        result.update(generation=generation, forwarded_port=port)
        if expected_generation is not None and generation != expected_generation:
            raise _Changed('VPN generation changed')
        if port == 0:
            result['status'] = 'no_port'
            return
        # Observe VPN first: failed authentication cannot hide tunnel evidence.
        prefs = self._preferences(deadline)
        result['qbit_port'] = prefs['listen_port']
        wanted = {'listen_port': port, 'upnp': False, 'random_port': False}
        if prefs != wanted and mutate:
            # Re-read under lock immediately before writing. A late qBit read
            # may belong to a tunnel whose forwarding lease has already moved.
            self._check(generation, port, deadline)
            status, _ = self.qbit.request('POST', '/api/v2/app/setPreferences', wanted,
                                          timeout=self._remaining(deadline))
            if type(status) is not int or not 200 <= status < 300:
                raise PortSyncError('qBit port update failed')
            prefs = self._preferences(deadline)
            result['qbit_port'] = prefs['listen_port']
            if prefs != wanted:
                raise PortSyncError('qBit port verification failed')
        self._check(generation, port, deadline)
        self._remaining(deadline)
        result['status'] = 'synced' if prefs == wanted else 'changed'

    def _run(self, mutate, expected_generation=None, deadline=None):
        result = self._result()
        own_deadline = time.monotonic()+(self.sync_timeout_s if mutate else self.observe_timeout_s)
        deadline = own_deadline if deadline is None else min(own_deadline, deadline)
        try:
            if mutate:
                with ExitStack() as locks:
                    try:
                        locks.enter_context(bounded_lock(self.paths.compose_lock,
                                                        min(LOCK_TIMEOUT_S, self._remaining(deadline))))
                    except (OSError, TimeoutError):
                        raise _Deferred() from None
                    self._operate(result, deadline, True, expected_generation)
            else:
                self._operate(result, deadline, False)
        except _Deferred:
            result.update(status='deferred', error='port writer lock unavailable')
        except _Changed:
            result.update(status='changed', error='VPN generation or port changed')
        except Exception:
            result.update(status='failed', error='qBit port operation failed')
        result['checked_at'] = datetime.now(timezone.utc).isoformat()
        return result

    def sync(self, expected_generation=None, *, deadline=None) -> dict:
        """A caller monotonic deadline can further shorten the total budget."""
        return self._run(True, expected_generation, deadline)

    def observe(self, *, deadline=None) -> dict:
        """Bounded reads only; reports VPN evidence even if qBit login fails."""
        return self._run(False, deadline=deadline)


def main(argv=None):
    parser = argparse.ArgumentParser(description='Verified qBit port synchronization')
    parser.add_argument('--env-file', default='/opt/magic_dingus_box/services/.env')
    parser.add_argument('--cookie-file')
    parser.add_argument('--qbit-api', default='http://127.0.0.1:8080')
    args = parser.parse_args(argv)
    try:
        env_file = Path(args.env_file)
        paths = Paths(base=env_file.parent.parent, services=env_file.parent)
        qbit = QbitClient(env_file, cookie_file=args.cookie_file, base_url=args.qbit_api)
        result = PortSynchronizer(paths, ControlClient(paths), qbit).sync()
    except Exception:
        result = {'status': 'failed', 'error': 'port synchronization unavailable'}
    print('[qbit-port-sync] '+json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
