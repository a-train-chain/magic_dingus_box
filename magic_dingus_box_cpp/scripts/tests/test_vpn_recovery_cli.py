import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from vpn_recovery.common import Paths,atomic_json
from vpn_recovery.intent import read_intent,set_intent
try:
    from vpn_recovery.cli import main
except ImportError: main=None

class CLITests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(main,'recovery CLI not implemented')
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        root=Path(self.tmp.name);self.paths=Paths(base=root,state=root/'state',runtime=root/'run',compose_lock=root/'lock')
        self.paths.services.mkdir();(self.paths.services/'.env').write_text('VPN_PORT_FORWARDING=on\n')
        self.args=['--base',str(root),'--state',str(root/'state'),'--runtime',str(root/'run'),'--compose-lock',str(root/'lock')]
    def call(self,*args):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            code=main(self.args+list(args))
        return code,output.getvalue()
    def test_intent_and_inhibit_commands(self):
        self.assertEqual(self.call('intent','enable','--reason','setup')[0],0)
        self.assertEqual(self.call('inhibit','begin','test','--owner-pid',str(os.getpid()),'--reason','maintenance')[0],0)
        self.assertIn('test',read_intent(self.paths)['inhibits'])
        self.call('intent','stop','--reason','reset')
        self.call('inhibit','end','test')
        self.assertFalse(read_intent(self.paths)['enabled'])
    def test_run_inhibited_releases_on_child_failure_and_preserves_stop(self):
        set_intent(self.paths,False,'stop')
        with patch('vpn_recovery.cli.SystemAdapters') as adapters:
            adapters.return_value.current.return_value={}
            self.assertEqual(self.call('run-inhibited','--reason','test','--',sys.executable,'-c','raise SystemExit(7)')[0],7)
        self.assertFalse(read_intent(self.paths)['enabled']);self.assertFalse(read_intent(self.paths)['inhibits'])
    def test_unprivileged_owner_bridge_keeps_child_identity_home_and_file_owner(self):
        import vpn_recovery.cli as cli
        self.assertTrue(hasattr(cli,'privileged_owner'),'privileged maintenance bridge is absent')
        deployed=self.paths.base/'magic_dingus_box_cpp/scripts/vpn_recoveryctl.py'
        deployed.parent.mkdir(parents=True);deployed.touch()
        output=self.paths.base/'child.json'
        calls=[]
        envelope={'MDB_VPN_INHIBIT_TOKEN':'fixture','MDB_VPN_INTENT_REVISION':'revision','MDB_VPN_SETTINGS_REVISION':'settings'}
        def bridge(paths,operation,token,reason,timeout,launcher=None):
            calls.append((operation,token))
            return {**envelope,'MDB_VPN_INHIBIT_TOKEN':token}
        command=[sys.executable,'-c',"import os,json,sys;json.dump({'uid':os.getuid(),'gid':os.getgid(),'home':os.environ['HOME']},open(sys.argv[1],'w'))",str(output)]
        home=str(self.paths.base/'user-home')
        with patch('vpn_recovery.cli.begin_owner',side_effect=PermissionError()), patch('vpn_recovery.cli.privileged_owner',side_effect=bridge),patch.dict(os.environ,{'HOME':home}):
            self.assertEqual(cli.run_inhibited(self.paths,command,'ota',5),0)
        data=json.loads(output.read_text())
        self.assertEqual(data,{'uid':os.getuid(),'gid':os.getgid(),'home':home})
        self.assertEqual(output.stat().st_uid,os.getuid())
        self.assertEqual([call[0] for call in calls],['begin','end','activate'])
        self.assertEqual(calls[0][1],calls[1][1])

    def test_privileged_release_survives_rollback_removing_deployed_package(self):
        import vpn_recovery.cli as cli
        deployed=self.paths.base/'magic_dingus_box_cpp/scripts'
        (deployed/'vpn_recovery').mkdir(parents=True)
        (deployed/'vpn_recoveryctl.py').touch()
        copies=[];calls=[]
        def bridge(paths,operation,token,reason,timeout,launcher=None):
            self.assertIsNotNone(launcher,'rollback-surviving helper is absent')
            launcher=Path(launcher);copies.append(launcher.parent);calls.append(operation)
            self.assertTrue(launcher.is_file())
            self.assertTrue((launcher.parent/'vpn_recovery/intent.py').is_file())
            self.assertEqual(launcher.parent.stat().st_mode & 0o777,0o700)
            self.assertFalse((launcher.parent/'services').exists())
            if operation=='end': self.assertFalse(deployed.exists())
            return {'MDB_VPN_INHIBIT_TOKEN':token,'MDB_VPN_INTENT_REVISION':'revision','MDB_VPN_SETTINGS_REVISION':'settings'}
        command=[sys.executable,'-c','import shutil,sys;shutil.rmtree(sys.argv[1])',str(deployed)]
        with patch('vpn_recovery.cli.begin_owner',side_effect=PermissionError()),patch('vpn_recovery.cli.privileged_owner',side_effect=bridge):
            self.assertEqual(cli.run_inhibited(self.paths,command,'ota',5),0)
        self.assertEqual(calls,['begin','end'])
        self.assertTrue(copies)
        self.assertTrue(all(not path.exists() for path in copies))

    def test_failed_privileged_begin_cleans_snapshot_and_never_runs_child(self):
        import vpn_recovery.cli as cli
        copies=[]
        def fail(paths,operation,token,reason,timeout,launcher=None):
            copies.append(Path(launcher).parent)
            raise RuntimeError('bridge unavailable')
        output=self.paths.base/'must-not-exist'
        with patch('vpn_recovery.cli.begin_owner',side_effect=PermissionError()),patch('vpn_recovery.cli.privileged_owner',side_effect=fail):
            with self.assertRaisesRegex(RuntimeError,'bridge unavailable'):
                cli.run_inhibited(self.paths,[sys.executable,'-c',"import pathlib,sys;pathlib.Path(sys.argv[1]).touch()",str(output)],'ota',5)
        self.assertFalse(output.exists())
        self.assertTrue(copies)
        self.assertTrue(all(not path.exists() for path in copies))

    def test_privileged_bridge_has_bounded_state_only_argv_and_sanitized_envelope(self):
        import subprocess
        from vpn_recovery.cli import privileged_owner
        token='a'*32
        envelope={'MDB_VPN_INHIBIT_TOKEN':token,'MDB_VPN_INTENT_REVISION':'b'*32,'MDB_VPN_SETTINGS_REVISION':'c'*64}
        with patch('vpn_recovery.cli.subprocess.run',return_value=subprocess.CompletedProcess([],0,json.dumps(envelope),'')) as command:
            self.assertEqual(privileged_owner(self.paths,'begin',token,'ota',5),envelope)
        argv=command.call_args.args[0]
        self.assertEqual(argv[:2],['sudo','-n'])
        self.assertIn('maintenance-owner',argv)
        self.assertEqual(argv[argv.index('--owner-pid')+1],str(os.getpid()))
        self.assertEqual(command.call_args.kwargs['timeout'],8)
        self.assertEqual(command.call_args.kwargs['cwd'],str(Path(__file__).resolve().parents[1]))
        self.assertNotIn('install',argv)

    @unittest.skipUnless(sys.platform.startswith('linux') and os.geteuid()==0 and os.environ.get('MDB_TEST_ROOT_BOUNDARY')=='1',
                         'real root/unprivileged OTA boundary requires Linux root and MDB_TEST_ROOT_BOUNDARY=1')
    def test_linux_root_owned_intent_with_real_unprivileged_ota_child(self):
        import pwd
        import shutil
        import subprocess
        user=pwd.getpwnam(os.environ.get('MDB_TEST_OTA_USER','magic'))
        self.assertNotEqual(user.pw_uid,0)
        root=self.paths.base;root.chmod(0o755)
        scripts=root/'magic_dingus_box_cpp/scripts';scripts.mkdir(parents=True)
        shutil.copytree(Path(__file__).resolve().parents[1]/'vpn_recovery',scripts/'vpn_recovery')
        launcher=scripts/'vpn_recoveryctl.py'
        # Only external Docker observations are replaced. Both the caller and
        # privileged helpers run the REAL state bridge/filesystem identities.
        launcher.write_text("from vpn_recovery import cli,runtime\nimport sys\nruntime.SystemAdapters.current=lambda *args: {}\ncli.SystemAdapters=runtime.SystemAdapters\nsys.exit(cli.main())\n")
        home=root/'home';home.mkdir();os.chown(home,user.pw_uid,user.pw_gid)
        os.chown(self.paths.services,user.pw_uid,user.pw_gid)
        os.chown(self.paths.services/'.env',user.pw_uid,user.pw_gid)
        (self.paths.services/'.env').chmod(0o600)
        set_intent(self.paths,False,'explicit stop')
        identity={'user':user.pw_uid,'group':user.pw_gid,'extra_groups':os.getgrouplist(user.pw_name,user.pw_gid)}
        env={**os.environ,'HOME':str(home),'USER':user.pw_name,'LOGNAME':user.pw_name}
        probe=subprocess.run(['sudo','-n','true'],capture_output=True,env=env,timeout=5,**identity)
        self.assertEqual(probe.returncode,0,'opt-in fixture requires the production user NOPASSWD sudo capability')
        output=home/'identity.json'
        child="import os,json,sys;json.dump({'uid':os.getuid(),'euid':os.geteuid(),'gid':os.getgid(),'home':os.environ['HOME']},open(sys.argv[1],'w'))"
        result=subprocess.run([sys.executable,str(launcher),*self.args,'run-inhibited','--reason','ota','--timeout','5','--',sys.executable,'-c',child,str(output)],
                              capture_output=True,text=True,env=env,timeout=30,**identity)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(json.loads(output.read_text()),{'uid':user.pw_uid,'euid':user.pw_uid,'gid':user.pw_gid,'home':str(home)})
        self.assertEqual(output.stat().st_uid,user.pw_uid)
        for name in ('intent.json','intent.lock'):
            self.assertEqual((self.paths.state/name).stat().st_uid,0)
            self.assertEqual((self.paths.state/name).stat().st_mode & 0o777,0o600)
        self.assertFalse(read_intent(self.paths)['enabled'])
        self.assertFalse(read_intent(self.paths)['inhibits'])

    def test_stale_status_never_ready(self):
        atomic_json(self.paths.state/'status.json',{'ready':True,'checked_at':'2000-01-01T00:00:00+00:00','traffic_verified':True})
        code,out=self.call('status');self.assertEqual(code,0)
        self.assertFalse(json.loads(out)['ready'])
    def test_supported_policy_and_invalid_fails_distinctly(self):
        self.assertEqual(self.call('supported')[0],0)
        (self.paths.services/'.env').write_text('VPN_SERVICE_PROVIDER=custom\n')
        self.assertEqual(self.call('supported')[0],1)
        (self.paths.services/'.env').write_text('VPN_TYPE=invalid\n')
        self.assertEqual(self.call('supported')[0],2)

if __name__=='__main__':unittest.main()
