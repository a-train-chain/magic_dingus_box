"""Paths, desired policy and durable I/O shared by VPN recovery adapters."""
from contextlib import contextmanager
from dataclasses import dataclass
import fcntl
import json
import math
import os
from pathlib import Path
import re
import tempfile
import time


@dataclass(frozen=True)
class Paths:
    base: Path = Path('/opt/magic_dingus_box')
    services: Path = None
    config: Path = None
    state: Path = Path('/var/lib/magic-dingus/vpn_recovery')
    runtime: Path = Path('/run/mdb-vpn-recovery')
    compose_lock: Path = Path('/run/lock/mdb-compose.lock')

    def __post_init__(self):
        for name in ('base', 'state', 'runtime', 'compose_lock'):
            object.__setattr__(self, name, Path(getattr(self, name)))
        for name in ('services', 'config'):
            value = getattr(self, name)
            object.__setattr__(self, name, Path(value) if value is not None else self.base/name)


def read_env(path: Path) -> dict:
    """Match the existing admin's quote/duplicate handling; never source a shell.

    An absent file is empty; unreadable/undecodable input fails closed. Unknown
    .env keys are retained for callers, but load_config only uses VPN policy.
    """
    try:
        text = Path(path).read_text()
    except FileNotFoundError:
        return {}
    except (OSError, UnicodeError):
        raise ValueError('cannot read VPN environment') from None
    result = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        key, _, value = line.partition('=')
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in '\"\'':
            value = value[1:-1]
        result[key.strip()] = value
    return result


def _countries(value):
    if not isinstance(value, list) or any(
        not isinstance(country, str) or not re.fullmatch(r"[A-Za-z][A-Za-z .'-]{0,79}", country)
        or country != country.strip() for country in value
    ):
        raise ValueError('invalid VPN country policy')
    if len(value) > 100 or len(set(value)) != len(value):
        raise ValueError('invalid VPN country policy')
    return value


def load_config(paths: Paths) -> dict:
    """Return desired settings without modifying .env or effective selection.

    The existing country UI owns VPN_COUNTRIES. The optional JSON file can
    configure only fallback_countries; absent policy never adds a fallback.
    """
    env = read_env(paths.services/'.env')
    provider = (env.get('VPN_SERVICE_PROVIDER') or 'protonvpn').strip().lower()
    vpn_type = (env.get('VPN_TYPE') or 'wireguard').strip().lower()
    forwarding = (env.get('VPN_PORT_FORWARDING') or 'off').strip().lower()
    if not re.fullmatch(r'[a-z][a-z0-9_-]{0,63}', provider):
        raise ValueError('invalid VPN provider')
    if vpn_type not in ('wireguard', 'openvpn') or forwarding not in ('on', 'off'):
        raise ValueError('invalid VPN type or forwarding policy')
    countries = [c.strip() for c in env.get('VPN_COUNTRIES', '').split(',') if c.strip()]
    preferred = _countries(countries)
    try:
        policy = json.loads((paths.config/'vpn_recovery.json').read_text())
    except FileNotFoundError:
        policy = {}
    except (OSError, UnicodeError, ValueError):
        raise ValueError('cannot read VPN recovery policy') from None
    if not isinstance(policy, dict) or policy.keys() - {'fallback_countries'}:
        raise ValueError('invalid VPN recovery policy fields')
    fallback = _countries(policy.get('fallback_countries', []))
    return {'provider': provider, 'type': vpn_type, 'forwarding': forwarding == 'on',
            'preferred_countries': preferred, 'fallback_countries': fallback}


def atomic_write(path: Path, content: bytes, mode: int = 0o600):
    """Replace in the same directory, fsync data and directory, restrict mode."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.'+path.name+'.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            os.fchmod(stream.fileno(), mode)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def atomic_json(path, value, mode=0o600):
    atomic_write(path, (json.dumps(value, sort_keys=True, allow_nan=False)+'\n').encode(), mode)


@contextmanager
def bounded_lock(path, timeout_s):
    """Read-open an EXISTING lock; a missing/unusable/busy lock never grants it.

    Read-only open preserves the shared compose-lock inode and works across
    its root/user owners. Callers must provision their own private lock first.
    """
    if not isinstance(timeout_s, (int, float)) or not math.isfinite(timeout_s) or timeout_s < 0:
        raise ValueError('invalid lock timeout')
    deadline = time.monotonic() + timeout_s
    with Path(path).open('rb') as stream:
        while True:
            try:
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                remaining = deadline-time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('lock unavailable') from None
                time.sleep(min(.025, remaining))
        try:
            yield
        finally:
            fcntl.flock(stream, fcntl.LOCK_UN)
