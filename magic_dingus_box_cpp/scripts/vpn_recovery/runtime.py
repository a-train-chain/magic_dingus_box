"""One polling owner, with bounded I/O and one independent action worker.

All mutations own the read-open /run/lock/mdb-compose.lock.
A tick's shared observation deadline is 9s. Commands/actions/catalog work have
separate aggregate deadlines and run on ONE worker: slow compose never stalls
10s observation cadence. Results are fenced before use, not trusted as health.
"""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
from dataclasses import asdict
import copy
import ipaddress
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time

from .common import atomic_json, bounded_lock, load_config
from .catalog import CatalogStore, bounded_history, eligible_candidates, record_failure, select_candidate, validate_catalog
from .control import ControlClient, ControlError
from .docker_api import DockerRunner
from .port_sync import PortSynchronizer, QbitClient
from .intent import (read_json, read_intent, may_recover, adopt_running, set_intent,
                     settings_revision, boot_id, process_start, owns_stop)
from .policy import CANDIDATE_GRACE_S, decide, finite


# Host WAN classification uses the already-required interpreter, not an
# undeclared wget/curl package. Parent subprocess timeout also bounds libc DNS;
# HTTPSConnection retains certificate/hostname verification and no redirects.
_HOST_TRAFFIC_SCRIPT = r'''
import http.client, sys
from urllib.parse import urlsplit
try:
    url = urlsplit(sys.argv[1])
    connection = http.client.HTTPSConnection(url.hostname, timeout=1)
    connection.request('GET', url.path)
    response = connection.getresponse()
    if response.status != (200 if url.path == '/cdn-cgi/trace' else 204):
        raise ValueError()
    body = response.read(16385)
    if len(body) > 16384:
        raise ValueError()
    connection.close()
    sys.stdout.write(body.decode('utf-8'))
except Exception:
    sys.exit(1)
'''


def utc(wall):
    return datetime.fromtimestamp(wall, timezone.utc).isoformat()


def parse_endpoint(output):
    """Firewall evidence is selection only, never traffic proof.

    Pinned firewall/vpn.go removes the old UDP endpoint then installs the new.
    Removal errors are tolerated upstream, so multiple rules are ambiguous.
    """
    endpoints = []
    for line in output.splitlines():
        try:
            fields = shlex.split(line)
            if fields[:2] != ['-A', 'OUTPUT']:
                continue
            def value(key): return fields[fields.index(key)+1] if key in fields else None
            if value('-j') != 'ACCEPT' or value('-p') != 'udp' or value('-o') != 'eth0' or not value('--dport'):
                continue
            address = ipaddress.ip_network(value('-d'), strict=True)
            if address.num_addresses == 1 and address.network_address.is_global:
                endpoints.append(str(address.network_address))
        except (ValueError, IndexError, TypeError):
            continue
    return endpoints[0] if len(endpoints) == 1 else None


def restore_policy(durable, now, wall):
    """Translate bounded wall ages across watcher restarts; never trust raw mono.

    Backward/large wall jumps reset failure confirmation but preserve a full
    restart cooldown. Boot/runtime changes cannot yield immediate storm retries.
    """
    s = copy.deepcopy(durable.get('policy', {}))
    oldmono, oldwall = durable.get('saved_mono'), durable.get('saved_wall')
    if not isinstance(s, dict) or not finite(oldmono) or not finite(oldwall):
        return {}
    elapsed = wall-oldwall
    if not 0 <= elapsed <= 86400:
        return {'version': 2, 'started': now, 'last_restart': now} if 'last_restart' in s else {}
    for key in ('started', 'last_tick', 'port_lost', 'traffic_lost', 'control_lost', 'last_restart',
                'traffic_unknown', 'port_unknown', 'tunnel_seen', 'rotated_at'):
        if finite(s.get(key)):
            age = max(0, oldmono-s[key])+elapsed
            s[key] = now-min(86400, age)
        else:
            s.pop(key, None)
    for key in ('candidate_until', 'backoff_until'):
        if finite(s.get(key)):
            s[key] = now+max(0, min(600, s[key]-oldmono-elapsed))
        else:
            s.pop(key, None)
    s['losses'] = [now-(oldmono-t+elapsed) for t in s.get('losses', [])[-256:]
                   if finite(t) and 0 <= oldmono-t+elapsed < 600] if isinstance(s.get('losses', []), list) else []
    return s


class Deferred(RuntimeError):
    pass


ROUND_STABLE_S = 1800


class SystemAdapters:
    def __init__(self, paths, runner=subprocess.run):
        self.paths = paths
        # Production identity/ps queries are served in-process from the
        # Docker socket with the CLI's exact stdout (~20 per tick); every
        # other command still runs as a process. Test runners are unchanged.
        self.production = runner is subprocess.run
        self.runner = DockerRunner(fallback=runner) if self.production else runner
        self.mono, self.wall = time.monotonic, time.time
        self.control = ControlClient(paths, self.runner)
        self.endpoint_cache = None
        # Generation our own in-tunnel probe has verified, and when (wall).
        self.traffic_proven = None
        self.traffic_proven_at = None
        self.ports = PortSynchronizer(paths, self.control, QbitClient(paths.services/'.env', runner=runner))
        self.store = CatalogStore(paths)
        self.traffic = {}
        self.event_cursor = int(time.time())-2
        self.dependent_since = {}

    def executor(self):
        return ThreadPoolExecutor(max_workers=1, thread_name_prefix='vpn-action')

    def config(self): return load_config(self.paths)

    def run(self, args, deadline, cap=3, *, on_submit=None):
        remaining = min(cap, deadline-self.mono())
        if remaining <= 0:
            raise TimeoutError('observation deadline')
        # No response or exception from Docker crosses the sanitized boundary.
        try:
            if on_submit is not None:
                on_submit()
            result = self.runner(args, capture_output=True, text=True, timeout=remaining, check=False)
            if result.returncode or len(result.stdout) > 1048576:
                raise ValueError()
            return result.stdout
        except Exception:
            raise RuntimeError('external operation failed') from None

    def current(self, deadline):
        try:
            raw = self.run(['docker', 'inspect', '--format',
                '{{.Id}} {{.State.Pid}} {{.State.StartedAt}} {{.State.Running}} {{.State.OOMKilled}} {{.State.Status}}',
                'mdb_gluetun'], deadline, 1)
            parts = raw.split()
            if len(parts) != 6 or not re.fullmatch('[a-f0-9]{64}', parts[0]) or not parts[1].isdigit():
                raise ValueError()
            value = {'running':parts[3]=='true', 'affirmative_failure':parts[4]=='true',
                     'container_id':parts[0], 'started_at':parts[2], 'pid':int(parts[1])}
        except Exception:
            value = {'running': None}
        value['configured'] = (self.paths.services/'.env').is_file() and (self.paths.services/'docker-compose.yml').is_file()
        try:
            value['unit_active'] = self.run(['systemctl','is-active','magic-dingus-services.service'],deadline,.3).strip() == 'active'
        except Exception:
            value['unit_active'] = False
        return value

    def _traffic(self, generation, deadline, host=False):
        prefix = [] if host else ['docker','exec',generation['container_id'],'timeout','3']
        # Two independent providers. DNS+TCP+TLS, bounded by the caller deadline;
        # cached publicip/control responses are deliberately never used.
        for url, marker in [('https://one.one.one.one/cdn-cgi/trace','ip='),
                            ('https://www.google.com/generate_204', None)]:
            try:
                command = ([sys.executable,'-c',_HOST_TRAFFIC_SCRIPT,url] if host else
                           prefix+['wget','-qO-','--tries=1','--timeout=1',url])
                out = self.run(command,deadline,1.3)
                if marker is None or any(line.startswith(marker) for line in out.splitlines()):
                    return True
            except Exception:
                pass
        return False

    def _healthcheck_vouches(self, generation, deadline):
        """Docker's own in-tunnel healthcheck (DNS+TCP+TLS, 3 s) passed <=45 s ago.

        Only the boolean leaves: the log Output holds the probe body.
        """
        try:
            log=json.loads(self.run(['docker','inspect','--format','{{json .State.Health.Log}}',
                                     generation['container_id']],deadline,.6))
            passed,failed=[],[]
            for entry in (log[-5:] if isinstance(log,list) else []):
                if isinstance(entry,dict) and isinstance(entry.get('End'),str):
                    text=re.sub(r'(\.\d{6})\d+',r'\1',entry['End']).replace('Z','+00:00')
                    (passed if entry.get('ExitCode')==0 else failed).append(datetime.fromisoformat(text).timestamp())
            if not passed or not finite(self.traffic_proven_at):
                return False
            latest=max(passed)
            # It must have run on THIS tunnel after our own probe proved it,
            # be recent, and not be followed by a failed check.
            return (latest>self.traffic_proven_at and 0<=self.wall()-latest<=45
                    and not any(end>latest for end in failed))
        except Exception:
            return False

    def _events(self, observation, deadline):
        now = int(self.wall())
        try:
            output = self.run(['docker','events','--since',str(self.event_cursor),'--until',str(now),
                              '--filter','container=mdb_gluetun','--filter','event=stop',
                              '--format','{{.TimeNano}} {{.Actor.ID}}'],deadline,.5)
            self.event_cursor = now
            for line in output.splitlines()[-100:]:
                fields = line.split()
                if len(fields)==2 and fields[0].isdigit() and re.fullmatch('[a-f0-9]{64}',fields[1]):
                    observation.update(explicit_stop=True,stop_at=int(fields[0])/1_000_000_000,stop_container=fields[1])
        except Exception:
            pass  # Current stopped state still prevents ambiguous resurrection.

    def _dependent_namespaces(self, generation, targets, deadline):
        """Compare kernel namespaces, not Compose's desired NetworkMode.

        A same-ID Gluetun restart replaces its namespace while running
        dependents can retain the old one. Inspect only identity fields; a
        missing process, changed Gluetun start or exhausted budget is unknown.
        """
        if not generation or not targets:
            return None
        try:
            output = self.run(['docker','inspect','--format',
                '{{.Name}} {{.Id}} {{.State.Pid}} {{.State.StartedAt}} {{.State.Running}}',
                'mdb_gluetun']+['mdb_'+name for name in targets],deadline,.8)
            identities = {}
            for line in output.splitlines():
                name, cid, pid, started, running = line.split()
                if not re.fullmatch('[a-f0-9]{64}',cid) or not pid.isdigit() or int(pid)<=0 or running!='true':
                    return None
                identities[name.lstrip('/')] = dict(container_id=cid,pid=int(pid),started_at=started)
            gluetun = identities['mdb_gluetun']
            if gluetun != Controller.container_key(generation):
                return None
            def namespace(pid):
                if self.mono()>=deadline:
                    raise TimeoutError()
                value=os.readlink('/proc/'+str(pid)+'/ns/net')
                if not re.fullmatch(r'net:\[[1-9][0-9]*\]',value):
                    raise ValueError()
                return value
            expected=namespace(gluetun['pid'])
            matches=[namespace(identities['mdb_'+name]['pid'])==expected for name in targets]
            if namespace(gluetun['pid'])!=expected:
                return None
            return all(matches)
        except (OSError,RuntimeError,TimeoutError,ValueError,KeyError):
            return None

    def _dependents(self, deadline, generation=None):
        names = ['radarr','sonarr','prowlarr','qbittorrent','byparr']
        paused = Path('/tmp/mdb_playback_services_paused').exists()
        targets = ['qbittorrent'] if paused else names
        try:
            output = self.run(['docker','ps','-a','--filter','name=mdb_', '--format',
                               '{{.Names}} {{.State}} {{.Status}}'],deadline,.8)
            states = {}
            for line in output.splitlines():
                fields=line.split()
                if len(fields)>=3: states[fields[0].lstrip('/')] = fields[1:]
        except Exception:
            return {'dependents_ok':False,'needs_converge':False,'playback_paused':paused}
        converge = False
        for name in targets:
            fields = states.get('mdb_'+name, ['absent'])
            if fields[0] in ('absent','created','exited','dead'): converge=True
            if any('unhealthy' in field for field in fields):
                self.dependent_since.setdefault(name,self.mono())
                if self.mono()-self.dependent_since[name]>=300: converge=True
            else: self.dependent_since.pop(name,None)
        running=[name for name in targets if states.get('mdb_'+name,['absent'])[0]=='running']
        namespaces=self._dependent_namespaces(generation,running,deadline)
        if namespaces is True and len(running)!=len(targets):
            namespaces=None
        # TCP is supplementary availability evidence, never namespace proof:
        # Docker's listener can accept and then reset HTTP in an orphaned netns.
        reachable=True
        import socket
        for name,port in [('radarr',7878),('sonarr',8989),('prowlarr',9696),('qbittorrent',8080),('byparr',8191)]:
            if name not in targets: continue
            try:
                remaining=min(.12,deadline-self.mono())
                if remaining<=0: raise TimeoutError()
                with socket.create_connection(('127.0.0.1',port),timeout=remaining): pass
            except OSError:
                reachable=False
        return {'dependents_ok':not converge and reachable and namespaces is True,'needs_converge':converge,
                'namespaces_match':namespaces,
                'host_reachable':reachable,'playback_paused':paused}

    def observe(self, *, deadline, traffic_due):
        now = self.mono()
        o = self.current(min(deadline,now+1.3))
        self._events(o,min(deadline,now+1.8))
        if not o.get('running'):
            return o
        # Reserve time after port evidence for independent tunnel observations.
        result = self.ports.observe(deadline=min(deadline,now+5.5))
        generation = result.get('generation')
        if generation is None:
            try: generation=self.control.observe_generation(deadline=min(deadline,now+6))
            except Exception: pass
        o.update(generation=generation, port=result.get('forwarded_port'), port_at=self.mono(),
                 port_generation=result.get('generation'), qbit_synced=result.get('status')=='synced',
                 qbit_port=result.get('qbit_port'), control_available=type(result.get('forwarded_port')) is int)
        # Sample dependencies before the final generation fence and reserve
        # observation time for them even when qBit's HTTP request is failing.
        o.update(self._dependents(min(deadline,now+6.8),generation))
        if generation is not None:
            if traffic_due or self.traffic.get('traffic_generation') != generation:
                ok=own=self._traffic(generation,min(deadline,now+8.2))
                # Our 1.3 s probe (650-750 ms idle) can starve under heavy
                # load while the tunnel is fine. Only for a tunnel this same
                # generation already proved working, a recent Docker in-tunnel
                # healthcheck success vouches for it; a new or dead candidate
                # never borrows that evidence.
                if not own and self.traffic_proven==generation:
                    ok=self._healthcheck_vouches(generation,min(deadline,now+8.4))
                # A late success from a replaced interface is not readiness.
                try: stable=self.control.observe_generation(deadline=min(deadline,now+8.6))==generation
                except Exception: stable=False
                self.traffic={'traffic':ok if stable else None,'traffic_at':self.mono(),
                              'traffic_generation':generation}
                if own and stable and self.traffic_proven!=generation:
                    self.traffic_proven=copy.deepcopy(generation)
                    self.traffic_proven_at=self.wall()
                if not ok:
                    # Host traffic is public probe metadata only, never indexer traffic.
                    self.traffic['wan']=self._traffic(generation,deadline,host=True)
            o.update(self.traffic)
            # Gluetun rewrites its endpoint rule only when it (re)connects,
            # which always creates a new tun0, so a parse is reused within one
            # full generation once two consecutive reads agree (a rule written
            # just after tun0 appears is never held), refreshed every 60 s.
            endpoint, fresh = None, False
            cached = self.endpoint_cache
            if cached and cached[0] == generation and cached[3] and 0 <= self.mono()-cached[2] < 60:
                endpoint = cached[1]
            else:
                fresh = True
                try:
                    endpoint=parse_endpoint(self.run(['docker','exec',generation['container_id'],'iptables','-S','OUTPUT'],deadline,.4))
                except Exception:
                    pass
            try:
                current_generation = self.control.observe_generation(deadline=deadline)
            except Exception:
                current_generation = None
            if current_generation == generation:
                o['endpoint'] = endpoint
                if fresh:
                    agreed = bool(endpoint and cached and cached[0] == generation and cached[1] == endpoint)
                    self.endpoint_cache = (copy.deepcopy(generation), endpoint, self.mono(), agreed) if endpoint else None
            else:
                self.endpoint_cache = None
                # A known replacement (or failed final fence) invalidates the
                # entire snapshot, including traffic cached before this tick.
                # Force a fresh traffic probe on the next coherent generation.
                self.traffic = {}
                o.update(generation=current_generation, port=None, port_generation=None,
                         qbit_synced=False, qbit_port=None, traffic=None,
                         traffic_generation=None, control_available=None,
                         namespaces_match=None, dependents_ok=False)
        else:
            # Missing tun0 is current affirmative tunnel failure; no cached port.
            o.update(traffic=False,traffic_at=self.mono(),port=None)
        return o

    def attest_catalog(self, generation, snapshot, deadline):
        if self.store.load().identity != snapshot.identity:
            return False
        # INFO log in pinned storage/merge.go, not a debug-only message. Only
        # return a Boolean; container logs/settings never leave this function.
        # It is printed during startup: read that window, not the newest 1000
        # lines (a restart-looping tunnel scrolls past it within minutes;
        # Docker 26 also returns nothing for --tail with --until).
        try:
            start=datetime.strptime(generation['started_at'][:19],'%Y-%m-%dT%H:%M:%S').replace(tzinfo=timezone.utc)
            window=['--until',(start+timedelta(seconds=121)).strftime('%Y-%m-%dT%H:%M:%SZ')]
        except (TypeError,ValueError):
            window=['--tail','1000']
        # Without --tail Docker reads json-file from the start (<=3x10 MB).
        output=self.run(['docker','logs','--since',generation['started_at'],*window,
                         generation['container_id']],deadline,5)
        return bool(re.search(r'Using protonvpn servers from file which are .* more recent',output))

    def container_action(self, action, observation, deadline, *, on_submit=None):
        """Caller owns compose lock; no nested lock or health wait.

        Dependent shell helper is sourceable and has explicit inherited-lock
        mode. Its whole process group is bounded by GNU timeout on the Pi.
        """
        if action=='restart_container':
            self.run(['docker','restart','--time','5','mdb_gluetun'],deadline,20,on_submit=on_submit)
            return True
        script=self.paths.base/'magic_dingus_box_cpp/scripts/gluetun_cascade_restart.sh'
        # bash source with static argv; no interpolated settings/credentials.
        command='source "$1"; COMPOSE_DIR="$2"; MDB_COMPOSE_LOCK_HELD=1; VPN_RECOVERY_CTL="$3"; VPN_RECOVERY_STATE="$4"; VPN_RECOVERY_RUNTIME="$5"; '
        command += 'cascade_dependents' if action=='relink_dependents' else 'converge_dependents'
        args=['bash','-c',command,'vpn-helpers',str(script),str(self.paths.services),str(script.with_name('vpn_recoveryctl.py')),str(self.paths.state),str(self.paths.runtime)]
        if self.production:
            remaining=min(28,deadline-self.mono()-.25)
            if remaining<=0:raise Deferred()
            process=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
            try:
                if process.wait(timeout=remaining):raise RuntimeError('dependent action failed')
            except subprocess.TimeoutExpired:
                # Kill the whole helper group before releasing compose lock;
                # killing only Bash could leave a later compose mutation alive.
                os.killpg(process.pid,signal.SIGKILL)
                process.wait(timeout=.25)
                raise RuntimeError('dependent action timed out') from None
        else:
            self.run(args,deadline,28)
        return True


class Controller:
    def __init__(self, paths, adapters=None):
        self.paths=paths
        self.a=adapters or SystemAdapters(paths)
        self.executor=self.a.executor()
        self.pending=None; self.pending_context=None
        self.loaded=None; self.loaded_container=None; self.load_pending=None
        self.downloaded_cache=None;self.downloaded_signature=None
        durable=read_json(paths.state/'controller.json')
        self.state=restore_policy(durable,self.a.mono(),self.a.wall())
        self.failures=bounded_history(durable.get('failures',[]),self.a.wall())
        self.round_attempts=[s for s in durable.get('round_attempts',[])[:256] if isinstance(s,str)] if isinstance(durable.get('round_attempts',[]),list) else []
        self.effective=None; self.attempted=durable.get('attempted')
        if not isinstance(self.attempted,dict) or not all(isinstance(self.attempted.get(k),str) for k in ('hostname','country','endpoint_ip')):
            self.attempted=None
        self.api_failed=False; self.operation=read_json(paths.state/'operation.json')
        # Continuous-Ready start (monotonic); closes a recovery round once stable.
        self.ready_since=None
        # Every supported maintenance owner/lifecycle change bumps the intent
        # revision (never routine ticks): the selection may have changed.
        self.last_intent_revision=read_intent(paths).get('revision')
        self.relinked=durable.get('relinked'); self.last_converge=-300
        self.last_converge_container=self.container_key(durable.get('last_converge_container'))
        last_converge_wall=durable.get('last_converge_wall')
        if finite(last_converge_wall):
            self.last_converge=self.a.mono()-max(0,min(300,self.a.wall()-last_converge_wall))
        self.next_traffic=0
        self.next_refresh=self.a.mono()+max(0,min(86400,durable.get('refresh_after',0)-self.a.wall())) if finite(durable.get('refresh_after')) else 0
        self.last_load_container=durable.get('last_load_container')
        self.last_load_identity=durable.get('last_load_identity')
        self.last_load_wall=durable.get('last_load_wall',0)
        if not finite(self.last_load_wall):self.last_load_wall=self.a.wall()
        self.last_reason=durable.get('last_recovery_reason')
        self.last_success=durable.get('last_success')
        self.last_selection=durable.get('last_selection') if isinstance(durable.get('last_selection'),dict) else None
        pending_load=read_json(paths.state/'load-pending.json',9*1024*1024)
        try:
            submitted_wall = pending_load['submitted_wall']
            if not finite(submitted_wall) or not 0 <= self.a.wall()-submitted_wall <= 86400:
                raise ValueError('invalid load submission time')
            snapshot = validate_catalog(pending_load['provider'],self.a.wall())
            self.load_pending={'snapshot':snapshot,'old_container':pending_load['old_container'],
                               'submitted_wall':submitted_wall,'operation':pending_load.get('operation','restart')}
            if (submitted_wall==self.last_load_wall and self.last_load_container
                    and self.last_load_container!=pending_load['old_container']):
                self.load_pending['result_container']=self.last_load_container
            self.external_load_wall=submitted_wall
            if self.load_pending['operation']!='start':
                self._charge_load(self.load_pending, self.a.mono(), self.a.wall())
        except (KeyError,ValueError,TypeError):pass
        loaded=read_json(paths.state/'loaded.json',9*1024*1024)
        try:
            self.loaded=validate_catalog(loaded['provider'],self.a.wall())
            self.loaded_container=loaded['container']
        except (KeyError,ValueError,TypeError): pass

    @staticmethod
    def container_key(generation):
        return {k:generation.get(k) for k in ('container_id','started_at','pid')} if isinstance(generation,dict) else None

    def _charge_load(self, pending, now, wall, container=None):
        """Charge a real/uncertain restart, or a Compose start proven non-no-op.

        The resulting start consumes the allowance even without INFO proof.
        Its identity, unlike catalog identity alone, permits a later autonomous
        start one necessary recovery load. It never resets the restart limit.
        """
        submitted=pending['submitted_wall']
        if not pending.get('snapshot'): return
        if submitted>=self.last_load_wall:
            self.last_load_identity=pending['snapshot'].identity
            self.last_load_wall=submitted
            self.last_load_container=container or pending.get('result_container') or pending['old_container']
            self.state['version']=2
            self.state['last_restart']=max(self.state.get('last_restart',float('-inf')),now-(wall-submitted))

    def close(self): self.executor.shutdown(wait=True)

    def _fence(self, ctx, deadline, require_tunnel=True, container_only=False):
        if (settings_revision(self.paths)!=ctx['revision'] or read_intent(self.paths).get('revision')!=ctx['intent_revision']
                or not may_recover(self.paths)):
            return False
        if container_only:
            # Server selection belongs to the container, not one tun0 instance:
            # Gluetun restart-loops a dead endpoint every few seconds, so the
            # observed tunnel is routinely gone by the time the PUT is fenced.
            actual=self.a.current(deadline)
            return (actual.get('running') is True and
                    self.container_key(actual.get('generation') or actual)==self.container_key(ctx.get('generation')))
        if require_tunnel:
            try: return self.a.control.observe_generation(deadline=deadline)==ctx['generation']
            except Exception: return False
        actual=self.a.current(deadline)
        if actual.get('running') is None: return False
        if not actual.get('running') and not actual.get('affirmative_failure'): return False
        current_gen=actual.get('generation') or actual
        return self.container_key(current_gen)==self.container_key(ctx.get('generation') or ctx['observation'])

    def _perform(self, ctx):
        action=ctx['action']; deadline=self.a.mono()+30
        allowed=[True]
        submission = {}
        # True once any precondition has authorized a request: from then on
        # the request may have reached Gluetun, so a failure is not 'unsent'.
        authorized=[False]
        def precondition():
            allowed[0]=self._fence(ctx,deadline,require_tunnel=bool(ctx.get('generation')),
                                   container_only=action=='rotate_server' and bool(ctx.get('generation')))
            authorized[0]=authorized[0] or allowed[0]
            return allowed[0]
        try:
            if action=='catalog-refresh':
                try: return {'status':'catalog','result':self.a.store.refresh(self.a.wall())}
                except Exception: return {'status':'catalog','result':{'status':'failed'}}
            if action=='sync_port':
                return {'status':'sync','result':self.a.ports.sync(ctx['generation'],deadline=deadline,precondition=precondition)}
            if action=='rotate_server':
                candidate=ctx['candidate']
                payload={'provider':{'server_selection':{'countries':[candidate.country], 'hostnames':[candidate.hostname],
                          'wireguard':{'endpoint_ip':candidate.endpoint_ip}}}}
                # SetSettings in the pinned loop restarts the VPN on change.
                # A separate stop/start pair would create an unsafe stopped gap.
                outcome=self.a.control.put('/v1/vpn/settings',payload,ctx['generation']['container_id'],precondition=precondition,deadline=deadline)
                if outcome=='settings left unchanged':
                    self.a.control.put('/v1/vpn/status',{'status':'stopped'},ctx['generation']['container_id'],precondition=precondition,deadline=deadline)
                    def start_precondition():
                        allowed[0]=self._fence(ctx,deadline,require_tunnel=False)
                        authorized[0]=authorized[0] or allowed[0]
                        return allowed[0]
                    self.a.control.put('/v1/vpn/status',{'status':'running'},ctx['generation']['container_id'],precondition=start_precondition,deadline=deadline)
                return {'status':'issued'}
            with bounded_lock(self.paths.compose_lock,.25):
                if not precondition(): raise Deferred()
                atomic_json(self.paths.runtime/'action.json',{'revision':ctx['revision'],'intent_revision':ctx['intent_revision'],'container':self.container_key(ctx.get('generation') or ctx['observation'])})
                if action == 'restart_container':
                    def record_submission():
                        submitted_wall = self.a.wall()
                        operation = {'generation':ctx.get('generation') or self.container_key(ctx['observation']),
                                     'started_wall':submitted_wall, 'until_wall':submitted_wall+60,
                                     'owner_pid':os.getpid(), 'owner_start':process_start(os.getpid()), 'boot_id':boot_id()}
                        atomic_json(self.paths.state/'operation.json',operation)
                        if ctx.get('snapshot'):
                            atomic_json(self.paths.state/'load-pending.json',
                                        {'provider':ctx['snapshot'].provider,
                                         'old_container':self.container_key(ctx.get('generation') or ctx['observation']),
                                         'submitted_wall':submitted_wall})
                        self.operation = operation
                        submission.update(wall=submitted_wall, mono=self.a.mono(),
                                          catalog_identity=ctx['snapshot'].identity if ctx.get('snapshot') else None)
                    # The adapter invokes this only after deadline checks and
                    # immediately before submitting Docker I/O under our lock.
                    self.a.container_action(action,ctx['observation'],deadline,on_submit=record_submission)
                else:
                    self.a.container_action(action,ctx['observation'],deadline)
            return {'status':'issued', 'submission':submission}
        except ControlError as exc:
            return {'status':'deferred' if not allowed[0] or str(exc)=='control mutation deferred' else 'failed',
                    'submission':submission,'authorized':authorized[0]}
        except (Deferred,TimeoutError,OSError):
            return {'status':'deferred', 'submission':submission,'authorized':authorized[0]}
        except Exception:
            return {'status':'failed' if allowed[0] else 'deferred', 'submission':submission,'authorized':authorized[0]}

    def _complete(self, o, now):
        """now is the tick's clock read: decide() rejects any later timer."""
        if not self.pending or not self.pending.done(): return
        ctx=self.pending_context
        try: result=self.pending.result()
        except Exception: result={'status':'failed'}
        self.pending=None;self.pending_context=None
        if ctx['action']=='catalog-refresh':
            # Failure is throttled too; don't fetch every 10-second tick.
            r=result.get('result',{})
            delay=max(600,min(88200,r.get('next_due',self.a.wall()+600)-self.a.wall())) if finite(r.get('next_due')) else 600
            self.next_refresh=self.a.mono()+delay
            return
        if ctx['action'] == 'restart_container':
            submission = result.get('submission') or {}
            if submission:
                # Submitted-but-uncertain Docker I/O still consumes the bound.
                # This bookkeeping is attribution, never start permission.
                self.last_load_identity = submission['catalog_identity']
                self.last_load_wall = submission['wall']
                self.state['last_restart'] = submission['mono']
            elif result['status'] == 'deferred' and self.state.get('last_restart') == ctx.get('decision_mono'):
                # A lock/precondition deferral made no request. Restore only
                # our own provisional timers, preserving newer observations.
                for key in ('last_restart', 'candidate_until'):
                    previous = ctx.get('restart_before', {}).get(key)
                    if previous is None:
                        self.state.pop(key, None)
                    else:
                        self.state[key] = previous
        if (ctx['action']=='rotate_server' and result.get('status')=='deferred' and result.get('authorized') is False
                and finite(ctx.get('decision_mono')) and self.state.get('candidate_until')==ctx['decision_mono']+CANDIDATE_GRACE_S
                and ctx['revision']==settings_revision(self.paths)
                and ctx['intent_revision']==read_intent(self.paths).get('revision')):
            # Nothing was sent and nothing changed but a busy lock: retry
            # after two ticks instead of costing the whole 120 s window. A
            # settings/intent change (or a possibly-sent request) keeps it.
            previous=ctx.get('restart_before',{}).get('candidate_until')
            self.state['candidate_until']=max(previous if finite(previous) else float('-inf'),ctx['decision_mono']+20)
        if (ctx['revision']!=settings_revision(self.paths) or ctx['intent_revision']!=read_intent(self.paths).get('revision')):
            return
        # Container identity: a tick that missed tun0 (mid-restart) still saw
        # the same container, and an issued rotation must be recorded.
        if ctx['action'] in ('rotate_server','sync_port') and self.container_key(o.get('generation') or o)!=self.container_key(ctx['generation']):
            return
        if ctx['action']=='rotate_server':
            if result['status']=='failed': self.api_failed=True
            if result['status']=='issued':
                candidate=ctx['candidate'];self.attempted={**asdict(candidate),'prior_generation':ctx['generation']}
                if candidate.hostname not in self.round_attempts: self.round_attempts.append(candidate.hostname)
                # The candidate is judged on its own evidence: tun0 changes in
                # the same container no longer reset the loss clocks, so clear
                # the previous server's here (candidate grace is unchanged).
                for key in ('traffic_lost','port_lost','control_lost','traffic_unknown','port_unknown'):
                    self.state.pop(key,None)
                self.state.update(started=now,had_port=False,port_was_present=False,losses=[])
        elif ctx['action']=='restart_container' and result.get('submission'):
            self.api_failed=False
            self.load_pending={'snapshot':ctx.get('snapshot'),'old_container':self.container_key(ctx.get('generation') or ctx['observation']),
                               'submitted_wall':result['submission']['wall'],'operation':'restart'}
        if ctx['action'] in ('relink_dependents','converge_dependents'):
            self.last_converge=self.a.mono()
            self.last_converge_container=self.container_key(ctx.get('generation'))

    def _record_failure(self, candidate):
        if not candidate:return
        value=asdict(candidate) if not isinstance(candidate,dict) else candidate
        self.failures=record_failure(self.failures,value['hostname'],value['endpoint_ip'],self.a.wall())
        if value['hostname'] not in self.round_attempts:self.round_attempts.append(value['hostname'])

    def tick(self):
        now,wall=self.a.mono(),self.a.wall(); deadline=now+9
        config_revision = None
        try:
            before_revision = settings_revision(self.paths)
            config = self.a.config()
            if settings_revision(self.paths) == before_revision:
                config_revision = before_revision
        except Exception:
            config = {}
        traffic_due = now >= self.next_traffic
        probe_start = now
        try: o=self.a.observe(deadline=deadline,traffic_due=traffic_due)
        except Exception: o={'running':None}
        now,wall=self.a.mono(),self.a.wall()
        # Advance only a requested probe, anchored to its scheduled start.
        # Observation completion crossing a deadline must not skip that probe.
        if traffic_due:
            self.next_traffic = probe_start + 30
        actual={k:o.get(k) for k in ('running','unit_active','configured')}
        adopt_running(self.paths,actual)
        if o.get('explicit_stop'):
            op=self.operation or {}
            event_gen=o.get('stop_generation')
            event_container=o.get('stop_container') or (event_gen or {}).get('container_id')
            own=(finite(o.get('stop_at')) and isinstance(op.get('generation'),dict) and finite(op.get('started_wall')) and finite(op.get('until_wall')) and
                 event_container==op['generation'].get('container_id') and
                 op.get('started_wall',wall+1)<=o['stop_at']<=op.get('until_wall',0))
            own = own or owns_stop(self.paths, event_container, o.get('stop_at'), wall)
            if not own:set_intent(self.paths,False,'observed Docker stop')
        config_current = config_revision is not None and settings_revision(self.paths) == config_revision
        o['allowed'] = may_recover(self.paths,actual) and config_current
        self._complete(o,now)
        generation=o.get('generation'); container=self.container_key(generation)
        # Health can be unknown while a previously attested catalog remains
        # loaded in the same container. Only affirmative container-start
        # evidence invalidates that snapshot; missing tun0/fence data does not.
        observed_container = self.container_key(generation or o)
        if (observed_container and isinstance(observed_container['container_id'], str)
                and re.fullmatch('[a-f0-9]{64}', observed_container['container_id'])
                and type(observed_container['pid']) is int and observed_container['pid'] > 0
                and isinstance(observed_container['started_at'], str) and observed_container['started_at']):
            if self.loaded_container != observed_container:
                self.loaded = None
        else:
            observed_container=None
        # A new container start reverts Gluetun to the .env selection: our
        # earlier PUT no longer applies, so the server it brought up must be
        # judged on its own evidence, not as a failed attempt at ours.
        if (self.attempted and observed_container and self.container_key(self.attempted.get('prior_generation'))
                and observed_container!=self.container_key(self.attempted.get('prior_generation'))):
            self.attempted=None
        # Supported lifecycle commands can load a catalog without restarting
        # this polling owner. Consume their real submitted-start journal with
        # exactly the same new-container/disk/INFO attestation as own restarts.
        external_load=read_json(self.paths.state/'load-pending.json',9*1024*1024)
        try:
            submitted=external_load['submitted_wall']
            if finite(submitted) and 0<=wall-submitted<=180 and submitted>getattr(self,'external_load_wall',0):
                snapshot=validate_catalog(external_load['provider'],wall)
                self.load_pending={'snapshot':snapshot,'old_container':external_load['old_container'],
                                   'submitted_wall':submitted,'operation':external_load.get('operation','restart')}
                self.external_load_wall=submitted
                if self.load_pending['operation']!='start':
                    self._charge_load(self.load_pending,now,wall)
        except (KeyError,ValueError,TypeError): pass
        if self.load_pending:
            pending=self.load_pending
            changed=bool(observed_container and observed_container.get('pid') and
                         observed_container!=pending['old_container'])
            result_container=pending.get('result_container')
            if result_container and observed_container and observed_container!=result_container:
                # This submission already produced one start. A subsequent
                # autonomous start cannot inherit either its proof or charge.
                self.load_pending=None
                (self.paths.state/'load-pending.json').unlink(missing_ok=True)
            elif changed:
                pending['result_container']=observed_container
                self._charge_load(pending,now,wall,observed_container)
            if self.load_pending and wall-pending['submitted_wall']>180:
                self.load_pending=None
                (self.paths.state/'load-pending.json').unlink(missing_ok=True)
            elif self.load_pending and changed and generation:
                snapshot=pending['snapshot']
                try:
                    if snapshot and self.a.attest_catalog(generation,snapshot,deadline):
                        self.loaded=snapshot;self.loaded_container=container
                        atomic_json(self.paths.state/'loaded.json',{'provider':snapshot.provider,'container':container})
                        self.load_pending=None
                        (self.paths.state/'load-pending.json').unlink(missing_ok=True)
                except Exception: pass
        try:
            path=getattr(self.a.store,'path',None)
            metadata=path.stat() if path else None
            signature=(metadata.st_ino,metadata.st_size,metadata.st_mtime_ns) if metadata else None
            if signature is not None and signature==self.downloaded_signature:
                downloaded=self.downloaded_cache
            else:
                downloaded=self.a.store.load()
                self.downloaded_cache=downloaded;self.downloaded_signature=signature
        except Exception: downloaded=None
        # Current compose/parser explicitly disable IPv6. Keep its metadata in
        # the catalog, but do not submit an unreachable IPv6 endpoint override.
        candidates=[c for c in eligible_candidates(self.loaded,[]) if ipaddress.ip_address(c.endpoint_ip).version==4] if self.loaded else []
        effective=next((c for c in candidates if c.endpoint_ip==o.get('endpoint')),None)
        self.effective=asdict(effective) if effective else None
        preferred=config.get('preferred_countries') or sorted({c.country for c in candidates})
        fallback=config.get('fallback_countries',[])
        histories={'failures':self.failures,'round_attempts':self.round_attempts}
        candidate=select_candidate(candidates,histories,preferred,fallback,wall)
        expired_backoff='backoff_until' in self.state and now>=self.state['backoff_until']
        if expired_backoff:
            candidate=select_candidate(candidates,{'failures':self.failures,'round_attempts':[]},preferred,fallback,wall)
        recent={e['endpoint_ip'] for e in bounded_history(self.failures,wall) if wall-e['failed_at']<1800}
        recycled=bool(candidate and candidate.endpoint_ip in recent)
        o.update(api_failed=self.api_failed, candidate_available=bool(candidate) and (not recycled or expired_backoff),
                 busy=self.pending is not None)
        # A failed/missing log attestation is not permission to restart over and
        # over merely seeking logs. Retry that same load at most daily; a newly
        # downloaded identity or autonomous container start may authorize one
        # necessary recovery load sooner. Its resulting start is charged too.
        can_load=bool(downloaded and (downloaded.identity!=self.last_load_identity or
                      not finite(self.last_load_wall) or wall-self.last_load_wall>=86400 or
                      (observed_container and observed_container!= (self.last_load_container or self.loaded_container))))
        o['catalog_unattested']=self.loaded is None and can_load
        o['catalog_needs_load']=bool(self.loaded and downloaded and self.loaded.identity!=downloaded.identity and can_load)
        if self.attempted and isinstance(self.attempted,dict):
            o['selection_verified']=(effective is not None and effective.endpoint_ip==self.attempted.get('endpoint_ip')
                                     and generation!=self.attempted.get('prior_generation'))
            o['selection_failed']=not o['selection_verified'] and now>=self.state.get('candidate_until',now+1)
        if container and o.get('namespaces_match') is True:
            self.relinked=container
        needs_relink=(o.get('namespaces_match') is False or
                      bool(container and self.relinked and container!=self.relinked))
        # A replacement start needs its own first repair promptly. Charge the
        # completion to its exact container/PID/start, never to tun0 identity;
        # missing observations and soft reconnects cannot reset the allowance.
        new_start=bool(container and container==observed_container and
                       self.last_converge_container and container!=self.last_converge_container)
        converge_due=new_start or now-self.last_converge>=300
        o['needs_relink']=needs_relink and converge_due
        o['relink_waiting']=needs_relink and not o['needs_relink']
        if not converge_due:o['needs_converge']=False
        restart_before = {key:self.state.get(key) for key in ('last_restart','candidate_until')}
        intent_revision=read_intent(self.paths).get('revision')
        if intent_revision!=self.last_intent_revision:
            o['selection_reset']=True
            self.last_intent_revision=intent_revision
        decision=decide(o,self.state,config,now,wall)
        self.state=decision['next_state']
        if decision.get('new_round'):
            self.round_attempts=[]
        action=decision['action']
        if action in ('rotate_server','restart_container'):
            # Record the currently observed/last attempted endpoint exactly once
            # per decision; short success never clears its history penalty.
            if not decision.get('new_round'):
                self._record_failure(self.attempted or effective)
            candidate=select_candidate(candidates,{'failures':self.failures,'round_attempts':self.round_attempts},preferred,fallback,wall)
            just_excluded={e['endpoint_ip'] for e in self.failures if wall-e['failed_at']<1800}
            if action=='rotate_server' and (candidate is None or (candidate.endpoint_ip in just_excluded and not decision.get('new_round'))):
                action='none';self.state.pop('candidate_until',None)
                level=self.state.get('backoff_level',0)
                level=level if type(level) is int and 0<=level<=2 else 0
                self.state.update(backoff_until=now+(120,300,600)[level],backoff_level=min(2,level+1))
                decision['reason']='Recovery backoff' 
        if action!='none' and not self.pending:
            ctx={'action':action,'revision':config_revision,'intent_revision':read_intent(self.paths).get('revision'),
                 'generation':generation,'observation':copy.deepcopy(o),'candidate':candidate,'snapshot':downloaded,
                 'decision_mono':now,'restart_before':restart_before}
            self.last_reason=decision['reason']
            # One sanitized journal line per mutation (hostnames are public
            # catalog data; never settings, keys or HTTP bodies).
            target=(' -> %s (%s)'%(candidate.hostname,candidate.country)) if action=='rotate_server' and candidate else ''
            print('[vpn-recovery] %s: %s%s'%(action,decision['reason'],target),flush=True)
            self.pending_context=ctx;self.pending=self.executor.submit(self._perform,ctx)
        elif not self.pending and not self.load_pending and now>=self.next_refresh and o.get('allowed'):
            self.next_refresh=now+600
            self.pending_context={'action':'catalog-refresh'}
            self.pending=self.executor.submit(self._perform,self.pending_context)
        if decision['reason']=='Ready':
            self.api_failed=False
            self.last_success=utc(wall)
            # A round is one incident: once the recovered selection has been
            # Ready continuously for the exclusion period it is stable, so the
            # next incident tries the preferred country again. A shorter
            # success keeps the round, so a flapping country still reaches
            # the allowed fallback (spec: short success must not erase it).
            if self.ready_since is None:
                self.ready_since=now
            elif self.round_attempts and now-self.ready_since>=ROUND_STABLE_S:
                self.round_attempts=[]
            if self.effective:
                self.last_selection={k:v for k,v in self.effective.items() if k!='public_key'}
            self.attempted=None
        status={'version':1,'checked_at':utc(wall),'state':decision['reason'],'ready':decision['reason']=='Ready',
                'traffic_verified':o.get('traffic') is True and generation is not None and o.get('traffic_generation',generation)==generation and finite(o.get('traffic_at')) and 0<=now-o['traffic_at']<=40,
                'forwarded_port':o.get('port'),'qbit_synced':o.get('qbit_synced',False),
                'dependents_ok':o.get('dependents_ok',False),'generation':generation,
                'effective':{k:v for k,v in (self.effective or {}).items() if k!='public_key'},
                'desired':config,'catalog':{'downloaded':downloaded.identity if downloaded else None,
                'loaded':self.loaded.identity if self.loaded else None,'age_s':max(0,wall-downloaded.timestamp) if downloaded else None},
                'last_success':self.last_success,'last_recovery_reason':self.last_reason,
                'next_retry':utc(wall+max(0,self.state.get('backoff_until',self.state.get('candidate_until',now))-now)),
                'failures':self.failures[-20:]}
        if decision['reason']!='Ready':
            self.ready_since=None
        self.paths.state.mkdir(parents=True,exist_ok=True)
        atomic_json(self.paths.state/'status.json',status,mode=0o644)
        atomic_json(self.paths.state/'controller.json',{'version':1,'saved_wall':wall,'saved_mono':now,'policy':self.state,
            'failures':self.failures,'round_attempts':self.round_attempts,'attempted':self.attempted,
            'relinked':self.relinked,
            'last_converge_container':self.last_converge_container,
            'last_converge_wall':wall-(now-self.last_converge) if self.last_converge!=-300 else None,
            'last_load_container':self.last_load_container,'last_load_identity':self.last_load_identity,'last_load_wall':self.last_load_wall,
            'refresh_after':wall+max(0,self.next_refresh-now),'last_success':self.last_success,'last_selection':self.last_selection,'last_recovery_reason':self.last_reason})
        return status
