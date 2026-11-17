"""Local mocks only: no Docker, systemd, credentials, namespace or network I/O."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from unittest.mock import Mock
from contextlib import nullcontext

source = Path(__file__).with_name('check_network_fault.py')
loader = importlib.util.spec_from_file_location('network_fault', source)
fault = importlib.util.module_from_spec(loader)
loader.loader.exec_module(fault)

GEN = {'container_id': 'a'*64, 'pid': 42, 'started_at': 'start', 'tunnel_ifindex': 3}
ITEM = {'destination': '10.2.0.1/32', 'natpmp': True, 'kind': 'unreachable', 'priority': 99, 'table': 20001}
SPEC = {'generation': GEN, 'process': {'pid_startticks': 123, 'netns_inode': 456}, 'rules': [ITEM]}


class OwnershipTests(unittest.TestCase):
    def test_insertion_pins_fd_between_strong_docker_fences(self):
        with patch.object(fault, 'namespace_fence', return_value=42) as fence, patch.object(fault, 'owned_namespace', return_value=nullcontext(99)) as pinned, patch.object(fault, 'run') as run:
            fault.net_ip(SPEC, ['route', 'add', 'unreachable', '10.2.0.1/32', 'table', '20001'], 999999999)
            self.assertEqual(fence.call_count, 2)
            pinned.assert_called_once_with(SPEC)
            self.assertIn('--net=/proc/self/fd/99', run.call_args.args[0])
            self.assertNotIn('--target', run.call_args.args[0])
            self.assertEqual(run.call_args.kwargs['pass_fds'], (99,))

    def test_insertion_fd_inode_recheck_rejects_before_dispatch(self):
        with patch.object(fault, 'namespace_fence', return_value=42), patch.object(fault, 'proc_identity', return_value=SPEC['process']), patch.object(fault.os, 'open', return_value=99), patch.object(fault.os, 'fstat', return_value=SimpleNamespace(st_ino=999)), patch.object(fault.os, 'close') as close, patch.object(fault, 'run') as run:
            with self.assertRaisesRegex(fault.FaultError, 'namespace_unproven'):
                fault.net_ip(SPEC, ['rule', 'add'], 999999999)
            run.assert_not_called()
            close.assert_called_once_with(99)

    def test_namespace_reuse_rejected_before_ip(self):
        same = {key: GEN[key] for key in ('container_id', 'pid', 'started_at')}
        with patch.object(fault, 'identity', return_value=same), patch.object(fault, 'proc_identity', return_value={'pid_startticks': 999, 'netns_inode': 456}), patch.object(fault, 'run') as run:
            with self.assertRaisesRegex(fault.FaultError, 'identity_changed'):
                fault.net_ip(SPEC, ['rule', 'show'], 999999999)
            run.assert_not_called()

    def test_start_changed_rejected(self):
        observed = {key: GEN[key] for key in ('container_id', 'pid', 'started_at')}
        observed['started_at'] = 'later'
        with patch.object(fault, 'identity', return_value=observed), patch.object(fault, 'proc_identity') as proc:
            with self.assertRaises(fault.FaultError):
                fault.namespace_fence(SPEC, 999999999)
            proc.assert_not_called()

    def test_tun0_change_does_not_block_same_namespace_cleanup(self):
        # Cleanup deliberately uses CID/PID/start/startticks/netns, not tun0.
        same = {key: GEN[key] for key in ('container_id', 'pid', 'started_at')}
        with patch.object(fault, 'identity', return_value=same), patch.object(fault, 'proc_identity', return_value=SPEC['process']):
            self.assertEqual(fault.namespace_fence(SPEC, 999999999), 42)

    def test_partial_insert_cleanup_and_missing_object_are_idempotent(self):
        with patch.object(fault, 'owned_namespace', return_value=nullcontext(99)), patch.object(fault, 'objects', side_effect=[(False, True), (False, False)]), patch.object(fault, 'cleanup_ip') as ip:
            result = fault.cleanup(SPEC, 999999999)
            self.assertEqual(result['cleanup'], 'exact_objects_absent')
            self.assertEqual(ip.call_count, 1)
            self.assertEqual(ip.call_args.args[2], ['route', 'del', 'unreachable', '10.2.0.1/32', 'table', '20001'])

    def test_cleanup_rule_before_route_and_readback_required(self):
        with patch.object(fault, 'owned_namespace', return_value=nullcontext(99)), patch.object(fault, 'objects', side_effect=[(True, True), (False, False)]), patch.object(fault, 'cleanup_ip') as ip:
            fault.cleanup(SPEC, 999999999)
            self.assertEqual(ip.call_args_list[0].args[2], ['rule', 'del', 'priority', '99', 'to', '10.2.0.1/32', 'ipproto', 'udp', 'dport', '5351', 'lookup', '20001'])
            self.assertEqual(ip.call_args_list[1].args[2][0:2], ['route', 'del'])
        with patch.object(fault, 'owned_namespace', return_value=nullcontext(99)), patch.object(fault, 'objects', side_effect=[(True, True), (True, False)]), patch.object(fault, 'cleanup_ip'):
            with self.assertRaisesRegex(fault.FaultError, 'absence_unproven'):
                fault.cleanup(SPEC, 999999999)

    def test_foreign_tuple_is_not_deleted(self):
        rules = [{'priority': 99, 'src': 'all', 'dst': '10.2.0.1', 'table': 20001, 'ipproto': 'udp', 'dport': 53}]
        routes = [{'table': 20001, 'type': 'unreachable', 'dst': '10.2.0.1'}]
        with patch.object(fault, 'owned_namespace', return_value=nullcontext(99)), patch.object(fault, 'cleanup_ip', side_effect=[SimpleNamespace(stdout=json.dumps(rules)), SimpleNamespace(stdout=json.dumps(routes))]) as ip:
            with self.assertRaisesRegex(fault.FaultError, 'foreign_contents'):
                fault.cleanup(SPEC, 999999999)
            self.assertEqual(ip.call_count, 2)
            self.assertTrue(all('del' not in call.args[2] for call in ip.call_args_list))

    def test_unrelated_foreign_objects_preserved(self):
        rules = [{'priority': 1, 'src': 'all', 'dst': '1.1.1.1', 'table': 20002},
                 {'priority': 99, 'src': 'all', 'dst': '10.2.0.1', 'table': 20001, 'ipproto': 'udp', 'dport': 5351}]
        routes = [{'table': 20002, 'type': 'blackhole', 'dst': '1.1.1.1'},
                  {'table': 20001, 'type': 'unreachable', 'dst': '10.2.0.1'}]
        with patch.object(fault, 'net_ip', side_effect=[SimpleNamespace(stdout=json.dumps(rules)), SimpleNamespace(stdout=json.dumps(routes))]):
            self.assertEqual(fault.objects(SPEC, ITEM, 999999999), (True, True))

    def test_natpmp_foreign_extra_selector_never_deleted(self):
        base = {'priority': 99, 'src': 'all', 'dst': '10.2.0.1', 'table': 20001, 'ipproto': 'udp', 'dport': 5351}
        route = {'table': 20001, 'type': 'unreachable', 'dst': '10.2.0.1'}
        selectors = {'fwmark': '0x1', 'fwmask': '0xff', 'sport': 123,
                     'iif': 'eth0', 'oif': 'tun0', 'uidrange': '0-100',
                     'not': True, 'l3mdev': True, 'tos': '0x10',
                     'suppress_prefixlength': 0, 'goto': 123, 'unknown_selector': 'x'}
        for field, value in selectors.items():
            def execute(fd, spec, args, end, **kwargs):
                self.assertNotIn('del', args, 'foreign selector reached deletion dispatch')
                return SimpleNamespace(stdout=json.dumps([{**base, field: value}] if 'rule' in args else [route]))
            with self.subTest(selector=field), patch.object(fault, 'owned_namespace', return_value=nullcontext(99)), patch.object(fault, 'cleanup_ip', side_effect=execute) as ip:
                with self.assertRaisesRegex(fault.FaultError, 'foreign_contents'):
                    fault.cleanup(SPEC, 999999999)
                self.assertEqual(ip.call_count, 2)
                self.assertTrue(all('del' not in call.args[2] for call in ip.call_args_list))

    def test_rule_protocol_origin_metadata_is_harmless(self):
        rule = {'priority': 99, 'src': 'all', 'dst': '10.2.0.1', 'table': 20001, 'ipproto': 'udp', 'dport': 5351, 'protocol': 'boot'}
        route = {'table': 20001, 'type': 'unreachable', 'dst': '10.2.0.1'}
        with patch.object(fault, 'net_ip', side_effect=[SimpleNamespace(stdout=json.dumps([rule])), SimpleNamespace(stdout=json.dumps([route]))]):
            self.assertEqual(fault.objects(SPEC, ITEM, 999999999), (True, True))

    def test_iproute2_exact_dport_mask_is_owned(self):
        # Observed on the Pi's iproute2 6.15 disposable capability readback.
        rule = {'priority': 99, 'src': 'all', 'dst': '10.2.0.1', 'table': '20001',
                'ipproto': 'udp', 'dport': 5351, 'dport_mask': '0xffff'}
        route = {'table': '20001', 'type': 'unreachable', 'dst': '10.2.0.1', 'flags': []}
        with patch.object(fault, 'net_ip', side_effect=[SimpleNamespace(stdout=json.dumps([rule])), SimpleNamespace(stdout=json.dumps([route]))]):
            self.assertEqual(fault.objects(SPEC, ITEM, 999999999), (True, True))

    def test_partial_dport_mask_is_foreign(self):
        for mask in ('0xff00', '0x0', '255'):
            with self.subTest(mask=mask):
                rule = {'priority': 99, 'src': 'all', 'dst': '10.2.0.1', 'table': 20001,
                        'ipproto': 'udp', 'dport': 5351, 'dport_mask': mask}
                route = {'table': 20001, 'type': 'unreachable', 'dst': '10.2.0.1'}
                with patch.object(fault, 'net_ip', side_effect=[SimpleNamespace(stdout=json.dumps([rule])), SimpleNamespace(stdout=json.dumps([route]))]):
                    with self.assertRaisesRegex(fault.FaultError, 'foreign_contents'):
                        fault.objects(SPEC, ITEM, 999999999)

    def test_unknown_generation_or_port_interrupts_claimed_zero(self):
        for mode in ('port-loss', 'first-lease'):
            for unknown_generation, unknown_port in ((None, None), (None, 0)):
                with self.subTest(mode=mode, port=unknown_port):
                    helper = object.__new__(fault.LiveFault)
                    helper.mode, helper.paths = mode, SimpleNamespace()
                    helper.gateway, helper.armed = '10.2.0.1', True
                    helper.end = fault.time.monotonic()+300
                    helper.initial = helper.target = SimpleNamespace(endpoint_ip='8.8.8.8', hostname='a')
                    candidate = SimpleNamespace(endpoint_ip='8.8.4.4', hostname='b')
                    helper.candidates, helper.durable = [helper.initial, candidate], {'round_attempts': []}
                    helper.initial_failures = []
                    helper.spec, helper.manifest = {'preserved': {}, 'kiosk': 'same'}, Path('/tmp/mock-evidence/manifest.json')
                    helper.preflight = helper.arm = helper.add = helper.remove = Mock()
                    helper.gen = helper.first_lease = Mock(return_value=GEN)
                    helper.ready = Mock(return_value={'round_attempts': ['b']})
                    policy = {'generation': GEN, 'port_lost': 1, 'started': 1}
                    zero = {'generation': GEN, 'port': 0, 'endpoint': '8.8.8.8', 'traffic': True, 'traffic_generation': GEN, 'policy': policy, 'failures': [], 'controller_saved_wall': 1000, 'controller_saved_mono': 1000}
                    unknown = dict(zero, generation=unknown_generation, port=unknown_port, policy={})
                    changed = dict(zero, generation=dict(GEN, tunnel_ifindex=4), endpoint='8.8.4.4', port=1234, policy={}, failures=[{'hostname': 'a', 'endpoint_ip': '8.8.8.8', 'failed_at': 200}])
                    helper.sample = Mock(side_effect=[zero, unknown, zero, changed])
                    with patch.object(fault.time, 'sleep'), patch.object(fault, 'preserved', return_value={}), patch.object(fault, 'run', return_value=SimpleNamespace(stdout='same')):
                        result = helper.execute()
                    self.assertFalse(result['ok'])
                    self.assertEqual(result['validity'], 'unproven')
                    self.assertEqual(result['reason'], 'unknown_sample_invalidates_continuous_zero')

    def test_unknown_sample_after_recorded_action_does_not_void_proof(self):
        # Live port-loss row 2026-11-15: the sample that first showed the
        # controller's failure entry was taken while its rotation replaced
        # tun0. Unknown samples before the action still fail the row.
        for mode in ('port-loss', 'first-lease'):
            with self.subTest(mode=mode):
                helper = object.__new__(fault.LiveFault)
                helper.mode, helper.paths = mode, SimpleNamespace()
                helper.gateway, helper.armed = '10.2.0.1', True
                helper.end = fault.time.monotonic()+300
                helper.initial = helper.target = SimpleNamespace(endpoint_ip='8.8.8.8', hostname='a')
                candidate = SimpleNamespace(endpoint_ip='8.8.4.4', hostname='b')
                helper.candidates, helper.durable = [helper.initial, candidate], {'round_attempts': []}
                helper.initial_failures = []
                helper.spec, helper.manifest = {'preserved': {}, 'kiosk': 'same'}, Path('/tmp/mock-evidence/manifest.json')
                helper.preflight = helper.arm = helper.add = helper.remove = Mock()
                helper.gen = helper.first_lease = Mock(return_value=GEN)
                helper.ready = Mock(return_value={'round_attempts': ['b']})
                policy = {'generation': GEN, 'port_lost': 1, 'started': 1}
                zero = {'generation': GEN, 'port': 0, 'endpoint': '8.8.8.8', 'traffic': True, 'traffic_generation': GEN, 'policy': policy,
                        'failures': [], 'controller_saved_wall': 1000, 'controller_saved_mono': 1000, 'mono': 150}
                acted = [{'hostname': 'a', 'endpoint_ip': '8.8.8.8', 'failed_at': 200}]
                unknown = dict(zero, generation=None, port=None, policy={}, failures=acted, mono=201)
                changed = dict(zero, generation=dict(GEN, tunnel_ifindex=4), endpoint='8.8.4.4', port=1234, policy={}, failures=acted, mono=210)
                helper.sample = Mock(side_effect=[zero, unknown, changed])
                with patch.object(fault.time, 'sleep'), patch.object(fault, 'preserved', return_value={}), patch.object(fault, 'run', return_value=SimpleNamespace(stdout='same')):
                    result = helper.execute()
                self.assertTrue(result['ok'], result)
                self.assertEqual(result['controller_loss_to_action_s'], 199)

    def test_blackhole_selector_does_not_widen_natpmp(self):
        peer = dict(ITEM, destination='8.8.8.8/32', natpmp=False, kind='blackhole')
        self.assertNotIn('dport', fault.rule_args(peer, 'add'))
        self.assertEqual(fault.rule_args(ITEM, 'add')[-6:], ['ipproto', 'udp', 'dport', '5351', 'lookup', '20001'])

    def test_watchdog_cleanup_after_gluetun_restart_is_a_clean_exit(self):
        # magicpi5 2026-11-15: the controller restarted Gluetun mid-test; the
        # 550 s watchdog then crashed on /proc/<old pid>/stat and left a
        # failed systemd unit (verify_box FAILs failed units).
        def gone(pid):
            raise FileNotFoundError('/proc/%s/stat' % pid)
        with patch.object(fault, 'proc_identity', side_effect=gone), \
                patch.object(fault.Path, 'exists', return_value=False), \
                patch.object(fault, 'run') as run:
            result = fault.cleanup(SPEC, 999999999)
        self.assertEqual(result['cleanup'], 'original_process_gone')
        run.assert_not_called()

    def test_cleanup_never_touches_a_reused_pid(self):
        with patch.object(fault, 'proc_identity', return_value={'pid_startticks': 999, 'netns_inode': 1}), \
                patch.object(fault.Path, 'exists', return_value=True), patch.object(fault, 'run') as run:
            with self.assertRaises(fault.FaultError):
                fault.cleanup(SPEC, 999999999)
        run.assert_not_called()

    def test_watchdog_explicit_oneshot_and_finite_early_timer(self):
        helper = object.__new__(fault.LiveFault)
        helper.mode, helper.scripts = 'fallback', '/tmp/code/scripts'
        helper.spec, helper.manifest = {}, Path('/run/mdb-vpn-network-1234567812345678/manifest.json')
        helper.save = lambda: None
        with patch.object(fault, 'run', return_value=SimpleNamespace(stdout='123456789')) as run:
            helper.arm()
            argv = run.call_args_list[0].args[0]
            self.assertIn('--property=Type=oneshot', argv)
            self.assertIn('--property=TimeoutStartSec=35s', argv)
            self.assertIn('--on-active=550s', argv)
            self.assertLess(helper.spec['insertion_deadline_mono']-fault.time.monotonic(), 546)
            self.assertTrue(helper.armed)

    def test_expired_command_never_dispatches(self):
        with patch.object(fault.subprocess, 'run') as run:
            with self.assertRaisesRegex(fault.FaultError, 'aggregate_deadline'):
                fault.run(['forbidden'], fault.time.monotonic()-1)
            run.assert_not_called()

    def test_cleanup_docker_down_uses_pinned_original_namespace_only(self):
        with patch.object(fault, 'proc_identity', return_value=SPEC['process']), patch.object(fault.os, 'open', return_value=99), patch.object(fault.os, 'fstat', return_value=SimpleNamespace(st_ino=456)), patch.object(fault.os, 'close') as close, patch.object(fault, 'identity', side_effect=RuntimeError('Docker unavailable')) as inspect, patch.object(fault, 'objects', side_effect=[(True, True), (False, False)]), patch.object(fault, 'run') as run:
            fault.cleanup(SPEC, 999999999)
            inspect.assert_not_called()
            for call in run.call_args_list:
                self.assertIn('--net=/proc/self/fd/99', call.args[0])
                self.assertEqual(call.kwargs['pass_fds'], (99,))
                self.assertNotIn('--target', call.args[0])
            close.assert_called_once_with(99)

    def test_cleanup_pin_recheck_rejects_replaced_process(self):
        with patch.object(fault, 'proc_identity', side_effect=[SPEC['process'], {'pid_startticks': 999, 'netns_inode': 456}]), patch.object(fault.os, 'open', return_value=99), patch.object(fault.os, 'fstat', return_value=SimpleNamespace(st_ino=456)), patch.object(fault.os, 'close') as close, patch.object(fault, 'run') as run:
            with self.assertRaisesRegex(fault.FaultError, 'namespace_unproven'):
                fault.cleanup(SPEC, 999999999)
            run.assert_not_called()
            close.assert_called_once_with(99)


class HostGuardTests(unittest.TestCase):
    """Live faults run only on the box the operator names (magicpi5 unless
    --box names another, e.g. a bench Pi 4B). No wildcard, no inference."""

    def test_default_box_is_magicpi5_only(self):
        self.assertTrue(fault.host_allowed('magicpi5', None))
        self.assertFalse(fault.host_allowed('magicpi4', None))
        self.assertFalse(fault.host_allowed('magicpi5-1a2b', None))

    def test_named_box_must_match_exactly(self):
        self.assertTrue(fault.host_allowed('magicpi4', 'magicpi4'))
        # Naming another box never also allows the default one.
        self.assertFalse(fault.host_allowed('magicpi5', 'magicpi4'))
        self.assertFalse(fault.host_allowed('magicpi4b', 'magicpi4'))
        self.assertFalse(fault.host_allowed('MAGICPI4', 'magicpi4'))

    def test_box_name_is_a_plain_hostname(self):
        for bad in ('', '*', 'magicpi*', '.', 'a b', 'magic.local', '-x', 'x' * 64, 'magicpi4\n'):
            with self.subTest(bad=bad):
                self.assertFalse(fault.host_allowed(bad, bad))

    def test_main_refuses_a_live_mode_on_the_wrong_box(self):
        argv = ['check_network_fault.py', '--opt-in-network-fault', '--mode', 'port-loss',
                '--gateway', '10.2.0.1', '--box', 'magicpi4']
        with patch.object(fault.sys, 'argv', argv), \
             patch.object(fault.os, 'geteuid', return_value=0), \
             patch.object(fault.sys, 'platform', 'linux'), \
             patch.object(fault, 'dependencies'), \
             patch.object(fault.socket, 'gethostname', return_value='magicpi5'), \
             patch.object(fault, 'LiveFault') as live:
            with self.assertRaises(SystemExit) as caught:
                fault.main()
        self.assertIn('host identity mismatch', str(caught.exception))
        live.assert_not_called()


if __name__ == '__main__':
    unittest.main()
