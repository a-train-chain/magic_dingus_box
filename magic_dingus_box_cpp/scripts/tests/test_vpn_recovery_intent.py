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

    def test_completed_operation_history_is_bounded_and_not_an_inhibit(self):
        from unittest.mock import patch
        intent.set_intent(self.paths,True,'setup')
        with patch('vpn_recovery.intent.time.time',return_value=1800000000):
            for index in range(70):
                token='operation_'+str(index)
                intent.inhibit(self.paths,token,os.getpid(),'maintenance',generation={'container_id':'a'*64})
                intent.release_inhibit(self.paths,token)
            state=intent.read_intent(self.paths)
            self.assertEqual(len(state.get('completed_operations',[])),64)
            self.assertFalse(state['inhibits'])
            self.assertTrue(intent.may_recover(self.paths))
        with patch('vpn_recovery.intent.time.time',return_value=1800000601):
            intent.set_intent(self.paths,False,'operator stop')
            self.assertEqual(intent.read_intent(self.paths).get('completed_operations'),[])
            self.assertFalse(intent.may_recover(self.paths))

    def test_completed_history_cannot_enable_new_stop_or_match_another_container(self):
        from unittest.mock import patch
        current_boot=intent.boot_id()
        with patch('vpn_recovery.intent.time.time',return_value=1800000000), patch('vpn_recovery.intent.boot_id',return_value=current_boot):
            intent.set_intent(self.paths,True,'setup')
            intent.inhibit(self.paths,'history',os.getpid(),'recreate',generation={'container_id':'a'*64})
        with patch('vpn_recovery.intent.time.time',return_value=1800000002):
            intent.set_intent(self.paths,False,'explicit stop')
            intent.release_inhibit(self.paths,'history')
        self.assertTrue(intent.owns_stop(self.paths,'a'*64,1800000001,1800000010))
        self.assertFalse(intent.owns_stop(self.paths,'b'*64,1800000001,1800000010))
        self.assertFalse(intent.owns_stop(self.paths,'a'*64,1800000003,1800000010))
        self.assertFalse(intent.owns_stop(self.paths,'a'*64,1800000001,1800000603))
        self.assertFalse(intent.may_recover(self.paths,actual={'running':True,'unit_active':True,'configured':True}))

    def test_revision_fence(self):
        before = intent.settings_revision(self.paths)
        (self.paths.services/'.env').write_text('VPN_COUNTRIES=Canada\n')
        self.assertNotEqual(before, intent.settings_revision(self.paths))

if __name__ == '__main__': unittest.main()
