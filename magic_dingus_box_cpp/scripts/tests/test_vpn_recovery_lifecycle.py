import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vpn_recovery.common import Paths
from vpn_recovery.intent import read_intent, set_intent, inhibit, release_inhibit
try:
    from vpn_recovery.lifecycle import manage, activate, fingerprint
except ImportError:
    manage = activate = fingerprint = None

class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(manage, 'lifecycle integration is absent')
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.paths = Paths(base=root, state=root/'state', runtime=root/'run', compose_lock=root/'compose.lock')
        self.paths.services.mkdir(); (self.paths.services/'.env').write_text('VPN_PORT_FORWARDING=on\nVPN_COUNTRIES=Canada\n')
        (self.paths.services/'docker-compose.yml').touch(); self.paths.compose_lock.touch()
        self.calls = []
        patched=patch("vpn_recovery.lifecycle.current", return_value={})
        self.current=patched.start(); self.addCleanup(patched.stop)
    def command(self, argv, **kw):
        self.calls.append(argv)
        return type('Result', (), {'returncode':0, 'stdout':'', 'stderr':''})()
    def test_stop_records_intent_before_lock_and_never_resurrects(self):
        set_intent(self.paths, True, 'running')
        self.paths.compose_lock.unlink()
        self.assertNotEqual(manage(self.paths, 'stop', runner=self.command), 0)
        self.assertFalse(read_intent(self.paths)['enabled']); self.assertFalse(self.calls)
        self.assertFalse(read_intent(self.paths)['inhibits'])
    def test_start_restart_stop_are_inhibited_and_serialized(self):
        def run(argv, **kw):
            self.assertTrue(read_intent(self.paths)['inhibits'])
            self.calls.append(argv)
            return self.run_result()
        self.assertEqual(manage(self.paths, 'start', runner=run), 0)
        self.assertTrue(read_intent(self.paths)['enabled'])
        self.assertEqual(manage(self.paths, 'restart', runner=run), 0)
        self.assertEqual(manage(self.paths, 'stop', runner=run), 0)
        self.assertFalse(read_intent(self.paths)['enabled'])
        self.assertFalse(read_intent(self.paths)['inhibits'])
        self.assertEqual([x[-1] for x in self.calls], ['-d','down','-d','down'])
    def run_result(self):
        return type('Result', (), {'returncode':0, 'stdout':'', 'stderr':''})()
    def test_stop_during_restart_prevents_up(self):
        def run(argv, **kw):
            self.calls.append(argv); set_intent(self.paths, False, 'operator stop'); return self.run_result()
        self.assertNotEqual(manage(self.paths, 'restart', runner=run), 0)
        self.assertEqual(len(self.calls), 1)
        self.assertFalse(read_intent(self.paths)['enabled'])
    def test_overlapping_maintenance_defers_without_mutation(self):
        set_intent(self.paths, True, 'running')
        inhibit(self.paths, 'other', os.getpid(), 'OTA')
        self.assertNotEqual(manage(self.paths, 'start', runner=self.command), 0)
        self.assertFalse(self.calls); self.assertEqual(set(read_intent(self.paths)['inhibits']), {'other'})
    def test_unconfigured_start_does_not_enable(self):
        (self.paths.services/'.env').unlink()
        self.assertNotEqual(manage(self.paths, 'start', runner=self.command), 0)
        self.assertFalse(read_intent(self.paths).get('enabled')); self.assertFalse(self.calls)
    def test_activate_adopts_only_proven_running_and_coalesces_auth_reload(self):
        actual = {'running':True,'unit_active':True,'configured':True,'container_id':'a','pid':2,'started_at':'x'}
        self.current.return_value=actual
        self.assertEqual(activate(self.paths, actual=actual, runner=self.command), 0)
        self.assertTrue(read_intent(self.paths)['enabled']); self.assertEqual(len(self.calls),1)
        self.assertEqual(activate(self.paths, actual=actual, runner=self.command),0)
        self.assertEqual(len(self.calls),1)
        set_intent(self.paths, False, 'operator stop')
        (self.paths.services/'config/gluetun/auth/config.toml').write_text('changed')
        self.assertEqual(activate(self.paths, actual=actual, runner=self.command),0)
        self.assertEqual(len(self.calls),1); self.assertFalse(read_intent(self.paths)['enabled'])
    def test_auth_activation_rechecks_current_stop_under_compose_lock(self):
        actual={'running':True,'unit_active':True,'configured':True,'container_id':'a','pid':2,'started_at':'x'}
        self.current.return_value={'running':False}
        self.assertNotEqual(activate(self.paths,actual=actual,runner=self.command),0)
        self.assertFalse(self.calls)

    def test_activate_does_not_adopt_stopped_or_corrupt_intent(self):
        for actual in ({'running':False,'unit_active':True,'configured':True}, {'running':True,'unit_active':False,'configured':True}):
            activate(self.paths,actual=actual,runner=self.command)
            self.assertFalse(read_intent(self.paths).get('enabled')); self.assertFalse(self.calls)
        self.paths.state.mkdir(exist_ok=True); (self.paths.state/'intent.json').write_text('broken')
        activate(self.paths,actual={'running':True,'unit_active':True,'configured':True},runner=self.command)
        self.assertFalse(read_intent(self.paths).get('enabled')); self.assertFalse(self.calls)
    def test_explicit_setup_reconciles_dead_owner_but_never_live_owner(self):
        from vpn_recovery.intent import enable_if_idle
        set_intent(self.paths,False,'stop')
        inhibit(self.paths,'dead',99999999,'interrupted setup')
        self.assertTrue(enable_if_idle(self.paths,'setup'))
        self.assertFalse(read_intent(self.paths)['inhibits'])
        inhibit(self.paths,'live',os.getpid(),'OTA')
        self.assertFalse(enable_if_idle(self.paths,'setup'))
        self.assertEqual(set(read_intent(self.paths)['inhibits']),{'live'})

    def test_missing_compose_lock_publishes_no_catalog_submission(self):
        self.paths.compose_lock.unlink()
        with patch('vpn_recovery.lifecycle.record_catalog_submission') as record:
            self.assertNotEqual(manage(self.paths,'start',runner=self.command),0)
        record.assert_not_called()
        self.assertFalse(self.calls)

    def test_fresh_boot_provisions_same_shared_inode_without_enable(self):
        from vpn_recovery import lifecycle
        self.assertTrue(hasattr(lifecycle,'provision'),'boot lock provision is absent')
        self.paths.compose_lock.unlink()
        lifecycle.provision(self.paths)
        inode=self.paths.compose_lock.stat().st_ino
        lifecycle.provision(self.paths)
        self.assertEqual(inode,self.paths.compose_lock.stat().st_ino)
        self.assertEqual(self.paths.compose_lock.stat().st_mode & 0o777,0o666)
        self.assertFalse(read_intent(self.paths))
    def test_different_boxes_generate_distinct_keys_after_clone_cleanup(self):
        from vpn_recovery.control import ensure_control_credentials
        ensure_control_credentials(self.paths)
        first=(self.paths.services/'config/gluetun/auth/recovery.key').read_bytes()
        import shutil
        shutil.rmtree(self.paths.services/'config/gluetun/auth')
        ensure_control_credentials(self.paths)
        second=(self.paths.services/'config/gluetun/auth/recovery.key').read_bytes()
        self.assertNotEqual(first,second)
    def test_fingerprint_changes_for_module_only_update(self):
        scripts=self.paths.base/'scripts'; (scripts/'vpn_recovery').mkdir(parents=True)
        (scripts/'gluetun_cascade_restart.sh').write_text('same'); (scripts/'vpn_recovery/runtime.py').write_text('one')
        first=fingerprint(scripts)
        (scripts/'vpn_recovery/runtime.py').write_text('two')
        self.assertNotEqual(first,fingerprint(scripts))

if __name__=='__main__': unittest.main()
