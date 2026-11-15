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
                   candidate_available=True, wan=True, selection_verified=True,
                   namespaces_match=True, dependents_ok=True)
        obs.update(changes)
        before = copy.deepcopy(self.state)
        result = decide(obs, self.state, CONFIG, t, 1800000000+t)
        self.assertEqual(before, self.state, 'policy must be pure')
        self.assertLessEqual(result['next_due'], t+10)
        self.state = result['next_state']
        return result
    def _rotate_on_traffic_loss(self):
        """Ready, then dead traffic from t=10: the rotation is issued at t=70."""
        self.step(0)
        self.assertEqual(self.step(10, traffic=False)['action'], 'none')
        self.assertEqual(self.step(70, traffic=False)['action'], 'rotate_server')
        self.assertEqual(self.state['candidate_until'], 190)

    def test_dead_candidate_is_judged_after_one_loss_window(self):
        # The candidate's own new tunnel carries no traffic at all: judge it
        # 60 s after the rotation, not after the full 120 s grace that exists
        # for a slow first port lease.
        self._rotate_on_traffic_loss()
        self.assertEqual(self.step(129, traffic=False, port=0)['reason'], 'Waiting for candidate')
        self.assertEqual(self.step(130, traffic=False, port=0)['action'], 'rotate_server')
        self.assertEqual(self.state['candidate_until'], 250)

    def test_candidate_not_yet_on_its_own_tunnel_keeps_the_full_grace(self):
        self._rotate_on_traffic_loss()
        result = self.step(130, traffic=False, port=0, selection_verified=False)
        self.assertEqual((result['action'], result['reason']), ('none', 'Waiting for candidate'))

    def test_candidate_with_traffic_but_no_port_keeps_the_full_grace(self):
        self._rotate_on_traffic_loss()
        for t in (130, 160, 189):
            result = self.step(t, traffic=True, port=0)
            self.assertEqual((result['action'], result['reason']), ('none', 'Waiting for candidate'), t)

    def test_unknown_candidate_traffic_never_counts_as_dead(self):
        self._rotate_on_traffic_loss()
        for t in (100, 130, 150):
            result = self.step(t, traffic=None, port=0)
            self.assertEqual((result['action'], result['reason']), ('none', 'Waiting for candidate'), t)

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

    def test_unknown_or_unavailable_dependents_never_prove_ready(self):
        for namespaces,available in [(None,False),(None,True),(True,False)]:
            with self.subTest(namespaces=namespaces,available=available):
                result=self.step(0,namespaces_match=namespaces,dependents_ok=available)
                self.assertNotEqual(result['reason'],'Ready')
                self.assertEqual(result['action'],'none')
        self.assertEqual(self.step(10)['reason'],'Ready')

    def test_relink_and_start_precede_unreachable_qbit_sync(self):
        self.assertEqual(self.step(0,needs_relink=True,qbit_synced=False)['action'],'relink_dependents')
        self.assertEqual(self.step(10,needs_converge=True,qbit_synced=False)['action'],'converge_dependents')
        self.assertEqual(self.step(20,relink_waiting=True,qbit_synced=False)['action'],'none')
        self.assertNotEqual(self.step(30,relink_waiting=True)['reason'],'Ready')

    def test_dependents_only_after_traffic_and_no_soft_relink(self):
        self.assertEqual(self.step(0, needs_relink=True)['action'], 'relink_dependents')
        self.assertEqual(self.step(10, needs_converge=True)['action'], 'converge_dependents')
        self.assertEqual(self.step(20, traffic=False, needs_relink=True)['action'], 'none')

    # Live magicpi5 2026-11-15: a blocked Proton endpoint makes Gluetun restart
    # its own tunnel every ~6 s (tun0 ifindex 4 -> 30 in 2.5 min). Each
    # replacement inside the SAME container must not restart the failure clocks
    # or the candidate grace, or a dead server is never rotated away from.
    def flapping(self, t, ifindex, unknown=False, **changes):
        gen = {**GEN, 'tunnel_ifindex': ifindex}
        values = dict(generation=gen, traffic=None if unknown else False, port=None if unknown else 0)
        values.update(changes)
        return self.step(t, **values)

    def test_autonomous_tunnel_flapping_does_not_postpone_traffic_failure(self):
        self.step(0)
        actions = []
        for n, t in enumerate(range(10, 300, 10)):
            # Every third sample is unknown: the tunnel changed mid-observation.
            result = self.flapping(t, 3+n, unknown=n % 3 == 2)
            actions.append((t, result['action']))
            if result['action'] != 'none':
                break
        self.assertEqual(actions[-1], (70, 'rotate_server'))

    def test_flapping_candidate_gets_one_grace_window_not_a_rolling_one(self):
        # Gluetun restart-loops a dead candidate (a new tun0 every tick). The
        # tunnel churn must not restart the clock: the candidate is judged once,
        # one loss window after the rotation (dead-candidate rule), not after a
        # grace that rolls forward with every tunnel.
        self.step(0)
        for n, t in enumerate(range(10, 70, 10)):
            self.flapping(t, 3+n)
        self.assertEqual(self.flapping(70, 20)['action'], 'rotate_server')
        actions = [(t, self.flapping(t, 21+n)['action']) for n, t in enumerate(range(80, 200, 10))]
        self.assertEqual([a for a in actions if a[1] != 'none'][0], (130, 'rotate_server'))

    def test_flapping_first_lease_grace_is_not_restarted_by_each_tunnel(self):
        for n, t in enumerate(range(0, 200, 10)):
            result = self.flapping(t, 3+n, traffic=True)
            if result['action'] != 'none':
                break
        self.assertEqual((t, result['action']), (120, 'rotate_server'))

    def test_missing_tun0_mid_flap_waits_for_tunnel_instead_of_restarting(self):
        # observe() reports a momentarily absent tun0 as generation=None with
        # only top-level container identity. Inside the same container that
        # is one more restart of a flapping tunnel, not a dead control path.
        self.step(0)
        for n, t in enumerate(range(10, 70, 10)):
            self.flapping(t, 3+n)
        gap = dict(generation=None, traffic=False, port=None, control_available=False,
                   container_id=GEN['container_id'], pid=GEN['pid'], started_at=GEN['started_at'])
        self.assertEqual(self.step(70, **gap)['action'], 'none')
        self.assertEqual(self.flapping(80, 20)['action'], 'rotate_server')

    def test_tun0_absent_beyond_tolerance_still_restarts(self):
        self.step(0)
        for n, t in enumerate(range(10, 70, 10)):
            self.flapping(t, 3+n)
        gap = dict(generation=None, traffic=False, port=None, control_available=False,
                   container_id=GEN['container_id'], pid=GEN['pid'], started_at=GEN['started_at'])
        # Last tunnel seen at 60: 90 is exactly the 30 s tolerance, 100 is past it.
        actions = [(t, self.step(t, **gap)['action']) for t in range(70, 110, 10)]
        self.assertEqual(actions, [(70, 'none'), (80, 'none'), (90, 'none'), (100, 'restart_container')])

    def test_unconfirmed_control_read_failure_mid_flap_rotates(self):
        self.step(0)
        for n, t in enumerate(range(10, 70, 10)):
            self.flapping(t, 3+n)
        result = self.flapping(70, 20, port=None, control_available=False)
        self.assertEqual(result['action'], 'rotate_server')

    def test_dead_control_api_escalates_from_failed_rotation_to_restart(self):
        dead = dict(traffic=False, port=None, control_available=False)
        self.step(0)
        for t in range(10, 70, 10):
            self.step(t, **dead)
        self.assertEqual(self.step(70, **dead)['action'], 'rotate_server')
        # The rotation PUT could not reach the control server: api_failed.
        for t in range(80, 190, 10):
            self.assertEqual(self.step(t, api_failed=True, **dead)['action'], 'none', t)
        self.assertEqual(self.step(190, api_failed=True, **dead)['action'], 'restart_container')

    def test_operator_selection_change_gets_first_lease_grace_and_clean_losses(self):
        # Live first-lease row 2026-11-15: two earlier servers' lease losses
        # plus the operator-selected server's first zero made "3 losses in
        # 600 s" fire 6 s after the operator's PUT. A maintenance-owned change
        # (runtime reports it as selection_reset) starts a fresh evaluation.
        self.step(0)
        for t in (10, 30):
            self.step(t, port=0); self.step(t+10)
        self.assertEqual(len(self.state['losses']), 2)
        self.step(50, generation={**GEN, 'tunnel_ifindex': 3}, selection_reset=True, port=0)
        self.assertEqual(self.state['losses'], [])
        for t in range(60, 170, 10):
            self.assertEqual(self.flapping(t, 3, traffic=True)['action'], 'none', t)
        self.assertEqual(self.flapping(170, 3, traffic=True)['action'], 'rotate_server')

    def test_selection_reset_while_inhibited_still_applies(self):
        self.step(0)
        self.step(10, port=0)
        self.step(20, allowed=False, selection_reset=True, port=0)
        self.assertNotIn('port_lost', self.state)
        self.assertFalse(self.state['had_port'])

    def test_new_container_start_still_resets_clocks_and_grants_grace(self):
        self.step(0)
        for t in range(10, 50, 10):
            self.flapping(t, 3)
        replaced = {**GEN, 'pid': 2, 'started_at': 'restart', 'tunnel_ifindex': 2}
        for t in range(50, 170, 10):
            self.assertEqual(self.step(t, generation=replaced, traffic=False, port=0)['action'], 'none', t)
        self.assertEqual(self.step(170, generation=replaced, traffic=False, port=0)['action'], 'rotate_server')

    def test_long_unknown_gap_discards_loss_evidence(self):
        self.step(0)
        self.flapping(10, 3)
        for t in range(20, 70, 10):
            self.flapping(t, 3, unknown=True)
        self.assertEqual(self.flapping(70, 3)['action'], 'none')
        self.assertEqual(self.flapping(129, 3)['action'], 'none')
        self.assertEqual(self.flapping(130, 3)['action'], 'rotate_server')

if __name__ == '__main__': unittest.main()
