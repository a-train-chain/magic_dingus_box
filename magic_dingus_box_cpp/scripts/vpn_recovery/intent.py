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


COMPLETED_RETENTION_S = 600
COMPLETED_LIMIT = 64


def _finite(value):
    try:
        return type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        return False


def _completed_operations(history, now):
    """Attribution only: never read by may_recover or used as an inhibit."""
    if not isinstance(history, list) or not _finite(now):
        return []
    result = []
    for entry in history[-128:]:
        if not isinstance(entry, dict):
            continue
        start, end = entry.get('created_at'), entry.get('completed_at')
        if (_finite(start) and _finite(end) and 0 <= end-start <= 86400
                and 0 <= now-end <= COMPLETED_RETENTION_S
                and isinstance(entry.get('generation'), dict)
                and isinstance(entry['generation'].get('container_id'), str)):
            result.append(entry)
    return result[-COMPLETED_LIMIT:]


def owns_stop(paths, container_id, event_at, now=None):
    """Attribute an observed event to live or recently completed maintenance.

    This never changes enabled intent and grants no container-start permission.
    Completed records cover only [created_at, completed_at], not their entire
    retention period. A later stop cannot borrow an old operation's ownership.
    """
    if not isinstance(container_id, str) or not _finite(event_at):
        return False
    now = time.time() if now is None else now
    state = read_intent(paths)
    current_boot = boot_id()
    for entry in state.get('inhibits', {}).values():
        if not isinstance(entry, dict):
            continue
        start, end = entry.get('created_at'), entry.get('expires_at')
        generation = entry.get('generation') or {}
        if (isinstance(generation, dict) and generation.get('container_id') == container_id
                and _finite(start) and _finite(end) and start <= event_at <= end
                and entry.get('boot_id') == current_boot and entry.get('owner_start') is not None
                and process_start(entry.get('owner_pid')) == entry['owner_start']):
            return True
    for entry in _completed_operations(state.get('completed_operations', []), now):
        if (entry['generation'].get('container_id') == container_id
                and entry.get('boot_id') == current_boot and entry.get('owner_start') is not None
                and entry['created_at'] <= event_at <= entry['completed_at']):
            return True
    return False


def _save(paths, state):
    now = time.time()
    state['completed_operations'] = _completed_operations(state.get('completed_operations', []), now)
    state.update(version=1, revision=uuid.uuid4().hex, updated_at=now)
    atomic_json(paths.state/'intent.json', state)
    return state


def set_intent(paths, enabled, reason):
    if type(enabled) is not bool:
        raise ValueError('invalid managed intent')
    with intent_lock(paths):
        s = read_intent(paths)
        s.update(enabled=enabled, reason=str(reason)[:80], inhibits=s.get('inhibits', {}))
        return _save(paths, s)


def enable_if_idle(paths, reason):
    """Explicit setup start, refused while another maintenance owner exists."""
    with intent_lock(paths):
        state=read_intent(paths)
        for owner in state.get('inhibits',{}).values():
            if not isinstance(owner,dict): continue
            expires=owner.get('expires_at')
            live=(type(expires) in (int,float) and time.time()<expires<=time.time()+86400
                  and owner.get('boot_id')==boot_id() and owner.get('owner_start') is not None
                  and process_start(owner.get('owner_pid'))==owner['owner_start'])
            if live: return False
        # Explicit setup authorizes a fresh start after dead-owner cleanup.
        # This never runs from the automatic controller or release operation.
        state.update(enabled=True,reason=str(reason)[:80],inhibits={})
        _save(paths,state)
        return True


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
            completed = s['inhibits'].pop(token, None)
            if isinstance(completed, dict):
                history = _completed_operations(s.get('completed_operations', []), time.time())
                # Keep historical evidence atomically with releasing inhibition.
                # Neither release nor this journal can enable stopped intent.
                history.append({**completed, 'completed_at': time.time()})
                s['completed_operations'] = history
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
