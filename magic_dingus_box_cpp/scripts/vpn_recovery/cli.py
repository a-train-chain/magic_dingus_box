"""Stable deployed-tree lifecycle entry point. No credential-bearing output."""
import argparse
from datetime import datetime, timezone
import json
import os
import signal
import shutil
import tempfile
import subprocess
import sys
import time
import uuid

from .common import Paths, bounded_lock, load_config
from .intent import (set_intent, inhibit, release_inhibit, read_intent, read_json,
                     settings_revision, adopt_running, enable_if_idle)
from .runtime import Controller, SystemAdapters
from .lifecycle import activate, manage, fingerprint, provision, record_catalog_submission, migration_needed
from .common import atomic_json
from pathlib import Path


def read_status(paths, now=None):
    status=read_json(paths.state/'status.json')
    now=time.time() if now is None else now
    try:
        age=now-datetime.fromisoformat(status['checked_at']).timestamp()
        fresh=0<=age<=45
    except (KeyError,ValueError,TypeError):fresh=False
    if not fresh:
        status.update(ready=False,traffic_verified=False,qbit_synced=False,state='Observation stale')
    return status


def operation_allowed(paths, environ=None):
    env=os.environ if environ is None else environ
    state=read_intent(paths);token=env.get('MDB_VPN_INHIBIT_TOKEN')
    return bool(state.get('enabled') and state.get('revision')==env.get('MDB_VPN_INTENT_REVISION')
                and set(state.get('inhibits',{}))=={token}
                and settings_revision(paths)==env.get('MDB_VPN_SETTINGS_REVISION'))


def controller_fence_allowed(paths):
    ctx=read_json(paths.runtime/'action.json')
    state=read_intent(paths)
    if (not state.get('enabled') or state.get('inhibits') or state.get('revision')!=ctx.get('intent_revision')
            or settings_revision(paths)!=ctx.get('revision')):return False
    actual=SystemAdapters(paths).current(time.monotonic()+2)
    return actual.get('running') is True and Controller.container_key(actual)==ctx.get('container')


def begin_owner(paths, token, reason, timeout, owner_pid):
    if not 0<timeout<=86400: raise ValueError('invalid owner timeout')
    actual=SystemAdapters(paths).current(time.monotonic()+2)
    generation={k:actual.get(k) for k in ('container_id','started_at','pid')} if actual.get('running') else None
    state=inhibit(paths,token,owner_pid,reason,generation=generation,ttl_s=min(86400,timeout+60))
    return {'MDB_VPN_INHIBIT_TOKEN':token,'MDB_VPN_INTENT_REVISION':state['revision'],
            'MDB_VPN_SETTINGS_REVISION':settings_revision(paths)}


def privileged_owner(paths, operation, token, reason, timeout, *, launcher=None):
    """Short privileged state bridge; the updater and its child stay unprivileged.

    Owner is the calling wrapper PID, not this transient sudo helper. Only the
    public revision envelope crosses stdout. Sudo never executes the OTA argv.
    """
    launcher=Path(launcher) if launcher is not None else Path(__file__).resolve().parent.parent/'vpn_recoveryctl.py'
    argv=['sudo','-n',sys.executable,'-B',str(launcher),'--base',str(paths.base),
          '--services',str(paths.services),'--state',str(paths.state),'--runtime',str(paths.runtime),
          '--compose-lock',str(paths.compose_lock),'maintenance-owner',operation,
          '--token',token,'--owner-pid',str(os.getpid()),'--reason',reason,'--timeout',str(timeout)]
    result=subprocess.run(argv,capture_output=True,text=True,check=False,cwd=str(launcher.parent),
                          timeout=90 if operation=='activate' else 8)
    if result.returncode: raise RuntimeError('privileged maintenance unavailable')
    if operation!='begin': return {}
    envelope=json.loads(result.stdout)
    required={'MDB_VPN_INHIBIT_TOKEN','MDB_VPN_INTENT_REVISION','MDB_VPN_SETTINGS_REVISION'}
    if (not isinstance(envelope,dict) or set(envelope)!=required or envelope.get('MDB_VPN_INHIBIT_TOKEN')!=token
            or any(not isinstance(v,str) or len(v)>128 for v in envelope.values())):
        raise ValueError('invalid maintenance envelope')
    return envelope


def owner_snapshot():
    """Private code-only lifetime copy, outside the tree an OTA may replace."""
    scratch=tempfile.TemporaryDirectory(prefix='mdb-vpn-owner-',dir='/tmp')
    try:
        root=Path(scratch.name)
        package=root/'vpn_recovery';package.mkdir(mode=0o700)
        for source in Path(__file__).resolve().parent.glob('*.py'):
            shutil.copyfile(source,package/source.name)
        launcher=root/'vpn_recoveryctl.py'
        shutil.copyfile(Path(__file__).resolve().parent.parent/'vpn_recoveryctl.py',launcher)
        return scratch,launcher
    except BaseException:
        scratch.cleanup();raise


def run_inhibited(paths, command, reason, timeout):
    if not command or not 0<timeout<=86400:
        raise ValueError('invalid bounded command')
    token=uuid.uuid4().hex
    privileged=False
    scratch=None;helper=None
    try:
        envelope=begin_owner(paths,token,reason,timeout,os.getpid())
    except PermissionError:
        if os.geteuid()==0: raise
        privileged=True
        scratch,helper=owner_snapshot()
        try: envelope=privileged_owner(paths,'begin',token,reason,timeout,launcher=helper)
        except BaseException:
            scratch.cleanup();raise
    env={**os.environ,**envelope}
    process=None
    old_handlers={}
    def interrupted(signum, _frame):
        if process is not None:
            try:os.killpg(process.pid,signal.SIGTERM)
            except ProcessLookupError:pass
        raise InterruptedError()
    try:
        for sig in (signal.SIGTERM,signal.SIGINT):
            old_handlers[sig]=signal.signal(sig,interrupted)
        process=subprocess.Popen(command,env=env,start_new_session=True)
        try:return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid,signal.SIGTERM)
            try:process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=2)
            return 124
    except InterruptedError:
        if process is not None:
            try:process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=2)
        return 130
    finally:
        for sig,handler in old_handlers.items():signal.signal(sig,handler)
        try:
            if privileged: privileged_owner(paths,'end',token,reason,timeout,launcher=helper)
            else: release_inhibit(paths,token)
            # Auth activation deferred by this maintenance owner runs once
            # after release; a pre-package rollback never reactivates it.
            if reason in ('ota','setup') and (paths.base/'magic_dingus_box_cpp/scripts/vpn_recoveryctl.py').is_file():
                try:
                    if privileged: privileged_owner(paths,'activate',token,reason,timeout,launcher=helper)
                    else: activate(paths)
                except Exception: print('[vpn-recovery] credential activation deferred',file=sys.stderr)
        finally:
            if scratch is not None: scratch.cleanup()



def parser():
    p=argparse.ArgumentParser(description='Managed VPN recovery')
    p.add_argument('--base',default='/opt/magic_dingus_box')
    p.add_argument('--services');p.add_argument('--state',default=os.environ.get('MDB_VPN_STATE','/var/lib/magic-dingus/vpn_recovery'))
    p.add_argument('--runtime',default=os.environ.get('MDB_VPN_RUNTIME','/run/mdb-vpn-recovery'));p.add_argument('--compose-lock',default=os.environ.get('MDB_COMPOSE_LOCK','/run/lock/mdb-compose.lock'))
    sub=p.add_subparsers(dest='command',required=True)
    run=sub.add_parser('run');run.add_argument('--once',action='store_true')
    for name in ('sync-port','catalog-refresh','supported','check-operation','check-fence','activate','fingerprint','settings-revision','provision','record-start','migration-needed'):sub.add_parser(name)
    status=sub.add_parser('status');status.add_argument('--summary',action='store_true')
    lifecycle=sub.add_parser('lifecycle');lifecycle.add_argument('operation',choices=['start','stop','restart'])
    intent=sub.add_parser('intent');intent.add_argument('operation',choices=['enable','stop','adopt-running']);intent.add_argument('--reason',default='operator');intent.add_argument('--if-idle',action='store_true')
    inh=sub.add_parser('inhibit');inh.add_argument('operation',choices=['begin','end']);inh.add_argument('token')
    inh.add_argument('--owner-pid',type=int);inh.add_argument('--reason',default='maintenance')
    owner=sub.add_parser('maintenance-owner');owner.add_argument('operation',choices=['begin','end','activate'])
    owner.add_argument('--token',required=True);owner.add_argument('--owner-pid',type=int,required=True)
    owner.add_argument('--reason',default='ota');owner.add_argument('--timeout',type=float,default=1800)
    wrapped=sub.add_parser('run-inhibited');wrapped.add_argument('--reason',default='maintenance')
    wrapped.add_argument('--timeout',type=float,default=1800);wrapped.add_argument('argv',nargs=argparse.REMAINDER)
    return p


def main(argv=None):
    args=parser().parse_args(argv)
    paths=Paths(base=args.base,services=args.services,state=args.state,runtime=args.runtime,compose_lock=args.compose_lock)
    try:
        if args.command=='maintenance-owner':
            if os.geteuid()!=0: return 3
            if args.operation=='begin': print(json.dumps(begin_owner(paths,args.token,args.reason,args.timeout,args.owner_pid),sort_keys=True))
            elif args.operation=='end': release_inhibit(paths,args.token)
            else: return activate(paths)
            return 0
        if args.command=='migration-needed': return 0 if migration_needed(paths,Path(__file__).resolve().parent.parent) else 1
        if args.command=='record-start':
            # Shell's read-open fd9 shares the same open-file description.
            # Take/retain its lock before touching the intent fence; never
            # create a lock or publish proof on an unavailable descriptor.
            import fcntl
            held=os.fstat(9);expected=paths.compose_lock.stat()
            if (held.st_dev,held.st_ino)!=(expected.st_dev,expected.st_ino): return 3
            fcntl.flock(9,fcntl.LOCK_EX|fcntl.LOCK_NB)
            if not operation_allowed(paths): return 3
            record_catalog_submission(paths,SystemAdapters(paths).current(time.monotonic()+2))
            return 0
        if args.command=='provision': provision(paths);return 0
        if args.command=='settings-revision': print(settings_revision(paths));return 0
        if args.command=='activate': return activate(paths)
        if args.command=='fingerprint': print(fingerprint(Path(__file__).resolve().parent.parent));return 0
        if args.command=='lifecycle': return manage(paths,args.operation)
        if args.command=='supported':
            c=load_config(paths)
            return 0 if c['provider']=='protonvpn' and c['type']=='wireguard' and c['forwarding'] else 1
        if args.command=='status':
            status=read_status(paths)
            if args.summary:
                if status.get('ready'):
                    print('pass\tVPN Ready: traffic verified, forwarded port and qBittorrent aligned')
                elif status.get('state')=='Observation stale':
                    print('warn\tVPN state unknown: no fresh recovery observation')
                else:
                    reason=str(status.get('state') or 'Unknown')[:120].replace('\n',' ').replace('\t',' ')
                    print('warn\tVPN '+reason+'; cached local movies remain available')
            else: print(json.dumps(status,sort_keys=True))
            return 0
        if args.command=='check-fence':return 0 if controller_fence_allowed(paths) else 3
        if args.command=='check-operation':return 0 if operation_allowed(paths) else 3
        if args.command=='intent':
            if args.operation=='adopt-running':
                a=SystemAdapters(paths)
                return 0 if adopt_running(paths,a.current(time.monotonic()+2)) or read_intent(paths).get('enabled') else 3
            if args.operation=='enable' and args.if_idle:
                return 0 if enable_if_idle(paths,args.reason) else 3
            set_intent(paths,args.operation=='enable',args.reason);return 0
        if args.command=='inhibit':
            if args.operation=='begin':
                if not args.owner_pid:raise ValueError('owner PID required')
                actual=SystemAdapters(paths).current(time.monotonic()+2)
                generation={k:actual.get(k) for k in ('container_id','started_at','pid')} if actual.get('running') else None
                inhibit(paths,args.token,args.owner_pid,args.reason,generation=generation)
            else:release_inhibit(paths,args.token)
            return 0
        if args.command=='run-inhibited':
            command=args.argv[1:] if args.argv[:1]==['--'] else args.argv
            return run_inhibited(paths,command,args.reason,args.timeout)
        if args.command=='catalog-refresh':
            a=SystemAdapters(paths);print(json.dumps(a.store.refresh(time.time(),force=True),sort_keys=True));return 0
        if args.command=='sync-port':
            a=SystemAdapters(paths);print(json.dumps(a.ports.sync(),sort_keys=True));return 0
        paths.runtime.mkdir(parents=True,exist_ok=True)
        lock=paths.runtime/'controller.lock'
        fd=os.open(lock,os.O_CREAT|os.O_RDONLY,0o600);os.close(fd)
        with bounded_lock(lock,0):
            atomic_json(paths.runtime/'running-fingerprint.json',{'fingerprint':fingerprint(Path(__file__).resolve().parent.parent)})
            controller=Controller(paths)
            stopped=[False]
            old={sig:signal.signal(sig,lambda *_:stopped.__setitem__(0,True)) for sig in (signal.SIGTERM,signal.SIGINT)}
            try:
                while not stopped[0]:
                    start=time.monotonic()
                    try:controller.tick()
                    except Exception:print('[vpn-recovery] observation unavailable',file=sys.stderr)
                    if args.once:break
                    # Anchored cadence; action/catalog futures never block it.
                    while not stopped[0] and time.monotonic()<start+10:
                        time.sleep(min(.2,max(0,start+10-time.monotonic())))
            finally:
                controller.close()
                for sig,handler in old.items():signal.signal(sig,handler)
        return 0
    except Exception:
        print('[vpn-recovery] operation unavailable',file=sys.stderr)
        return 2


if __name__=='__main__':sys.exit(main())
