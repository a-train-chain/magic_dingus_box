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
    def container_action(self, action, observation, deadline, *, on_submit=None):
        if on_submit is not None: on_submit()
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
    def test_noop_managed_start_keeps_new_start_recovery_live(self):
        self._new_unattested_start_recovers(managed_noop=True)

    def test_autonomous_start_keeps_new_start_recovery_live(self):
        self._new_unattested_start_recovers(managed_noop=False)

    def _new_unattested_start_recovers(self, managed_noop):
        from vpn_recovery.lifecycle import record_catalog_submission
        self.c.last_load_identity=self.a.catalog.identity
        self.c.last_load_wall=self.a.wall()
        self.a.observation.update(port=1234,qbit_synced=True)
        self.tick(0)
        self.c.state['last_restart']=0
        newer={**GEN,'pid':43,'started_at':'autonomous-start'}
        self.a.observation['generation']=newer
        if managed_noop:
            with bounded_lock(self.paths.compose_lock,0):
                record_catalog_submission(self.paths,newer,snapshot=self.a.catalog,
                                          now=self.a.wall()+1,operation='start')
        self.tick(10)
        self.assertIsNone(self.c.loaded)  # never borrow the previous proof
        self.tick(200)
        self.assertFalse(any(c[0]=='restart_container' for c in self.a.calls))
        self.a.observation.update(port=0,qbit_synced=False)
        for t in [210,300,400,599]: self.tick(t)
        self.assertFalse(any(c[0]=='restart_container' for c in self.a.calls))
        self.tick(600);self.finish();self.tick(610)
        self.assertEqual(sum(c[0]=='restart_container' for c in self.a.calls),1)
        # A failed log check on the resulting start must consume its allowance,
        # even across controller restart, so this is not a log-seeking loop.
        self.a.attested=False
        resulting={**newer,'pid':44,'started_at':'recovery-start'}
        self.a.observation['generation']=resulting
        self.tick(620)
        self.c=Controller(self.paths,self.a);self.addCleanup(self.c.close)
        self.c.next_refresh=100000
        for t in [790,1200,1800,3600]: self.tick(t)
        self.assertIsNone(self.c.loaded)
        self.assertEqual(sum(c[0]=='restart_container' for c in self.a.calls),1)

    def test_second_autonomous_start_does_not_inherit_previous_load_charge(self):
        self.c.loaded=None;self.a.attested=False
        self.tick(0);self.tick(120);self.finish()
        first={**GEN,'pid':43,'started_at':'first-load-result'}
        self.a.observation['generation']=first
        self.tick(130)
        # Reopen the owner to prove the consumed result is durable, then
        # replace that result before its failed log-attestation window expires.
        self.c=Controller(self.paths,self.a);self.addCleanup(self.c.close)
        self.c.next_refresh=100000
        self.a.observation['generation']={**GEN,'pid':44,'started_at':'autonomous-after-result'}
        for t in [140,150,280,719]: self.tick(t)
        self.assertEqual(sum(c[0]=='restart_container' for c in self.a.calls),1)
        self.tick(720);self.finish()
        self.assertEqual(sum(c[0]=='restart_container' for c in self.a.calls),2)

    def test_managed_recreation_after_recent_load_can_attest_then_recover(self):
        import vpn_recovery.lifecycle as lifecycle
        self.assertTrue(hasattr(lifecycle,'record_catalog_submission'),'managed catalog submission is absent')
        self.c.last_load_identity=self.a.catalog.identity
        self.c.last_load_wall=self.a.wall()
        self.tick(0)
        # Real lifecycle publication of the immutable file immediately before
        # Docker submission. No old loaded attestation is copied.
        with bounded_lock(self.paths.compose_lock,0):
            lifecycle.record_catalog_submission(self.paths,GEN,snapshot=self.a.catalog,now=self.a.wall())
        replacement={**GEN,'pid':43,'started_at':'2026-11-14T01:00:00Z'}
        self.a.observation.update(generation=replacement,port=1234,qbit_synced=True)
        self.tick(10)
        self.assertIsNotNone(self.c.loaded)
        self.assertEqual(self.c.loaded_container,self.c.container_key(replacement))
        self.a.observation.update(port=0,qbit_synced=False)
        self.tick(20);self.tick(110);self.finish()
        self.assertTrue(any(call[0]=='put' for call in self.a.calls),self.a.calls)
        self.assertFalse(any(call[0]=='restart_container' for call in self.a.calls))

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
    def test_future_catalog_retry_after_clock_correction_preserves_healthy_tunnel(self):
        from vpn_recovery.catalog import CatalogStore, FetchResponse
        file=self.paths.services/'config/gluetun/servers.json'
        file.parent.mkdir(parents=True)
        file.write_text(json.dumps({'version':4,'protonvpn':self.a.catalog.provider}))
        before=file.read_bytes()
        future=copy.deepcopy(self.a.catalog.provider);future['timestamp']=int(self.a.wall())+600
        fetches=[]
        def fetch(*args):
            fetches.append(self.a.now)
            return FetchResponse(200,json.dumps(future).encode(),{})
        self.a.store=CatalogStore(self.paths,fetch,clock=self.a.wall,jitter=lambda:0)
        self.c.next_refresh=0
        self.a.observation.update(port=1234,qbit_synced=True)
        for t in [0,10,599,609]:self.tick(t)
        self.assertEqual(fetches,[0])
        self.assertEqual(file.read_bytes(),before)
        self.tick(610);self.finish();self.tick(620)
        self.assertEqual(fetches,[0,610])
        self.assertEqual(json.loads(file.read_text())['protonvpn']['timestamp'],1800000600)
        self.assertEqual(self.c.loaded.identity,self.a.catalog.identity)
        self.assertFalse(any(call[0]=='restart_container' for call in self.a.calls))

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
    def test_host_probe_uses_bounded_python_child_with_verified_tls_and_fallback(self):
        from vpn_recovery.runtime import SystemAdapters
        import subprocess
        calls=[]
        def runner(args,**kwargs):
            calls.append((args,kwargs))
            # Reject an undeclared host wget/curl dependency. Only the existing
            # Python interpreter is available on this clean target fixture.
            if args[0] != sys.executable:
                raise FileNotFoundError('host tool absent')
            code=1 if 'https://one.one.one.one/cdn-cgi/trace' in args else 0
            return subprocess.CompletedProcess(args,code,'','')
        a=SystemAdapters(self.paths,runner)
        self.assertTrue(a._traffic(GEN,time.monotonic()+3,host=True))
        self.assertEqual(len(calls),2)
        for args,kwargs in calls:
            self.assertEqual(args[0],sys.executable)
            self.assertLessEqual(kwargs['timeout'],1.3)
            self.assertNotIn('CERT_NONE',args[2])
            self.assertNotIn('unverified',args[2])

    def test_blocked_host_dns_child_is_killed_and_reaped(self):
        from vpn_recovery.runtime import SystemAdapters
        import subprocess
        from unittest.mock import patch
        original=subprocess.Popen;children=[]
        def blocked(*args,**kwargs):
            child=original([sys.executable,'-c','import time;time.sleep(60)'],**kwargs)
            children.append(child);return child
        a=SystemAdapters(self.paths)
        started=time.monotonic()
        with patch('subprocess.Popen',blocked):
            self.assertFalse(a._traffic(GEN,started+.08,host=True))
        self.assertLess(time.monotonic()-started,.5)
        self.assertTrue(children)
        self.assertTrue(all(child.poll() is not None for child in children))

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

    def test_traffic_cadence_survives_nonzero_observation_duration(self):
        requested=[]
        cached={}
        original=self.a.observe
        self.a.observation.update(port=1234,qbit_synced=True)
        def observe(*,deadline,traffic_due):
            started=self.a.now
            if traffic_due:
                requested.append(started)
                cached.update(traffic=True,traffic_at=started,traffic_generation=GEN)
            self.a.now+=.25
            return {**original(deadline=deadline,traffic_due=traffic_due),**cached}
        self.a.observe=observe
        for t in range(0,121,10):
            self.assertTrue(self.tick(t)['ready'])
        self.assertEqual(requested,[0,30,60,90,120])

    def test_final_generation_fence_invalidates_cached_and_fresh_readiness(self):
        from types import SimpleNamespace
        from vpn_recovery.runtime import SystemAdapters
        from vpn_recovery.policy import decide
        newer={**GEN,'tunnel_ifindex':3}
        for fresh in [False,True]:
            with self.subTest(fresh=fresh):
                a=SystemAdapters(self.paths)
                a.mono=lambda:100
                a.current=lambda *_:dict(running=True,configured=True,unit_active=True)
                a._events=lambda *_:None
                a._dependents=lambda *_:{'dependents_ok':True}
                a.ports=SimpleNamespace(observe=lambda **_:dict(generation=GEN,forwarded_port=1234,status='synced',qbit_port=1234))
                a.traffic=dict(traffic=True,traffic_at=90,traffic_generation=GEN)
                a._traffic=lambda *_:True
                sequence=iter([GEN,newer] if fresh else [newer])
                a.control=SimpleNamespace(observe_generation=lambda **_:next(sequence))
                a.run=lambda *_:'-A OUTPUT -d 84.20.16.29/32 -o eth0 -p udp --dport 51820 -j ACCEPT'
                obs=a.observe(deadline=109,traffic_due=fresh)
                obs['allowed']=True
                decision=decide(obs,{},self.a.config(),100,self.a.wall())
                self.assertNotEqual(decision['reason'],'Ready')
                self.assertIsNone(obs['port'])
                self.assertFalse(obs['qbit_synced'])
                self.assertIsNone(obs['traffic'])
                self.assertEqual(a.traffic,{})

    def test_deferred_catalog_load_keeps_allowance_and_retries_after_lock_release(self):
        self.c.loaded=None
        self.tick(0)
        with bounded_lock(self.paths.compose_lock,0):
            self.tick(120);self.finish()
        self.assertIsNone(self.c.last_load_identity)
        self.assertFalse((self.paths.state/'load-pending.json').exists())
        self.tick(130);self.finish()
        self.tick(720);self.finish()
        self.assertEqual(sum(c[0]=='restart_container' for c in self.a.calls),1)
        self.assertGreater(self.c.last_load_wall,1800000120)

    def test_settings_changed_during_observation_cannot_authorize_old_candidate(self):
        from vpn_recovery.common import load_config
        env=self.paths.services/'.env'
        env.write_text('VPN_COUNTRIES=Canada\nVPN_PORT_FORWARDING=on\n')
        self.a.config=lambda:load_config(self.paths)
        self.tick(0)
        original=self.a.observe
        def observe(**kwargs):
            env.write_text('VPN_COUNTRIES=Netherlands\nVPN_PORT_FORWARDING=on\n')
            return original(**kwargs)
        self.a.observe=observe
        self.tick(120);self.finish()
        self.assertFalse(self.a.calls)
        self.a.observe=original
        self.tick(130);self.finish()
        puts=[c[1]['provider']['server_selection'] for c in self.a.calls if c[0]=='put']
        self.assertTrue(puts)
        self.assertEqual(puts[0]['countries'],['Netherlands'])

    def test_nanosecond_docker_event_attributes_same_second_own_stop(self):
        from vpn_recovery.runtime import SystemAdapters
        from vpn_recovery.intent import may_recover
        self.a.now=.75
        a=SystemAdapters(self.paths)
        a.wall=lambda:1800000001.25
        calls=[]
        def events(args,*_):
            calls.append(args)
            return str(1800000000800000000)+' '+GEN['container_id']
        a.run=events
        obs={}
        a._events(obs,10)
        self.assertIn('{{.TimeNano}} {{.Actor.ID}}',calls[0])
        self.c.operation={'generation':GEN,'started_wall':1800000000.75,'until_wall':1800000060.75}
        self.a.observation.update(obs)
        self.tick(1.25)
        self.assertTrue(may_recover(self.paths))
        self.assertAlmostEqual(obs['stop_at'],1800000000.8)

    def test_completed_maintenance_attribution_survives_next_poll_without_resurrection(self):
        import os
        from unittest.mock import patch
        from vpn_recovery.intent import read_intent, boot_id
        current_boot = boot_id()
        with patch('vpn_recovery.intent.time.time',return_value=1800000000), patch('vpn_recovery.intent.boot_id',return_value=current_boot):
            inhibit(self.paths,'completed',os.getpid(),'recreate',generation=GEN)
        with patch('vpn_recovery.intent.time.time',return_value=1800000002):
            release_inhibit(self.paths,'completed')
        self.a.observation.update(explicit_stop=True,stop_container=GEN['container_id'],stop_at=1800000001)
        self.tick(10)
        self.assertTrue(read_intent(self.paths)['enabled'])
        self.assertFalse(read_intent(self.paths)['inhibits'])
        self.a.observation['running']=False
        self.tick(20)
        self.assertFalse(self.a.calls)
        # A later stop cannot borrow a completed operation's attribution.
        self.a.observation['stop_at']=1800000003
        self.tick(30)
        self.assertFalse(read_intent(self.paths)['enabled'])

    def test_submitted_uncertain_catalog_load_keeps_allowance_across_watcher_restart(self):
        self.c.loaded=None
        original=self.a.container_action
        def uncertain(action,observation,deadline,*,on_submit=None):
            original(action,observation,deadline,on_submit=on_submit)
            raise TimeoutError('Docker reply lost after submission')
        self.a.container_action=uncertain
        self.tick(0);self.tick(120);self.finish()
        # The worker's submission journal survives before the next poll saves
        # controller.json, including a lost/failed client reply.
        restored=Controller(self.paths,self.a)
        self.addCleanup(restored.close)
        self.assertEqual(restored.last_load_identity,self.a.catalog.identity)
        self.assertEqual(restored.last_load_wall,1800000120)
        self.tick(130)
        self.assertEqual(self.c.last_load_identity,self.a.catalog.identity)
        self.assertEqual(self.c.last_load_wall,1800000120)
        self.tick(720)
        self.assertEqual(sum(c[0]=='restart_container' for c in self.a.calls),1)

    def test_exhausted_command_deadline_never_marks_catalog_submission(self):
        from vpn_recovery.runtime import SystemAdapters
        calls=[]
        def runner(*args,**kwargs):
            calls.append('runner')
            raise AssertionError('deadline must prevent submission')
        a=SystemAdapters(self.paths,runner)
        a.mono=lambda:30
        with self.assertRaises(TimeoutError):
            a.container_action('restart_container',{},30,on_submit=lambda:calls.append('submitted'))
        self.assertEqual(calls,[])

    def test_config_snapshot_changing_during_read_defers(self):
        original=self.a.config
        def config():
            old=original()
            (self.paths.services/'.env').write_text('VPN_COUNTRIES=Netherlands\n')
            return old
        self.tick(0)
        self.a.config=config
        self.tick(120);self.finish()
        self.assertFalse(self.a.calls)

    def test_nanosecond_stop_before_operation_is_not_attributed(self):
        from vpn_recovery.intent import read_intent
        self.c.operation={'generation':GEN,'started_wall':1800000000.75,'until_wall':1800000060.75}
        self.a.observation.update(explicit_stop=True,stop_at=1800000000.70,stop_container=GEN['container_id'])
        self.tick(1)
        self.assertFalse(read_intent(self.paths)['enabled'])

    def test_unknown_final_fence_preserves_catalog_for_later_lease_recovery(self):
        from types import SimpleNamespace
        from vpn_recovery.runtime import SystemAdapters
        self.c.last_load_identity=self.a.catalog.identity
        self.c.last_load_wall=self.a.wall()
        self.a.observation.update(port=1234,qbit_synced=True)
        self.assertTrue(self.tick(0)['ready'])

        adapter=SystemAdapters(self.paths)
        adapter.mono=lambda:10
        adapter.current=lambda *_:dict(running=True,configured=True,unit_active=True,
                                      **self.c.container_key(GEN))
        adapter._events=lambda *_:None
        adapter._dependents=lambda *_:{'dependents_ok':True}
        adapter.ports=SimpleNamespace(observe=lambda **_:dict(generation=GEN,forwarded_port=1234,
                                                            status='synced',qbit_port=1234))
        adapter.traffic=dict(traffic=True,traffic_at=0,traffic_generation=GEN)
        def unavailable(**_):
            raise TimeoutError('final generation fence temporarily unavailable')
        adapter.control=SimpleNamespace(observe_generation=unavailable)
        adapter.run=lambda *_:'-A OUTPUT -d 84.20.16.29/32 -o eth0 -p udp --dport 51820 -j ACCEPT'
        invalidated=adapter.observe(deadline=19,traffic_due=False)
        self.assertIsNone(invalidated['generation'])
        original_observe=self.a.observe
        self.a.observe=lambda **_:copy.deepcopy(invalidated)
        status=self.tick(10)
        self.assertFalse(status['ready'])
        self.assertFalse(status['traffic_verified'])
        self.assertFalse(status['qbit_synced'])
        self.assertIsNone(status['forwarded_port'])
        self.assertIsNotNone(self.c.loaded)
        self.assertEqual(self.c.loaded.identity,self.a.catalog.identity)

        self.a.observe=original_observe
        self.assertTrue(self.tick(20)['ready'])
        self.assertEqual(self.c.loaded.identity,self.a.catalog.identity)
        self.a.observation['port']=0
        self.tick(30)
        self.assertFalse(self.a.calls)
        self.tick(119)
        self.assertFalse(self.a.calls)
        self.tick(120);self.finish()
        puts=[c[1]['provider']['server_selection'] for c in self.a.calls if c[0]=='put']
        self.assertEqual(len(puts),1)
        self.assertEqual(puts[0]['hostnames'],['node-ca-60.protonvpn.net'])
        self.assertFalse(any(c[0]=='restart_container' for c in self.a.calls))

    def test_catalog_attestation_cannot_cross_confirmed_container_start(self):
        newer={**GEN,'started_at':'2026-11-14T00:01:00Z','pid':43}
        for tunnel_available in [True,False]:
            with self.subTest(tunnel_available=tunnel_available):
                self.c.loaded=self.a.catalog
                self.c.loaded_container=self.c.container_key(GEN)
                self.c.last_load_identity=self.a.catalog.identity
                self.c.last_load_wall=self.a.wall()
                self.a.observation.update(generation=newer if tunnel_available else None,
                    **self.c.container_key(newer),traffic=None,port=None,qbit_synced=False)
                status=self.tick(10)
                self.assertIsNone(self.c.loaded)
                self.assertIsNone(status['catalog']['loaded'])
                self.assertFalse(status['ready'])
                self.assertFalse(any(c[0]=='put' for c in self.a.calls))

    def test_real_catalog_attestation_accepts_sorted_file_but_requires_content_and_info(self):
        import subprocess
        from vpn_recovery.runtime import SystemAdapters
        snapshot = self.a.catalog
        provider = copy.deepcopy(snapshot.provider)
        provider['servers'] = [provider['servers'][i] for i in (0, 1, 4, 5, 2, 3)]
        path = self.paths.services/'config/gluetun/servers.json'
        atomic_json(path, {'version': 1, 'protonvpn': provider})
        log = 'INFO [storage] Using protonvpn servers from file which are 361 days more recent'
        calls = []
        def runner(args, **kwargs):
            calls.append(args)
            return subprocess.CompletedProcess(args, 0, log, '')
        adapter = SystemAdapters(self.paths, runner)
        self.assertTrue(adapter.attest_catalog(GEN, snapshot, time.monotonic()+3))
        self.assertEqual(calls, [['docker', 'logs', '--since', GEN['started_at'],
                                  '--tail', '1000', GEN['container_id']]])
        log = ''
        self.assertFalse(adapter.attest_catalog(GEN, snapshot, time.monotonic()+3))
        log = 'INFO [storage] Using protonvpn servers from file which are 361 days more recent'
        provider['servers'][0]['city'] = 'Changed after load'
        atomic_json(path, {'version': 1, 'protonvpn': provider})
        self.assertFalse(adapter.attest_catalog(GEN, snapshot, time.monotonic()+3))

    def test_bootstrap_stale_namespace_repairs_before_port_sync_and_is_bounded(self):
        self.a.observation.update(port=33013,qbit_synced=False,namespaces_match=False,
                                  dependents_ok=False,host_reachable=True)
        self.assertIsNone(self.c.relinked)
        self.tick(0);self.finish()
        self.assertEqual([c[0] for c in self.a.calls],['relink_dependents'])
        self.tick(10)
        # The repair's command success is not namespace proof. Do not loop or
        # borrow success when Docker leaves the dependent on the old netns.
        for t in [20,100,299]:self.assertFalse(self.tick(t)['ready'])
        self.assertEqual([c[0] for c in self.a.calls],['relink_dependents'])
        restored=Controller(self.paths,self.a);self.addCleanup(restored.close)
        restored.next_refresh=100000
        self.a.now=300;restored.tick()
        self.assertEqual([c[0] for c in self.a.calls],['relink_dependents'])
        self.a.now=310;restored.tick()
        self.assertEqual([c[0] for c in self.a.calls],['relink_dependents','relink_dependents'])
        self.a.observation.update(namespaces_match=True,dependents_ok=True)
        self.a.now=320;restored.tick()
        self.assertEqual(self.a.calls[-1][0],'sync')
        self.a.observation['qbit_synced']=True
        self.a.now=330;self.assertTrue(restored.tick()['ready'])

    def test_bootstrap_unknown_namespace_never_stamps_relink_proof(self):
        self.a.observation.update(port=33013,qbit_synced=False,namespaces_match=None)
        self.tick(0)
        self.assertIsNone(self.c.relinked)
        self.assertIsNone(json.loads((self.paths.state/'controller.json').read_text())['relinked'])

    def test_qbit_fault_with_matching_namespace_never_relinks(self):
        self.a.observation.update(port=33013,qbit_synced=False,namespaces_match=True,
                                  host_reachable=False,dependents_ok=False)
        for t in [0,10,300,600,1200]:self.tick(t)
        self.assertEqual({c[0] for c in self.a.calls},{'sync'})

    def test_unique_public_endpoint_only(self):
        rule='-A OUTPUT -d 84.20.16.29/32 -o eth0 -p udp -m udp --dport 51820 -j ACCEPT\n'
        self.assertEqual(parse_endpoint(rule),'84.20.16.29')
        self.assertIsNone(parse_endpoint(rule+rule.replace('84.20.16.29','84.20.16.1')))
        self.assertIsNone(parse_endpoint(rule.replace('84.20.16.29','10.2.0.1')))

class NamespaceObservationTests(unittest.TestCase):
    def setUp(self):
        from vpn_recovery.runtime import SystemAdapters
        from unittest.mock import patch, MagicMock
        import subprocess
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.a=SystemAdapters(Paths(base=Path(self.tmp.name)))
        self.a.mono=lambda:0
        self.names=['radarr','sonarr','prowlarr','qbittorrent','byparr']
        self.pids={name:100+i for i,name in enumerate(self.names)}
        self.links={42:'net:[4026532677]',**{pid:'net:[4026532908]' for pid in self.pids.values()}}
        self.paused=False;self.calls=[];self.replace_during_read=False
        def runner(args,**kwargs):
            self.calls.append((args,kwargs['timeout']))
            if args[:2]==['docker','ps']:
                out=''.join('mdb_'+name+' running Up 2 days (healthy)\n' for name in self.names)
            elif args[:2]==['docker','inspect']:
                rows=[]
                for name in args[4:]:
                    if name=='mdb_gluetun':
                        rows.append('/mdb_gluetun '+GEN['container_id']+' 42 '+GEN['started_at']+' true')
                    else:
                        short=name.removeprefix('mdb_')
                        rows.append('/'+name+' '+('b'*64)+' '+str(self.pids[short])+' old-start true')
                out='\n'.join(rows)
            else:raise AssertionError(args)
            return subprocess.CompletedProcess(args,0,out,'')
        self.a.runner=runner
        def readlink(path):
            pid=int(str(path).split('/')[2])
            value=self.links[pid]
            if self.replace_during_read and pid!=42:self.links[42]='net:[4026533000]'
            if isinstance(value,Exception):raise value
            return value
        self.addCleanup(patch.stopall)
        patch('vpn_recovery.runtime.os.readlink',side_effect=readlink).start()
        patch('vpn_recovery.runtime.Path.exists',side_effect=lambda:self.paused).start()
        patch('socket.create_connection',return_value=MagicMock()).start()

    def observe(self):
        return self.a._dependents(9,GEN)

    def test_same_container_id_new_namespace_detected_despite_healthy_tcp(self):
        o=self.observe()
        self.assertFalse(o['dependents_ok'])
        self.assertIs(o.get('namespaces_match'),False)
        self.assertTrue(o['host_reachable'])
        self.assertFalse(o['needs_converge'])
        self.assertLessEqual(sum(timeout for _,timeout in self.calls),9)

    def test_paused_heavy_services_do_not_request_relink(self):
        self.paused=True
        self.links[self.pids['qbittorrent']]=self.links[42]
        o=self.observe()
        self.assertIs(o.get('namespaces_match'),True)
        self.assertTrue(o['dependents_ok'])
        self.assertFalse(o['needs_converge'])
        self.links[self.pids['qbittorrent']]='net:[4026532908]'
        self.assertIs(self.observe().get('namespaces_match'),False)
        self.links[self.pids['qbittorrent']]=self.links[42]
        self.paused=False
        self.assertIs(self.observe().get('namespaces_match'),False)

    def test_unavailable_or_replaced_namespace_is_unknown(self):
        self.links[42]=FileNotFoundError()
        o=self.observe()
        self.assertIsNone(o.get('namespaces_match'))
        self.assertFalse(o['dependents_ok'])
        self.links[42]='net:[4026532677]';self.replace_during_read=True
        o=self.observe()
        self.assertIsNone(o.get('namespaces_match'))
        self.assertFalse(o['dependents_ok'])

if __name__=='__main__': unittest.main()
