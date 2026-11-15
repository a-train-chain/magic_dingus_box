"""Sanitized snapshot and bounded native lifecycle bridge for existing routes."""
from datetime import datetime
import fcntl
import json
import math
import os
from pathlib import Path
import subprocess
import time
import uuid

STATUS_PATH = Path('/var/lib/magic-dingus/vpn_recovery/status.json')
COMPOSE_LOCK = Path('/run/lock/mdb-compose.lock')


def native(env):
    return ((env.get('VPN_SERVICE_PROVIDER') or 'protonvpn').strip().lower() == 'protonvpn'
            and (env.get('VPN_TYPE') or 'wireguard').strip().lower() == 'wireguard'
            and (env.get('VPN_PORT_FORWARDING') or 'off').strip().lower() == 'on')


def snapshot(path=None, *, now=None):
    """Public health only: never credentials, durable history or entire settings."""
    now=time.time() if now is None else now
    try:
        with Path(path or STATUS_PATH).open('rb') as stream: raw=stream.read(262145)
        data=json.loads(raw) if len(raw)<=262144 else {}
        if not isinstance(data,dict): data={}
    except (OSError,ValueError,RecursionError): data={}
    try: age=now-datetime.fromisoformat(data['checked_at']).timestamp();fresh=0<=age<=45
    except (KeyError,ValueError,TypeError,OverflowError): age=None;fresh=False
    effective=data.get('effective') if isinstance(data.get('effective'),dict) else {}
    catalog=data.get('catalog') if isinstance(data.get('catalog'),dict) else {}
    def text(value): return value[:160] if isinstance(value,str) else None
    def number(value):
        try: return value if type(value) in (int,float) and math.isfinite(value) and value>=0 else None
        except OverflowError: return None
    port=data.get('forwarded_port')
    return {'fresh':fresh,'age_s':number(age),'checked_at':text(data.get('checked_at')),
            'ready':fresh and data.get('ready') is True,
            'state':text(data.get('state')) if fresh else 'Observation stale',
            'reason':text(data.get('state')) if fresh else 'No current VPN observation',
            'country':text(effective.get('country')),'hostname':text(effective.get('hostname')),
            'traffic_verified':fresh and data.get('traffic_verified') is True,
            'forwarded_port':port if fresh and type(port) is int and 1<=port<=65535 else None,
            'qbit_synced':fresh and data.get('qbit_synced') is True,
            'dependents_ok':fresh and data.get('dependents_ok') is True,
            'last_success':text(data.get('last_success')),
            'last_recovery_reason':text(data.get('last_recovery_reason')),
            'catalog_age_s':number(catalog.get('age_s')),'next_retry':text(data.get('next_retry'))}


class Maintenance:
    """Hold compose before intent; process death cannot leave an immortal token.

    Old/rollback trees without the CLI retain the legacy route implementation.
    A present but broken native CLI fails closed before any config mutation.
    """
    def __init__(self, ctl, base, *, reason='settings', compose_lock=None, wait_s=2, lock_held=False):
        self.ctl,self.base=Path(ctl),Path(base)
        self.path=Path(compose_lock or COMPOSE_LOCK);self.wait_s=wait_s
        self.lock_held=lock_held
        self.reason=reason;self.token=uuid.uuid4().hex;self.fd=None;self.started=False
    def command(self,*args):
        result=subprocess.run(['sudo','-n','python3',str(self.ctl),'--base',str(self.base),*args],
                              capture_output=True,text=True,timeout=6)
        if result.returncode: raise RuntimeError('VPN maintenance unavailable; retry after current maintenance')
    def __enter__(self):
        if not self.ctl.is_file(): return self
        try:
            if not self.lock_held:
                self.fd=self.path.open('rb')
            deadline=time.monotonic()+self.wait_s
            while not self.lock_held:
                try: fcntl.flock(self.fd,fcntl.LOCK_EX|fcntl.LOCK_NB);break
                except BlockingIOError:
                    if time.monotonic()>=deadline: raise RuntimeError('compose lock unavailable')
                    time.sleep(.025)
            self.started=True
            self.command('inhibit','begin',self.token,'--owner-pid',str(os.getpid()),'--reason',self.reason)
            self.started=True
            return self
        except OSError:
            self.__exit__(None,None,None)
            raise RuntimeError('compose lock unavailable') from None
        except BaseException:
            self.__exit__(None,None,None);raise
    def release(self):
        # Handoff before launching a detached owner, while this request still
        # holds compose. The child captures a fresh intent revision.
        if self.started:
            self.command('inhibit','end',self.token)
            self.started=False
    def __exit__(self,*args):
        try:
            self.release()
        finally:
            if self.fd is not None: self.fd.close();self.fd=None
        return False
