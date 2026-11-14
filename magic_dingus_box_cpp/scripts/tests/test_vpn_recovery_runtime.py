import copy
import fcntl
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from concurrent.futures import Future
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vpn_recovery.common import Paths, atomic_json, bounded_lock
from vpn_recovery.catalog import validate_catalog, eligible_candidates
from vpn_recovery.intent import set_intent, inhibit, release_inhibit, settings_revision
try:
    from vpn_recovery.runtime import Controller, parse_endpoint, restore_policy
except ImportError:
    Controller = None
GEN = {'container_id': 'a'*64, 'pid': 42, 'started_at': '2026-11-14T00:00:00Z', 'tunnel_ifindex': 2}

class Immediate:
    def submit(self, fn, *args):
        future = Future()
        try: future.set_result(fn(*args))
        except Exception as exc: future.set_exception(exc)
        return future
    def shutdown(self, **kwargs): pass

class FakeAdapters:
    def __init__(self, paths):
        self.paths, self.now, self.calls, self.before_action = paths, 0., [], None
        self.wall = lambda: 1800000000+self.now
        self.mono = lambda: self.now
        self.observation = dict(running=True, unit_active=True, configured=True, generation=GEN,
            traffic=True, port=0, qbit_synced=False, wan=True, endpoint='84.20.16.29',
            dependents_ok=True, needs_converge=False, explicit_stop=False)
        raw = json.loads((Path(__file__).parent/'fixtures/vpn_recovery/proton-v4-sample.json').read_text())
        self.catalog = validate_catalog(raw, self.wall())
        self.control, self.ports, self.store = self, self, self
        self.fail_api, self.attested = False, True
    def config(self): return {'provider':'protonvpn','type':'wireguard','forwarding':True,'preferred_countries':['Canada'],'fallback_countries':['United States']}
    def observe(self, *, deadline, traffic_due):
        return copy.deepcopy({**self.observation, 'traffic_at': self.now, 'port_at': self.now, 'port_generation': self.observation['generation']})
    def current(self, deadline): return copy.deepcopy(self.observation)
    def observe_generation(self, container_id=None, *, deadline=None): return copy.deepcopy(self.observation['generation'])
    def put(self, route, payload, cid, *, precondition, deadline=None):
        if self.before_action: self.before_action()
        with bounded_lock(self.paths.compose_lock, .01):
            if not precondition(): raise RuntimeError('precondition')
            if self.fail_api: raise RuntimeError('api refused secret=DO_NOT_LEAK')
            self.calls.append(('put', payload))
        return 'updated'
    def sync(self, expected_generation=None, *, deadline=None, precondition=None):
        with bounded_lock(self.paths.compose_lock, .01):
            if precondition and not precondition(): return {'status':'deferred'}
            self.calls.append(('sync', expected_generation))
        return {'status':'synced', 'generation':expected_generation}
    def container_action(self, action, observation, deadline):
        self.calls.append((action, observation['generation']))
        return True
    def load(self): return self.catalog
    def refresh(self, now, force=False):
        self.calls.append(('refresh', force)); return {'status':'failed'}
    def attest_catalog(self, generation, snapshot, deadline): return self.attested
    def executor(self): return Immediate()

class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(Controller, 'recovery runtime not implemented')
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.paths = Paths(base=root, state=root/'state', runtime=root/'run', compose_lock=root/'lock')
        self.paths.compose_lock.touch(); self.paths.services.mkdir(); (self.paths.services/'.env').write_text('VPN_COUNTRIES=Canada\n')
        set_intent(self.paths, True, 'setup')
        self.a = FakeAdapters(self.paths)
        self.c = Controller(self.paths, self.a)
        self.addCleanup(self.c.close)
        self.c.loaded = self.a.catalog
        self.c.loaded_container = self.c.container_key(GEN)
        self.c.next_refresh = 100000
    def tick(self, t):
        self.a.now=t
        return self.c.tick()
    def finish(self):
        if self.c.pending: self.c.pending.result(timeout=2)
    def test_lost_events_recover_with_current_state_and_soft_tunnel_does_not_relink(self):
        self.tick(0); self.tick(120); self.finish()
        self.assertEqual(self.a.calls[0][0], 'put')
        self.assertNotIn('relink_dependents', [x[0] for x in self.a.calls])
        self.a.observation.update(generation={**GEN, 'tunnel_ifindex':3}, port=4567, qbit_synced=True)
        self.tick(130)
        self.assertFalse(any(x[0]=='restart_container' for x in self.a.calls))
    def test_settings_change_inside_lock_defers(self):
        self.tick(0)
        self.a.before_action=lambda: (self.paths.services/'.env').write_text('VPN_COUNTRIES=Netherlands\n')
        self.tick(120); self.finish(); self.tick(130)
        self.assertEqual(self.a.calls, [])
    def test_busy_lock_never_mutates(self):
        self.tick(0)
        with bounded_lock(self.paths.compose_lock, 0):
            self.tick(120); self.finish()
        self.assertFalse(self.a.calls)
    def test_maintenance_and_operator_stop_win(self):
        import os
        self.tick(0)
        inhibit(self.paths, 'maintenance', os.getpid(), 'setup')
        self.tick(120)
        self.assertFalse(self.a.calls)
        release_inhibit(self.paths, 'maintenance')
        self.a.observation['explicit_stop']=True
        self.tick(130)
        self.assertFalse(self.a.calls)
    def test_own_operation_stop_event_does_not_disable_intent(self):
        from vpn_recovery.intent import may_recover
        self.c.operation = {'generation': GEN, 'started_wall': self.a.wall(), 'until_wall': self.a.wall()+60}
        self.a.observation.update(explicit_stop=True, stop_generation=GEN, stop_at=self.a.wall()+1)
        self.tick(0)
        self.assertTrue(may_recover(self.paths))
    def test_api_refusal_escalates_once_and_redacts(self):
        self.a.fail_api=True
        self.tick(0); self.tick(120); self.finish()
        self.tick(240); self.finish(); self.tick(250)
        self.assertEqual(sum(c[0]=='restart_container' for c in self.a.calls), 1)
        for t in [360, 480, 600, 839]: self.tick(t)
        self.assertEqual(sum(c[0]=='restart_container' for c in self.a.calls), 1)
        self.assertNotIn('DO_NOT_LEAK', (self.paths.state/'status.json').read_text())
    def test_downloaded_catalog_not_loaded_and_healthy_never_restarts(self):
        self.c.loaded=None
        self.a.observation.update(port=12345, qbit_synced=True)
        for t in [0, 120, 1000]: self.tick(t)
        self.assertFalse(self.a.calls)
        self.a.observation.update(port=0)
        self.tick(1010); self.tick(1100); self.finish()
        self.assertEqual(self.a.calls[-1][0], 'restart_container')
        self.assertIsNone(self.c.loaded)
    def test_failed_catalog_refresh_is_throttled(self):
        self.c.next_refresh=0
        self.a.observation.update(port=12345,qbit_synced=True)
        for t in [0,10,20,30,100,599]: self.tick(t)
        self.assertEqual(sum(c[0]=='refresh' for c in self.a.calls),1)
    def test_paused_dependents_and_ssd_guard_are_not_torrent_owners(self):
        self.a.observation.update(port=1234,qbit_synced=False,playback_paused=True,ssd_missing=True)
        self.tick(0);self.finish()
        self.assertEqual([c[0] for c in self.a.calls], ['sync'])
    def test_all_excluded_waits_backoff_and_distinct_preferred_hosts(self):
        self.tick(0)
        for t in [120,240,360]:
            self.tick(t);self.finish()
        puts=[x[1]['provider']['server_selection'] for x in self.a.calls if x[0]=='put']
        self.assertEqual([p['countries'] for p in puts], [['Canada'],['United States'],['United States']])
        # Current Canadian endpoint counted as the first failed trial.
        self.assertEqual(len({p['hostnames'][0] for p in puts}),3)
    def test_restores_watch_restart_durations_and_clock_jump(self):
        self.tick(0);self.tick(90)
        durable=json.loads((self.paths.state/'controller.json').read_text())
        restored=restore_policy(durable, 10, self.a.wall()+10)
        self.assertEqual(restored['started'], -90)
        self.assertLessEqual(restore_policy(durable,10,self.a.wall()-1000).get('candidate_until',10),130)
    def test_one_candidate_is_not_recycled_without_backoff(self):
        only=copy.deepcopy(self.a.catalog.provider)
        only['servers']=only['servers'][:1]
        self.c.loaded=validate_catalog(only,self.a.wall())
        self.a.catalog=self.c.loaded
        self.tick(0);self.tick(120);self.finish()
        self.assertFalse(any(c[0]=='put' for c in self.a.calls))
        self.assertEqual(self.c.state.get('backoff_until'),240)
        self.tick(239);self.tick(240);self.finish()
        self.assertEqual(sum(c[0]=='put' for c in self.a.calls),1)
    def test_unconfirmed_selection_is_never_ready(self):
        self.tick(0);self.tick(120);self.finish()
        self.a.observation.update(port=5555,qbit_synced=True)
        status=self.tick(130)
        self.assertFalse(status['ready'])
    def test_stale_completion_cannot_replace_newer_stop(self):
        self.tick(0)
        def stop_after_send():
            set_intent(self.paths,False,'explicit stop during HTTP')
        original=self.a.put
        def put(*args,**kwargs):
            answer=original(*args,**kwargs);stop_after_send();return answer
        self.a.put=put
        self.tick(120);self.finish();self.tick(130)
        self.assertIsNone(self.c.attempted)
        self.assertEqual(len(self.a.calls),1)
    def test_maintenance_owned_stop_is_not_direct_operator_override(self):
        import os
        from vpn_recovery.intent import may_recover
        inhibit(self.paths,'maintenance',os.getpid(),'country-change',generation=GEN)
        self.a.observation.update(explicit_stop=True,stop_container=GEN['container_id'],stop_at=time.time())
        self.tick(0)
        release_inhibit(self.paths,'maintenance')
        self.assertTrue(may_recover(self.paths))

    def test_same_recycled_settings_force_bounded_tunnel_restart(self):
        from vpn_recovery.intent import read_intent
        candidate=eligible_candidates(self.a.catalog,[])[0]
        def put(route,payload,cid,**kwargs):
            with bounded_lock(self.paths.compose_lock,.01):
                self.assertTrue(kwargs['precondition']())
                self.a.calls.append((route,payload))
            return 'settings left unchanged' if route.endswith('settings') else {'outcome':'running'}
        self.a.put=put
        ctx=dict(action='rotate_server',candidate=candidate,generation=GEN,observation=self.a.observation,
                 revision=settings_revision(self.paths),intent_revision=read_intent(self.paths)['revision'])
        self.c._perform(ctx)
        self.assertEqual([c[1].get('status') for c in self.a.calls],[None,'stopped','running'])
    def test_stop_between_soft_restart_phases_prevents_start(self):
        from vpn_recovery.intent import read_intent
        candidate=eligible_candidates(self.a.catalog,[])[0]
        def put(route,payload,cid,**kwargs):
            if not kwargs['precondition']():raise RuntimeError('deferred')
            self.a.calls.append((route,payload))
            if payload.get('status')=='stopped':set_intent(self.paths,False,'operator stop')
            return 'settings left unchanged' if route.endswith('settings') else {}
        self.a.put=put
        ctx=dict(action='rotate_server',candidate=candidate,generation=GEN,observation=self.a.observation,
                 revision=settings_revision(self.paths),intent_revision=read_intent(self.paths)['revision'])
        self.c._perform(ctx)
        self.assertEqual([c[1].get('status') for c in self.a.calls],[None,'stopped'])

    def test_malformed_attempt_history_does_not_strand_recovery(self):
        atomic_json(self.paths.state/'controller.json',{'attempted':['corrupt'],'round_attempts':17,'policy':{},'last_load_wall':'bad'})
        c=Controller(self.paths,self.a);self.addCleanup(c.close)
        c.loaded=self.a.catalog;c.loaded_container=c.container_key(GEN);c.next_refresh=10000
        c.tick();self.a.now=120;c.tick()
        self.assertTrue(any(call[0]=='put' for call in self.a.calls))
    def test_slow_action_does_not_block_observations_and_new_stop(self):
        import threading
        from concurrent.futures import ThreadPoolExecutor
        gate=threading.Event();entered=threading.Event()
        self.c.executor=ThreadPoolExecutor(max_workers=1)
        self.addCleanup(gate.set)
        old=self.a.put
        def slow(*args,**kwargs):
            entered.set();gate.wait(timeout=2);return old(*args,**kwargs)
        self.a.put=slow
        self.tick(0);self.tick(120);self.assertTrue(entered.wait(timeout=1))
        started=time.monotonic();self.tick(130)
        self.assertLess(time.monotonic()-started,.2)
        set_intent(self.paths,False,'operator stop')
        gate.set();self.finish();self.tick(140)
        self.assertFalse(self.a.calls)

    def test_real_adapter_probe_uses_container_dns_bounded_child_and_fallback(self):
        from vpn_recovery.runtime import SystemAdapters
        import subprocess
        calls=[]
        def runner(args,**kwargs):
            calls.append((args,kwargs))
            code=1 if 'https://one.one.one.one/cdn-cgi/trace' in args else 0
            return subprocess.CompletedProcess(args,code,'','')
        a=SystemAdapters(self.paths,runner)
        self.assertTrue(a._traffic(GEN,time.monotonic()+3))
        self.assertEqual(len(calls),2)
        for args,kwargs in calls:
            self.assertEqual(args[:5],['docker','exec',GEN['container_id'],'timeout','3'])
            self.assertIn('--timeout=1',args)
            self.assertLessEqual(kwargs['timeout'],1.3)
    def test_load_attestation_requires_new_generation_and_positive_evidence(self):
        self.c.loaded=None
        self.tick(0);self.tick(120);self.finish();self.tick(130)
        self.assertIsNone(self.c.loaded)
        self.a.attested=False
        self.a.observation['generation']={**GEN,'started_at':'new-start','pid':43}
        self.tick(140)
        self.assertIsNone(self.c.loaded)
        self.a.attested=True;self.tick(150)
        self.assertEqual(self.c.loaded.identity,self.a.catalog.identity)
        self.assertEqual(self.c.loaded_container['started_at'],'new-start')

    def test_observation_completion_time_is_not_treated_as_future(self):
        original=self.a.observe
        self.a.observation.update(port=1234,qbit_synced=True)
        def observe(**kwargs):
            self.a.now+=.25
            return original(**kwargs)
        self.a.observe=observe
        self.assertTrue(self.tick(0)['ready'])

    def test_recycled_same_endpoint_needs_new_tunnel_generation(self):
        self.c.attempted={**self.a.catalog.provider['servers'][0],
                          'hostname':'node-ca-59.protonvpn.net','endpoint_ip':'84.20.16.29','country':'Canada',
                          'prior_generation':GEN}
        self.a.observation.update(port=1234,qbit_synced=True)
        self.assertFalse(self.tick(0)['ready'])
        self.a.observation['generation']={**GEN,'tunnel_ifindex':3}
        self.assertTrue(self.tick(10)['ready'])

    def test_current_ipv4_container_never_selects_ipv6_endpoint(self):
        provider=copy.deepcopy(self.a.catalog.provider)
        provider['servers']=[provider['servers'][2]]
        provider['servers'][0]['ips']=['2a02:6ea0:e20d:2101::10']
        self.a.catalog=validate_catalog(provider,self.a.wall());self.c.loaded=self.a.catalog
        self.a.observation['endpoint']=None
        self.tick(0);self.tick(120);self.finish()
        self.assertFalse(any(c[0]=='put' for c in self.a.calls))

    def test_successful_selection_is_persisted_without_erasing_penalty(self):
        self.c.failures=[{'hostname':'node-ca-59.protonvpn.net','endpoint_ip':'84.20.16.29','failed_at':self.a.wall()-60}]
        self.a.observation.update(port=1234,qbit_synced=True)
        self.tick(0)
        saved=json.loads((self.paths.state/'controller.json').read_text())
        self.assertEqual(saved.get('last_selection',{}).get('hostname'),'node-ca-59.protonvpn.net')
        self.assertEqual(len(saved['failures']),1)

    def test_unique_public_endpoint_only(self):
        rule='-A OUTPUT -d 84.20.16.29/32 -o eth0 -p udp -m udp --dport 51820 -j ACCEPT\n'
        self.assertEqual(parse_endpoint(rule),'84.20.16.29')
        self.assertIsNone(parse_endpoint(rule+rule.replace('84.20.16.29','84.20.16.1')))
        self.assertIsNone(parse_endpoint(rule.replace('84.20.16.29','10.2.0.1')))

if __name__=='__main__': unittest.main()
