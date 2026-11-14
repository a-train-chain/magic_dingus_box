import json
import os
import sys
import tempfile
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vpn_recovery.common import Paths
try:
    from vpn_recovery import intent
except ImportError:
    intent = None

class IntentTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(intent, 'managed intent not implemented')
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.paths = Paths(base=root, state=root/'state', runtime=root/'run', compose_lock=root/'lock')
        self.paths.services.mkdir(); (self.paths.services/'.env').write_text('WIREGUARD_PRIVATE_KEY=x\n')
    def test_missing_corrupt_and_explicit_stop_fail_closed(self):
        self.assertFalse(intent.may_recover(self.paths))
        intent.set_intent(self.paths, True, 'setup')
        self.assertTrue(intent.may_recover(self.paths))
        intent.set_intent(self.paths, False, 'operator_stop')
        self.assertFalse(intent.may_recover(self.paths))
        (self.paths.state/'intent.json').write_text('{broken')
        self.assertFalse(intent.may_recover(self.paths))
    def test_inhibit_has_process_identity_and_generation(self):
        intent.set_intent(self.paths, True, 'setup')
        intent.inhibit(self.paths, 'test_token', os.getpid(), 'maintenance', generation={'container_id': 'a'})
        self.assertFalse(intent.may_recover(self.paths))
        token = intent.read_intent(self.paths)['inhibits']['test_token']
        self.assertTrue(token['owner_start']); self.assertTrue(token['boot_id'])
        self.assertEqual(token['generation'], {'container_id': 'a'})
        intent.release_inhibit(self.paths, 'test_token')
        self.assertTrue(intent.may_recover(self.paths))
    def test_stop_wins_release_and_nested_maintenance(self):
        intent.set_intent(self.paths, True, 'setup')
        for token in ['first', 'second']:
            intent.inhibit(self.paths, token, os.getpid(), 'maintenance')
        intent.release_inhibit(self.paths, 'first')
        self.assertFalse(intent.may_recover(self.paths))
        intent.set_intent(self.paths, False, 'reset')
        intent.release_inhibit(self.paths, 'second')
        self.assertFalse(intent.may_recover(self.paths))
    def test_dead_owner_reconciles_actual_state_never_enables_stop(self):
        intent.set_intent(self.paths, True, 'setup')
        intent.inhibit(self.paths, 'dead', 99999999, 'maintenance')
        self.assertTrue(intent.may_recover(self.paths, actual={'running': True, 'unit_active': True, 'configured': True}))
        intent.inhibit(self.paths, 'dead', 99999999, 'maintenance')
        self.assertFalse(intent.may_recover(self.paths, actual={'running': False, 'unit_active': False, 'configured': True}))
    def test_adopt_only_absent_running_managed_installation(self):
        self.assertFalse(intent.adopt_running(self.paths, {'running': False, 'unit_active': True, 'configured': True}))
        self.assertTrue(intent.adopt_running(self.paths, {'running': True, 'unit_active': True, 'configured': True}))
        intent.set_intent(self.paths, False, 'stop')
        self.assertFalse(intent.adopt_running(self.paths, {'running': True, 'unit_active': True, 'configured': True}))
    def test_expired_live_owner_reconciles_instead_of_inhibiting_forever(self):
        from vpn_recovery.common import atomic_json
        intent.set_intent(self.paths,True,'setup')
        intent.inhibit(self.paths,'expired',os.getpid(),'maintenance')
        state=intent.read_intent(self.paths);state['inhibits']['expired']['expires_at']=0
        atomic_json(self.paths.state/'intent.json',state)
        self.assertTrue(intent.may_recover(self.paths,actual={'running':True,'configured':True,'unit_active':True}))

    def test_revision_fence(self):
        before = intent.settings_revision(self.paths)
        (self.paths.services/'.env').write_text('VPN_COUNTRIES=Canada\n')
        self.assertNotEqual(before, intent.settings_revision(self.paths))

if __name__ == '__main__': unittest.main()
