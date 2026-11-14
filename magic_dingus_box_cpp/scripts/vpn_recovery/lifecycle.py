"""Bounded, inhibited management of the existing stack; no second service.

Only explicit start/restart enables intent. Delivery activates credentials on
proven running native installations, preserving stop/enablement decisions.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import time
import uuid
from .common import atomic_json, bounded_lock, load_config
from .control import ensure_control_credentials, ControlClient
from .intent import (adopt_running, inhibit, read_intent, read_json,
                     release_inhibit, set_intent, settings_revision)


def provision(paths):
    """Installer/boot-only inode creation. Mutation clients never create it."""
    paths.compose_lock.parent.mkdir(parents=True,exist_ok=True)
    try:
        fd=os.open(paths.compose_lock,os.O_CREAT|os.O_EXCL|os.O_RDONLY,0o666)
        try: os.fchmod(fd,0o666)
        finally: os.close(fd)
    except FileExistsError: pass
    paths.state.mkdir(parents=True,exist_ok=True)
    paths.runtime.mkdir(parents=True,exist_ok=True)


def current(paths):
    from .runtime import SystemAdapters
    return SystemAdapters(paths).current(time.monotonic()+2)


def fingerprint(scripts):
    scripts = Path(scripts)
    digest = hashlib.sha256()
    files = [scripts/'gluetun_cascade_restart.sh', scripts/'vpn_recoveryctl.py']
    files += sorted((scripts/'vpn_recovery').glob('*.py'))
    for path in files:
        digest.update(path.name.encode()); digest.update(b'\0')
        if path.is_file(): digest.update(path.read_bytes())
        digest.update(b'\0')
    return digest.hexdigest()


def migration_needed(paths, scripts):
    if read_json(paths.runtime/'running-fingerprint.json').get('fingerprint') != fingerprint(scripts): return True
    try: auth=hashlib.sha256((paths.services/'config/gluetun/auth/config.toml').read_bytes()).hexdigest()
    except OSError: return True
    return read_json(paths.state/'auth-loaded.json').get('fingerprint') != auth


def _allowed(paths, token, revision, settings):
    state=read_intent(paths)
    return (state.get('enabled') is True and state.get('revision') == revision
            and set(state.get('inhibits', {})) == {token}
            and settings_revision(paths) == settings)


def record_catalog_submission(paths, actual, *, snapshot=None, now=None):
    """Caller holds compose + validated intent/settings fence, at Docker I/O.

    An immutable downloaded snapshot is submission evidence only. The polling
    owner still requires a NEW start, matching disk identity and INFO log proof.
    Missing/invalid catalog is ordinary bootstrap and never blocks a start.
    """
    from .catalog import CatalogStore
    try:
        snapshot=CatalogStore(paths).load() if snapshot is None else snapshot
        if snapshot is None: return False
        old={k:actual.get(k) for k in ('container_id','started_at','pid')} if actual.get('container_id') else None
        atomic_json(paths.state/'load-pending.json',{'provider':snapshot.provider,'old_container':old,
                    'submitted_wall':time.time() if now is None else now})
        return True
    except (OSError,ValueError,TypeError): return False


def manage(paths, action, *, runner=subprocess.run, actual=None):
    if action not in ('start', 'stop', 'restart', 'reload-auth'):
        raise ValueError('unknown stack operation')
    configured=(paths.services/'.env').is_file() and (paths.services/'docker-compose.yml').is_file()
    if action != 'stop' and not configured: return 3
    # Stop is published BEFORE waiting for compose. Later starts are fenced.
    if action == 'stop': set_intent(paths, False, 'operator stop')
    elif action in ('start', 'restart'):
        if read_intent(paths).get('inhibits'): return 3
        set_intent(paths, True, 'operator '+action)
    actual=current(paths) if actual is None else actual
    generation={k:actual.get(k) for k in ('container_id','pid','started_at')} if actual.get('running') else None
    token=uuid.uuid4().hex
    state=inhibit(paths, token, os.getpid(), action, generation=generation, ttl_s=360)
    settings=settings_revision(paths)
    try:
        with bounded_lock(paths.compose_lock, 30):
            if action != 'stop' and not _allowed(paths,token,state['revision'],settings): return 3
            if action=='reload-auth':
                observed=current(paths)
                identity=lambda item: tuple(item.get(k) for k in ('container_id','pid','started_at'))
                if observed.get('running') is not True or identity(observed)!=identity(actual): return 3
            commands=[]
            if action in ('stop','restart'): commands.append(['docker','compose','down'])
            if action in ('start','restart'): commands.append(['docker','compose','up','-d'])
            if action == 'reload-auth': commands.append(['docker','restart','mdb_gluetun'])
            deadline=time.monotonic()+270
            for command in commands:
                if action != 'stop' and not _allowed(paths,token,state['revision'],settings): return 3
                remaining=deadline-time.monotonic()
                if remaining<=0: return 124
                if command[-1]=='-d' or action=='reload-auth':
                    record_catalog_submission(paths,actual)
                result=runner(command,cwd=str(paths.services),timeout=remaining,check=False)
                if result.returncode: return result.returncode
        return 0
    except (OSError, TimeoutError, subprocess.SubprocessError):
        return 2
    finally:
        release_inhibit(paths,token)


def activate(paths, *, actual=None, runner=subprocess.run):
    """Idempotent new-tree hook, before watcher activation. Never starts a stack."""
    if not (paths.services/'.env').is_file(): return 0
    ensure_control_credentials(paths)
    config=load_config(paths)
    if not (config['provider']=='protonvpn' and config['type']=='wireguard' and config['forwarding']): return 0
    actual=current(paths) if actual is None else actual
    adopt_running(paths,actual)
    state=read_intent(paths)
    if (not all(actual.get(k) is True for k in ('running','unit_active','configured'))
            or not state.get('enabled') or state.get('inhibits')): return 0
    auth=paths.services/'config/gluetun/auth/config.toml'
    installed=hashlib.sha256(auth.read_bytes()).hexdigest()
    loaded=read_json(paths.state/'auth-loaded.json')
    if loaded.get('fingerprint') == installed: return 0
    try:
        ControlClient(paths).get('/v1/vpn/status',actual.get('container_id'),deadline=time.monotonic()+3)
        atomic_json(paths.state/'auth-loaded.json',{'fingerprint':installed})
        return 0
    except Exception: pass
    rc=manage(paths,'reload-auth',runner=runner,actual=actual)
    if rc == 0: atomic_json(paths.state/'auth-loaded.json',{'fingerprint':installed})
    return rc
