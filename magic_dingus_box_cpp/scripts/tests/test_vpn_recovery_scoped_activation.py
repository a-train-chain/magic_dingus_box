"""Installed OLD/source NEW migration, using real wrapper and native process."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from vpn_recovery.common import Paths
from vpn_recovery.intent import read_intent, set_intent
from vpn_recovery.lifecycle import fingerprint

SCRIPTS=Path(__file__).resolve().parents[1]

class ActivationTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.source=self.root/'magic_dingus_box_cpp/scripts'
        self.source.mkdir(parents=True)
        for name in ('vpn_recoveryctl.py','gluetun_cascade_restart.sh','qbit_port_sync.sh'):
            shutil.copyfile(SCRIPTS/name,self.source/name)
        shutil.copytree(SCRIPTS/'vpn_recovery',self.source/'vpn_recovery')
        (self.source/'setup_memory_tuning.sh').touch()
        self.paths=Paths(base=self.root,state=self.root/'state',runtime=self.root/'run',compose_lock=self.root/'lock')
        self.paths.services.mkdir();(self.paths.services/'.env').write_text('VPN_PORT_FORWARDING=on\nVPN_COUNTRIES=Canada\n')
        (self.paths.services/'docker-compose.yml').touch()
        self.installed=self.root/'installed';self.installed.mkdir()
        self.watcher=self.installed/'gluetun_cascade_restart.sh'
        self.watcher.write_text('#!/bin/sh\nexec sleep 120\n')
        (self.installed/'qbit-port-sync.sh').write_text('old port sync')
        self.bin=self.root/'bin';self.bin.mkdir()
        for name,body in [('docker','exit 1'),('systemctl','echo inactive; exit 3')]:
            p=self.bin/name;p.write_text('#!/bin/sh\n'+body+'\n');p.chmod(0o755)
        self.env=dict(os.environ,PATH=str(self.bin)+':'+os.environ['PATH'],VPN_RECOVERY_SOURCE_DIR=str(self.source),
                      COMPOSE_DIR=str(self.paths.services),MDB_VPN_STATE=str(self.paths.state),
                      MDB_VPN_RUNTIME=str(self.paths.runtime),MDB_COMPOSE_LOCK=str(self.paths.compose_lock))
        self.child=None;self.events=[];self.addCleanup(self.stop)
        self.start()
    def start(self):
        self.child=subprocess.Popen(['bash',str(self.watcher)],env=self.env,stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL,start_new_session=True)
    def stop(self):
        if self.child and self.child.poll() is None:
            os.killpg(self.child.pid,signal.SIGTERM);self.child.wait(timeout=5)
    def runner(self,args,**kwargs):
        self.events.append(args)
        if args[0]=='systemctl':
            action=args[1]
            if action=='is-active':return subprocess.CompletedProcess(args,0 if self.child and self.child.poll() is None else 3,'','')
            if action=='stop':self.stop()
            if action=='start':self.start()
            if action=='show':return subprocess.CompletedProcess(args,0,str(self.child.pid),'')
            return subprocess.CompletedProcess(args,0,'','')
        if args[:1]==['bash']:
            self.assertIsNotNone(self.child.poll(),'hook must run with watcher quiesced')
            if (self.paths.services/'.env').is_file():
                self.assertTrue((self.paths.services/'config/gluetun/auth/recovery.key').is_file())
            return subprocess.CompletedProcess(args,0,'','')
        return subprocess.run(args,**kwargs)
    def activate(self):
        path=SCRIPTS/'activate_vpn_recovery.py'
        spec=importlib.util.spec_from_file_location('scoped_activation',path)
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with patch.dict(os.environ,self.env):
            return module.activate_scoped(self.paths,self.source,installed=self.installed,runner=self.runner)
    def test_installed_old_source_new_activates_real_native_command_and_fingerprint(self):
        result=self.activate()
        self.assertEqual(self.watcher.read_bytes(),(self.source/self.watcher.name).read_bytes())
        self.assertEqual(result['watcher'],'native')
        self.assertEqual(result['fingerprint'],fingerprint(self.source))
        command=subprocess.check_output(['ps','-ww','-p',str(self.child.pid),'-o','args='],text=True)
        self.assertIn(str(self.source/'vpn_recoveryctl.py'),command)
        self.assertTrue(command.rstrip().endswith(' run'))
        self.assertEqual((self.installed/'qbit-port-sync.sh').read_bytes(),(self.source/'qbit_port_sync.sh').read_bytes())
    def test_inactive_watcher_and_explicit_stop_are_preserved(self):
        self.stop();set_intent(self.paths,False,'operator stop')
        result=self.activate()
        self.assertEqual(result['watcher'],'inactive')
        self.assertFalse(read_intent(self.paths)['enabled'])
        self.assertFalse(any(a[:2]==['systemctl','start'] for a in self.events))
    def test_failed_quiesce_cannot_copy_or_activate_native_helper(self):
        original=self.runner
        def fail_stop(args,**kwargs):
            if args[:2]==['systemctl','stop']:
                return subprocess.CompletedProcess(args,1,'','')
            return original(args,**kwargs)
        self.runner=fail_stop
        with self.assertRaises(RuntimeError):self.activate()
        self.assertIn('exec sleep',self.watcher.read_text())
        self.assertFalse(any(a[:1]==['bash'] for a in self.events))

    def test_native_verification_rejects_wrong_mainpid_command_and_stops_watcher(self):
        original=self.runner
        def wrong_command(args,**kwargs):
            if args[:1]==['ps']:
                evidence=json.loads((self.paths.runtime/'running-fingerprint.json').read_text())
                self.assertEqual(evidence['fingerprint'],fingerprint(self.source))
                self.assertEqual(evidence['pid'],self.child.pid)
                return subprocess.CompletedProcess(args,0,'bash legacy-watcher','')
            result=original(args,**kwargs)
            if args[:2]==['systemctl','start']:
                import time
                until=time.monotonic()+3
                while not (self.paths.runtime/'running-fingerprint.json').exists() and time.monotonic()<until:
                    time.sleep(.02)
            return result
        self.runner=wrong_command
        # Advance only the verification clock, leaving real CLI/intent clocks
        # intact. One read of the actual native record cannot certify a wrong
        # MainPID command even when the source fingerprint matches.
        times=iter([0,0,16])
        path=SCRIPTS/'activate_vpn_recovery.py'
        spec=importlib.util.spec_from_file_location('scoped_activation',path)
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        from types import SimpleNamespace
        module.time=SimpleNamespace(monotonic=lambda:next(times,16),sleep=lambda _:None)
        with patch.dict(os.environ,self.env), self.assertRaises(RuntimeError):
            module.activate_scoped(self.paths,self.source,installed=self.installed,runner=self.runner)
        self.assertIsNotNone(self.child.poll())

    def test_unconfigured_install_is_not_activated(self):
        self.stop();(self.paths.services/'.env').unlink()
        result=self.activate()
        self.assertEqual(result['watcher'],'inactive')
        self.assertFalse(read_intent(self.paths))

if __name__=='__main__':unittest.main()
