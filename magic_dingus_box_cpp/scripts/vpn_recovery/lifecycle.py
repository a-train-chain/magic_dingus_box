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
from .control import ensure_control_credentials, ControlClient, ControlError
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


def current(paths, deadline=None):
    from .runtime import SystemAdapters
    return SystemAdapters(paths).current(min(time.monotonic()+2,deadline) if deadline is not None else time.monotonic()+2)


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


PLAYBACK_PAUSE_MARKER=Path('/tmp/mdb_playback_services_paused')
DEPENDENTS=('radarr','sonarr','prowlarr','qbittorrent','byparr')


def container_key(item):
    return tuple(item.get(k) for k in ('container_id','pid','started_at'))


def generation(paths, deadline):
    return ControlClient(paths).observe_generation(deadline=deadline)


def wait_healthy(paths, container, deadline, *, runner=subprocess.run, allowed=lambda: True):
    """Observe only, outside compose. Health belongs to this exact start."""
    while time.monotonic()<deadline and allowed():
        try:
            observed=current(paths,deadline)
            if observed.get('running') is not True or container_key(observed)!=container: return None
            remaining=min(3,deadline-time.monotonic())
            if remaining<=0: return None
            result=runner(['docker','inspect','--format','{{.State.Health.Status}}',container[0]],
                          capture_output=True,text=True,timeout=remaining,check=False)
            if result.returncode==0 and result.stdout.strip()=='healthy':
                selected=generation(paths,min(deadline,time.monotonic()+3))
                if container_key(selected)==container: return selected
                return None
        except (OSError,TimeoutError,subprocess.SubprocessError,ControlError): return None
        remaining=deadline-time.monotonic()
        if remaining<=0: return None
        time.sleep(min(1,remaining))
    return None


def manage(paths, action, *, runner=subprocess.run, actual=None):
    if action not in ('start', 'stop', 'restart', 'reload-auth'):
        raise ValueError('unknown stack operation')
    configured=(paths.services/'.env').is_file() and (paths.services/'docker-compose.yml').is_file()
    if action != 'stop' and not configured: return 3
    if action == 'stop': set_intent(paths, False, 'operator stop')
    elif action in ('start', 'restart'):
        if read_intent(paths).get('inhibits'): return 3
        set_intent(paths, True, 'operator '+action)
    deadline=time.monotonic()+270
    actual=current(paths,deadline) if actual is None else actual
    prior={k:actual.get(k) for k in ('container_id','pid','started_at')} if actual.get('running') else None
    token=uuid.uuid4().hex
    state=inhibit(paths,token,os.getpid(),action,generation=prior,ttl_s=360)
    settings=settings_revision(paths)
    allowed=lambda: _allowed(paths,token,state['revision'],settings)
    def lock_wait():
        remaining=deadline-time.monotonic()
        if remaining<=0: raise TimeoutError('management deadline')
        return min(30,remaining)
    def execute(command, cap=30, *, submit=False, before=None):
        if action!='stop' and not allowed(): return 3
        remaining=min(cap,deadline-time.monotonic())
        if remaining<=0: return 124
        if submit: record_catalog_submission(paths,before or {})
        return runner(command,cwd=str(paths.services),timeout=remaining,check=False).returncode
    try:
        # Phase one has no Compose dependency/health wait. An explicit stop can
        # take this same lock while the later read-only health phase is pending.
        with bounded_lock(paths.compose_lock,lock_wait()):
            if action!='stop' and not allowed(): return 3
            if action=='reload-auth':
                observed=current(paths,deadline)
                if observed.get('running') is not True or container_key(observed)!=container_key(actual): return 3
                return execute(['docker','restart','mdb_gluetun'],20,submit=True,before=observed)
            if action in ('stop','restart'):
                rc=execute(['docker','compose','down'],60)
                if rc or action=='stop': return rc
            before=current(paths,deadline)
            rc=execute(['docker','compose','up','-d','--no-deps','gluetun'],30,submit=True,before=before)
            if rc: return rc
            started=current(paths,deadline)
            if (started.get('running') is not True or not isinstance(started.get('container_id'),str)
                    or not started['container_id'] or type(started.get('pid')) is not int or started['pid']<=0
                    or not isinstance(started.get('started_at'),str) or not started['started_at']): return 2
            expected=container_key(started)
        selected=wait_healthy(paths,expected,deadline,runner=runner,allowed=allowed)
        if not selected: return 3 if not allowed() else 1
        # Reacquire and fence ALL evidence after the unlocked wait. --no-deps
        # is required here too, so health changing cannot start a hidden wait.
        with bounded_lock(paths.compose_lock,lock_wait()):
            if not allowed(): return 3
            if generation(paths,min(deadline,time.monotonic()+3))!=selected: return 3
            if not allowed(): return 3
            targets=['qbittorrent'] if PLAYBACK_PAUSE_MARKER.exists() else list(DEPENDENTS)
            return execute(['docker','compose','up','-d','--no-deps',*targets],30)
    except (OSError,TimeoutError,subprocess.SubprocessError,ControlError): return 2
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
