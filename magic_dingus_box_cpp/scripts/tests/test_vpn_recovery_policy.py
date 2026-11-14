"""Fake-clock decisions: no transport or machine dependencies."""
import copy
import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    from vpn_recovery.policy import decide
except ImportError:
    decide = None

CONFIG = {'provider': 'protonvpn', 'type': 'wireguard', 'forwarding': True}
GEN = {'container_id': 'a'*64, 'started_at': 'start', 'pid': 1, 'tunnel_ifindex': 2}

class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(decide, 'recovery policy not implemented')
        self.state = {}
    def step(self, t, **changes):
        obs = dict(allowed=True, running=True, generation=GEN, traffic=True,
                   traffic_at=t, port=12345, port_at=t, qbit_synced=True,
                   candidate_available=True, wan=True, selection_verified=True)
        obs.update(changes)
        before = copy.deepcopy(self.state)
        result = decide(obs, self.state, CONFIG, t, 1800000000+t)
        self.assertEqual(before, self.state, 'policy must be pure')
        self.assertLessEqual(result['next_due'], t+10)
        self.state = result['next_state']
        return result
    def test_first_port_deadline_and_one_transient(self):
        self.assertEqual(self.step(0, port=0)['action'], 'none')
        self.assertEqual(self.step(119, port=0)['action'], 'none')
        self.assertEqual(self.step(120, port=0)['action'], 'rotate_server')
    def test_sustained_loss_exact_threshold(self):
        self.step(0)
        self.assertEqual(self.step(10, port=0)['action'], 'none')
        self.assertEqual(self.step(99, port=0)['action'], 'none')
        self.assertEqual(self.step(100, port=0)['action'], 'rotate_server')
    def test_three_flaps_in_ten_minutes(self):
        self.step(0)
        for t in [10, 30]:
            self.assertEqual(self.step(t, port=0)['action'], 'none')
            self.step(t+10)
        self.assertEqual(self.step(50, port=0)['action'], 'rotate_server')
    def test_old_flaps_expire(self):
        self.step(0)
        self.step(10, port=0); self.step(20)
        self.step(30, port=0); self.step(40)
        self.assertEqual(self.step(611, port=0)['action'], 'none')
    def test_traffic_confirmation_and_stale_evidence(self):
        self.step(0, traffic=False)
        self.assertEqual(self.step(59, traffic=False)['action'], 'none')
        self.assertEqual(self.step(60, traffic=False)['action'], 'rotate_server')
        self.assertNotEqual(self.step(61, traffic=True, traffic_at=0)['reason'], 'Ready')
    def test_qbit_failure_never_rotates(self):
        for t in range(0, 2000, 10):
            self.assertEqual(self.step(t, qbit_synced=False)['action'], 'sync_port')
    def test_wan_outage_never_rotates(self):
        for t in [0, 60, 120, 600]:
            self.assertEqual(self.step(t, traffic=False, port=0, wan=False)['action'], 'none')
    def test_stopped_and_ambiguous_do_not_resurrect(self):
        for changes in [dict(allowed=False), dict(running=False, exit_code=137), dict(running=None)]:
            self.assertEqual(self.step(1000, **changes)['action'], 'none')
    def test_oom_and_restart_limit(self):
        self.assertEqual(self.step(0, running=False, affirmative_failure=True)['action'], 'restart_container')
        self.assertEqual(self.step(599, running=False, affirmative_failure=True)['action'], 'none')
        self.assertEqual(self.step(600, running=False, affirmative_failure=True)['action'], 'restart_container')
    def test_candidate_grace_and_autonomous_generation_change(self):
        self.step(0, port=0)
        self.step(120, port=0)
        self.assertEqual(self.step(239, port=0)['action'], 'none')
        self.assertEqual(self.step(240, port=0)['action'], 'rotate_server')
        newer = {**GEN, 'tunnel_ifindex': 3}
        self.assertEqual(self.step(250, generation=newer, port=0)['action'], 'none')
    def test_healthy_fallback_stays_put(self):
        for t in [0, 1000, 100000]:
            self.assertEqual(self.step(t, effective_country='United States')['action'], 'none')
    def test_progressive_backoff_no_immediate_recycle(self):
        self.step(0, port=0)
        t = 120
        for delay in [120, 300, 600, 600]:
            r = self.step(t, port=0, candidate_available=False)
            self.assertEqual(r['action'], 'none')
            self.assertEqual(self.state['backoff_until'], t+delay)
            self.assertEqual(self.step(t+delay-1, port=0)['action'], 'none')
            r = self.step(t+delay, port=0, candidate_available=True)
            self.assertEqual(r['action'], 'rotate_server')
            self.assertTrue(r.get('new_round'))
            t += delay+120
    def test_unattested_catalog_or_api_refusal_requires_bounded_restart(self):
        for flag in ['catalog_unattested', 'api_failed']:
            self.state = {}
            self.step(0, port=0)
            self.assertEqual(self.step(120, port=0, **{flag: True})['action'], 'restart_container')
    def test_corrupt_state_clock_jumps_and_stale_generation(self):
        for state in [None, [], {'version': 1, 'losses': 'bad'}, {'version': 2, 'started': float('nan')}]:
            self.state = state
            self.assertEqual(self.step(0, port=0)['action'], 'none')
        self.step(10, port=0)
        self.assertEqual(self.step(5, port=0)['action'], 'none')
        self.assertNotEqual(self.step(1000, port=12345, port_generation={'other': 1})['reason'], 'Ready')
    def test_control_read_failure_is_confirmed_and_qbit_is_independent(self):
        self.step(0)
        self.assertEqual(self.step(10,port=None,control_available=False)['action'],'none')
        self.assertEqual(self.step(99,port=None,control_available=False)['action'],'none')
        self.assertEqual(self.step(100,port=None,control_available=False)['action'],'restart_container')
    def test_missing_tunnel_after_confirmation_restarts_instead_of_unusable_put(self):
        self.step(0,traffic=False,generation=None,port=None)
        self.assertEqual(self.step(60,traffic=False,generation=None,port=None)['action'],'restart_container')

    def test_dependents_only_after_traffic_and_no_soft_relink(self):
        self.assertEqual(self.step(0, needs_relink=True)['action'], 'relink_dependents')
        self.assertEqual(self.step(10, needs_converge=True)['action'], 'converge_dependents')
        self.assertEqual(self.step(20, traffic=False, needs_relink=True)['action'], 'none')

if __name__ == '__main__': unittest.main()
