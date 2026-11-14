"""Stable deployed-tree lifecycle entry point. No credential-bearing output."""
import argparse
from datetime import datetime, timezone
import json
import os
import signal
import subprocess
import sys
import time
import uuid

from .common import Paths, bounded_lock, load_config
from .intent import (set_intent, inhibit, release_inhibit, read_intent, read_json,
                     settings_revision, adopt_running)
from .runtime import Controller, SystemAdapters


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


def run_inhibited(paths, command, reason, timeout):
    if not command or not 0<timeout<=86400:
        raise ValueError('invalid bounded command')
    token=uuid.uuid4().hex
    actual=SystemAdapters(paths).current(time.monotonic()+2)
    generation={k:actual.get(k) for k in ('container_id','started_at','pid')} if actual.get('running') else None
    state=inhibit(paths,token,os.getpid(),reason,generation=generation,ttl_s=min(86400,timeout+60))
    env={**os.environ,'MDB_VPN_INHIBIT_TOKEN':token,'MDB_VPN_INTENT_REVISION':state['revision'],
         'MDB_VPN_SETTINGS_REVISION':settings_revision(paths)}
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
        release_inhibit(paths,token)


def parser():
    p=argparse.ArgumentParser(description='Managed VPN recovery')
    p.add_argument('--base',default='/opt/magic_dingus_box')
    p.add_argument('--services');p.add_argument('--state',default='/var/lib/magic-dingus/vpn_recovery')
    p.add_argument('--runtime',default='/run/mdb-vpn-recovery');p.add_argument('--compose-lock',default='/run/lock/mdb-compose.lock')
    sub=p.add_subparsers(dest='command',required=True)
    run=sub.add_parser('run');run.add_argument('--once',action='store_true')
    for name in ('status','sync-port','catalog-refresh','supported','check-operation','check-fence'):sub.add_parser(name)
    intent=sub.add_parser('intent');intent.add_argument('operation',choices=['enable','stop','adopt-running']);intent.add_argument('--reason',default='operator')
    inh=sub.add_parser('inhibit');inh.add_argument('operation',choices=['begin','end']);inh.add_argument('token')
    inh.add_argument('--owner-pid',type=int);inh.add_argument('--reason',default='maintenance')
    wrapped=sub.add_parser('run-inhibited');wrapped.add_argument('--reason',default='maintenance')
    wrapped.add_argument('--timeout',type=float,default=1800);wrapped.add_argument('argv',nargs=argparse.REMAINDER)
    return p


def main(argv=None):
    args=parser().parse_args(argv)
    paths=Paths(base=args.base,services=args.services,state=args.state,runtime=args.runtime,compose_lock=args.compose_lock)
    try:
        if args.command=='supported':
            c=load_config(paths)
            return 0 if c['provider']=='protonvpn' and c['type']=='wireguard' and c['forwarding'] else 1
        if args.command=='status':
            print(json.dumps(read_status(paths),sort_keys=True));return 0
        if args.command=='check-fence':return 0 if controller_fence_allowed(paths) else 3
        if args.command=='check-operation':return 0 if operation_allowed(paths) else 3
        if args.command=='intent':
            if args.operation=='adopt-running':
                a=SystemAdapters(paths)
                return 0 if adopt_running(paths,a.current(time.monotonic()+2)) or read_intent(paths).get('enabled') else 3
            set_intent(paths,args.operation=='enable',args.reason);return 0
        if args.command=='inhibit':
            if args.operation=='begin':
                if not args.owner_pid:raise ValueError('owner PID required')
                inhibit(paths,args.token,args.owner_pid,args.reason)
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
