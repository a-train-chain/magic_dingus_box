#!/usr/bin/env python3
"""Attended scoped deployment handoff, after source sync and secure backup.

Not called by the common hook or OTA: OLD updaters must keep their existing
verified-kiosk-start -> installed-helper-refresh ordering. No package copy.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from vpn_recovery.common import Paths, load_config
from vpn_recovery.control import ensure_control_credentials
from vpn_recovery.intent import read_intent, read_json
from vpn_recovery.lifecycle import activate, fingerprint, provision

UNIT='gluetun-cascade-restart.service'
# Only this change's installed entrypoints. These are the same mappings used by
# update.sh:refresh_out_of_tree_files; all other helpers still use that normal
# delivery path. Source-only helpers and the package stay in the deployed tree.
HELPERS={'gluetun_cascade_restart.sh':'gluetun_cascade_restart.sh',
         'qbit_port_sync.sh':'qbit-port-sync.sh'}


def activate_scoped(paths, scripts, *, installed=Path('/usr/local/bin'), runner=subprocess.run):
    scripts,installed=Path(scripts),Path(installed)
    def run(argv, timeout=3, required=True):
        result=runner(argv,capture_output=True,text=True,timeout=timeout,check=False)
        if required and result.returncode: raise RuntimeError('scoped activation command failed')
        return result
    if read_intent(paths).get('inhibits'):
        raise RuntimeError('maintenance in progress; defer scoped activation')
    configured=(paths.services/'.env').is_file()
    if configured:
        config=load_config(paths)
        if not (config['provider']=='protonvpn' and config['type']=='wireguard' and config['forwarding']):
            raise RuntimeError('scoped activation requires native Proton forwarding')
    was_active=run(['systemctl','is-active','--quiet',UNIT],required=False).returncode==0
    if was_active:
        run(['systemctl','stop',UNIT],45)
        if run(['systemctl','is-active','--quiet',UNIT],required=False).returncode==0:
            raise RuntimeError('watcher did not quiesce')
    # On any failure, leave the watcher quiesced for attended investigation;
    # never automatically launch a partly installed or unauthenticated owner.
    provision(paths)
    if configured:
        ensure_control_credentials(paths)
        if activate(paths): raise RuntimeError('credential activation failed')
    run(['bash',str(scripts/'setup_memory_tuning.sh')],180)
    for source,target in HELPERS.items():
        destination=installed/target
        if not destination.is_file(): continue  # preserve games-only boxes
        payload=(scripts/source).read_bytes()
        fd,name=tempfile.mkstemp(prefix='.'+target+'.',dir=installed)
        try:
            with os.fdopen(fd,'wb') as stream:
                os.fchmod(stream.fileno(),0o755);stream.write(payload);stream.flush();os.fsync(stream.fileno())
            os.replace(name,destination)
        finally:
            Path(name).unlink(missing_ok=True)
    if not was_active or not configured:
        return {'watcher':'inactive'}
    wanted=fingerprint(scripts)
    record=paths.runtime/'running-fingerprint.json'
    record.unlink(missing_ok=True)  # an old record cannot certify this handoff
    try:
        run(['systemctl','start',UNIT],45)
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            pid_text=run(['systemctl','show','--property=MainPID','--value',UNIT]).stdout.strip()
            if pid_text.isdecimal() and int(pid_text)>0:
                pid=int(pid_text)
                evidence=read_json(record)
                command=run(['ps','-ww','-p',str(pid),'-o','args='],required=False).stdout.strip()
                if (evidence.get('pid')==pid and evidence.get('fingerprint')==wanted
                        and str(scripts/'vpn_recoveryctl.py') in command and command.endswith(' run')
                        and run(['systemctl','is-active','--quiet',UNIT],required=False).returncode==0):
                    return {'watcher':'native','pid':pid,'fingerprint':wanted}
            time.sleep(.2)
        raise RuntimeError('native watcher command/fingerprint verification failed')
    except BaseException:
        run(['systemctl','stop',UNIT],45,required=False)
        raise


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--opt-in-scoped-activation',action='store_true',required=True)
    parser.parse_args()
    if os.geteuid()!=0: parser.error('root required')
    scripts=Path(__file__).resolve().parent
    try:
        result=activate_scoped(Paths(base=scripts.parent.parent),scripts)
        print(json.dumps(result,sort_keys=True))
        return 0
    except Exception:
        print('Scoped VPN activation failed; inspect the watcher state before continuing.',file=__import__('sys').stderr)
        return 1

if __name__=='__main__':raise SystemExit(main())
