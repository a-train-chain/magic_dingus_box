"""Durable operator intent; maintenance owners are PID/start/boot identities.

Lock order: compose -> intent is permitted. Intent functions NEVER acquire
compose or call Docker. No release operation can turn a stopped intent on.
"""
from contextlib import contextmanager
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import time
import uuid
from .common import atomic_json, bounded_lock


def read_json(path, limit=262144):
    try:
        with Path(path).open('rb') as stream:
            raw = stream.read(limit+1)
        if len(raw) > limit:
            return {}
        value = json.loads(raw)
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError, RecursionError):
        return {}


def boot_id():
    try:
        return Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    except OSError:
        # Dev machines: a boot-stable identity, without changing production.
        return 'dev-'+str(int(time.time()-time.monotonic())//10)


def process_start(pid):
    if type(pid) is not int or pid <= 0:
        return None
    try:
        # comm may contain spaces/parentheses. Field 22 is index 19 after ).
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[19]
    except (OSError, IndexError):
        if Path('/proc').is_dir():
            return None
        try:
            value = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart='], capture_output=True,
                                   text=True, timeout=.2, check=False)
            return value.stdout.strip() or None
        except Exception:
            return None


@contextmanager
def intent_lock(paths):
    paths.state.mkdir(parents=True, exist_ok=True)
    path = paths.state/'intent.lock'
    fd = os.open(path, os.O_RDONLY | os.O_CREAT, 0o600); os.close(fd)
    with bounded_lock(path, .25):
        yield


def read_intent(paths):
    state = read_json(paths.state/'intent.json')
    if (state.get('version') != 1 or type(state.get('enabled')) is not bool
            or not isinstance(state.get('revision'), str) or not isinstance(state.get('inhibits'), dict)
            or len(state['inhibits'])>32):
        return {}
    return state


def _save(paths, state):
    state.update(version=1, revision=uuid.uuid4().hex, updated_at=time.time())
    atomic_json(paths.state/'intent.json', state)
    return state


def set_intent(paths, enabled, reason):
    if type(enabled) is not bool:
        raise ValueError('invalid managed intent')
    with intent_lock(paths):
        s = read_intent(paths)
        s.update(enabled=enabled, reason=str(reason)[:80], inhibits=s.get('inhibits', {}))
        return _save(paths, s)


def inhibit(paths, token, owner_pid, reason, generation=None, ttl_s=3600):
    if not isinstance(token, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,100}', token):
        raise ValueError('invalid inhibit token')
    if type(ttl_s) not in (int,float) or not math.isfinite(ttl_s) or not 0<ttl_s<=86400:
        raise ValueError('invalid inhibit lifetime')
    with intent_lock(paths):
        s = read_intent(paths) or {'enabled': False, 'inhibits': {}}
        if len(s['inhibits'])>=32 and token not in s['inhibits']:
            raise ValueError('too many maintenance owners')
        s['inhibits'][token] = {'owner_pid': owner_pid, 'owner_start': process_start(owner_pid),
                                'boot_id': boot_id(), 'reason': str(reason)[:80],
                                'generation': generation, 'created_at': time.time(), 'expires_at':time.time()+ttl_s}
        return _save(paths, s)


def release_inhibit(paths, token):
    with intent_lock(paths):
        s = read_intent(paths)
        if s:
            s['inhibits'].pop(token, None)
            return _save(paths, s)
        return {}


def may_recover(paths, actual=None):
    s = read_intent(paths)
    if not s or not s['enabled']:
        return False
    if not s['inhibits']:
        return True
    with intent_lock(paths):
        s = read_intent(paths)
        if not s or not s['enabled']:
            return False
        dead = []
        for token, owner in s['inhibits'].items():
            if not isinstance(owner, dict):
                dead.append(token); continue
            expires=owner.get('expires_at')
            unexpired=type(expires) in (int,float) and time.time()<expires<=time.time()+86400
            alive = (unexpired and owner.get('boot_id') == boot_id() and owner.get('owner_start') is not None
                     and process_start(owner.get('owner_pid')) == owner.get('owner_start'))
            if alive:
                return False
            dead.append(token)
        # Dead owners need actual managed state; unknown state stays inhibited.
        if dead and actual is not None:
            for token in dead:
                s['inhibits'].pop(token, None)
            if not all(actual.get(k) is True for k in ('running', 'unit_active', 'configured')):
                s.update(enabled=False, reason='maintenance owner disappeared while stopped')
            _save(paths, s)
        return s['enabled'] and not s['inhibits']


def adopt_running(paths, actual):
    with intent_lock(paths):
        if (paths.state/'intent.json').exists() or not all(actual.get(k) is True for k in ('running', 'unit_active', 'configured')):
            return False
        _save(paths, {'enabled': True, 'reason': 'adopt existing running stack', 'inhibits': {}})
        return True


def settings_revision(paths):
    digest = hashlib.sha256()
    for path in (paths.services/'.env', paths.config/'vpn_recovery.json'):
        try:
            with path.open('rb') as stream:
                raw = stream.read(1048577)
            if len(raw) > 1048576:
                raise ValueError('settings too large')
            digest.update(raw)
        except FileNotFoundError:
            digest.update(b'<absent>')
        digest.update(b'\0')
    return digest.hexdigest()
