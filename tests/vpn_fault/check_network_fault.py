#!/usr/bin/env python3
"""Attended, opt-in, namespace-only VPN faults. Never runs on import.

Root owns baseline/deployment/Ready gates. This helper never changes a file in
the installation, credentials, desired policy, torrent state or kiosk service.
Only its root-private /run manifest, finite transient units, owned netns rules,
and (first-lease only) one supported same-country selection PUT are written.
"""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import signal
import socket
import stat
import subprocess
import sys
import time


class FaultError(RuntimeError):
    pass


def emit(value):
    print(json.dumps(value, sort_keys=True), flush=True)


def dependencies(directory):
    global Paths, atomic_json, bounded_lock, load_config, read_status
    global read_intent, read_json, settings_revision, release_inhibit
    global SystemAdapters, parse_endpoint, CatalogStore, eligible_candidates
    global select_candidate, bounded_history, validate_catalog, begin_owner, operation_allowed
    sys.path.insert(0, str(Path(directory).resolve()))
    from vpn_recovery.common import Paths, atomic_json, bounded_lock, load_config
    from vpn_recovery.cli import read_status, begin_owner, operation_allowed
    from vpn_recovery.intent import read_intent, read_json, settings_revision, release_inhibit
    from vpn_recovery.runtime import SystemAdapters, parse_endpoint
    from vpn_recovery.catalog import CatalogStore, eligible_candidates, select_candidate, bounded_history, validate_catalog


def run(argv, end, *, cap=3, allowed_failure=False, pass_fds=()):
    remaining = min(cap, end-time.monotonic())
    if remaining <= 0:
        raise FaultError('aggregate_deadline')
    try:
        result = subprocess.run(argv, capture_output=True, text=True,
                                timeout=remaining, check=False, pass_fds=pass_fds)
        if len(result.stdout) > 2*1024*1024 or result.returncode and not allowed_failure:
            raise FaultError('bounded_command_failed')
        return result
    except (OSError, subprocess.SubprocessError):
        raise FaultError('bounded_command_failed') from None


def proc_identity(pid):
    # comm can include spaces/parentheses: fields after the final ')' start at3.
    tail = Path('/proc/%s/stat' % pid).read_text().rsplit(')', 1)[1].split()
    return {'pid_startticks': int(tail[19]),
            'netns_inode': os.stat('/proc/%s/ns/net' % pid).st_ino}


def identity(cid, end):
    out = run(['docker', 'inspect', '--format',
               '{{.Id}} {{.State.Pid}} {{.State.StartedAt}} {{.State.Running}}', cid], end).stdout.split()
    if len(out) != 4 or out[0] != cid or out[3] != 'true' or not out[1].isdigit() or int(out[1]) <= 0:
        raise FaultError('namespace_identity_unavailable')
    return {'container_id': out[0], 'pid': int(out[1]), 'started_at': out[2]}


def namespace_fence(spec, end):
    before = identity(spec['generation']['container_id'], end)
    wanted = {key: spec['generation'][key] for key in ('container_id', 'pid', 'started_at')}
    if before != wanted or proc_identity(before['pid']) != spec['process']:
        raise FaultError('namespace_identity_changed')
    if identity(before['container_id'], end) != before:
        raise FaultError('namespace_identity_changed')
    return before['pid']


def net_ip(spec, args, end, *, allowed_failure=False):
    namespace_fence(spec, end)
    with owned_namespace(spec) as fd:
        # Retain insertion's stronger Docker fence after pinning. A later PID
        # reuse cannot redirect nsenter: it receives only this verified FD.
        namespace_fence(spec, end)
        return run(['nsenter', '--net=/proc/self/fd/'+str(fd), '--', '/usr/sbin/ip', '-4', *args],
                   end, allowed_failure=allowed_failure, pass_fds=(fd,))


def destination(value):
    return str(ipaddress.IPv4Network(value, strict=False))


def table_number(value):
    aliases = {'local': 255, 'main': 254, 'default': 253}
    return aliases.get(str(value), int(value) if str(value).isdigit() else -1)


def objects(spec, item, end, executor=None):
    execute = executor or net_ip
    rules = json.loads(execute(spec, ['-j', 'rule', 'show'], end).stdout)
    routes = json.loads(execute(spec, ['-j', 'route', 'show', 'table', 'all'], end).stdout)
    at_priority = [r for r in rules if r.get('priority') == item['priority']]
    in_table = [r for r in routes if table_number(r.get('table', 'main')) == item['table']]
    def own_rule(rule):
        # protocol is installation-origin metadata (ip-rule(8)), not a match
        # selector. All other unknown fields fail closed before deletion.
        permitted = {'priority', 'src', 'dst', 'table', 'protocol'}
        if item['natpmp']:
            # iproute2 >= 6.x emits dport_mask for an exact-port rule; only
            # the full mask (exactly port 5351) is ours, any other mask widens
            # or narrows the selector and fails closed.
            permitted |= {'ipproto', 'dport', 'dport_mask'}
        return (not set(rule)-permitted
                and str(rule.get('dport_mask', '0xffff')).lower() in ('0xffff', '65535')
                and destination(rule.get('dst', '0.0.0.0/0')) == item['destination']
                and rule.get('src', 'all') == 'all'
                and table_number(rule.get('table')) == item['table']
                and (str(rule.get('ipproto')) in ('udp', '17')
                     and str(rule.get('dport')) == '5351' if item['natpmp'] else True))
    def own_route(route):
        return (destination(route.get('dst', '0.0.0.0/0')) == item['destination']
                and route.get('type') == item['kind'] and not route.get('gateway')
                and not route.get('dev') and not route.get('multipath'))
    if len(at_priority) > 1 or len(in_table) > 1 or any(not own_rule(r) for r in at_priority) or any(not own_route(r) for r in in_table):
        raise FaultError('owned_tuple_has_foreign_contents')
    return bool(at_priority), bool(in_table)


def rule_args(item, verb):
    args = ['rule', verb, 'priority', str(item['priority']), 'to', item['destination']]
    if item['natpmp']:
        args += ['ipproto', 'udp', 'dport', '5351']
    return args + ['lookup', str(item['table'])]


@contextmanager
def owned_namespace(spec):
    # Shared identity utility: startticks+inode prove and pin the original ns.
    # Cleanup can use this even if Docker is unavailable. Insertion must also
    # retain its stronger Docker fences before and after this utility.
    pid = spec['generation']['pid']
    if proc_identity(pid) != spec['process']:
        raise FaultError('cleanup_original_process_unproven')
    fd = os.open('/proc/%s/ns/net' % pid, os.O_RDONLY | os.O_CLOEXEC)
    try:
        if os.fstat(fd).st_ino != spec['process']['netns_inode'] or proc_identity(pid) != spec['process']:
            raise FaultError('cleanup_original_namespace_unproven')
        yield fd
    finally:
        os.close(fd)


def cleanup_ip(fd, spec, args, end, *, allowed_failure=False):
    return run(['nsenter', '--net=/proc/self/fd/'+str(fd), '--', '/usr/sbin/ip', '-4', *args],
               end, allowed_failure=allowed_failure, pass_fds=(fd,))


def cleanup(spec, end):
    # Removing our network fault is always permitted, even after operator stop.
    # Never reapply it to a new tunnel/container; never write lifecycle/intent.
    try:
        return _cleanup_pinned(spec, end)
    except FileNotFoundError:
        if Path('/proc/%s' % spec['generation']['pid']).exists():
            raise
        # The pinned Gluetun process is gone (its container restarted): our
        # rules lived only in its namespace, which nothing here can reach.
        # Dependents briefly left in it are relinked by the controller. A
        # reused pid still fails proc_identity and is never touched.
        return {'cleanup': 'original_process_gone', 'rules': len(spec['rules']),
                'namespace_proof': 'pinned_process_absent'}


def _cleanup_pinned(spec, end):
    with owned_namespace(spec) as fd:
        def execute(spec, args, end, **kwargs):
            return cleanup_ip(fd, spec, args, end, **kwargs)
        for item in reversed(spec['rules']):
            present_rule, present_route = objects(spec, item, end, execute)
            if present_rule:
                execute(spec, rule_args(item, 'del'), end, allowed_failure=True)
            if present_route:
                execute(spec, ['route', 'del', item['kind'], item['destination'], 'table', str(item['table'])], end, allowed_failure=True)
            if objects(spec, item, end, execute) != (False, False):
                raise FaultError('cleanup_absence_unproven')
    return {'cleanup': 'exact_objects_absent', 'rules': len(spec['rules']),
            'namespace_proof': 'pinned_original_startticks_inode'}


def validated_spec(path):
    path = Path(path)
    info = path.lstat()
    directory = path.parent.stat()
    if (directory.st_uid != 0 or stat.S_IMODE(directory.st_mode) != 0o700
            or info.st_uid != 0 or stat.S_IMODE(info.st_mode) != 0o600 or not stat.S_ISREG(info.st_mode)
            or info.st_nlink != 1 or path.name != 'manifest.json'
            or not re.fullmatch(r'mdb-vpn-network-[0-9a-f]{16}', path.parent.name)
            or path.parent.parent != Path('/run') or path.parent.is_symlink()):
        raise FaultError('invalid_cleanup_manifest')
    spec = json.loads(path.read_text())
    if (spec.get('version') != 1 or path.parent.name != 'mdb-vpn-network-'+spec.get('token', '')
            or not re.fullmatch('[0-9a-f]{64}', spec['generation']['container_id'])
            or type(spec['generation']['pid']) is not int or spec['generation']['pid'] <= 0
            or len(spec['rules']) > 2):
        raise FaultError('invalid_cleanup_manifest')
    for item in spec['rules']:
        if (not 1 <= item['priority'] < 1000 or not 20000 <= item['table'] < 30000
                or item['kind'] not in ('unreachable', 'blackhole')
                or type(item['natpmp']) is not bool
                or destination(item['destination']) != item['destination']
                or not item['destination'].endswith('/32')):
            raise FaultError('invalid_cleanup_manifest')
    return spec


def preserved(paths):
    files = [paths.services/'.env', paths.config/'vpn_recovery.json',
             Path('/tmp/mdb_playback_services_paused'),
             paths.base/'magic_dingus_box_cpp/data/qbit_paused_by_kiosk.services',
             paths.base/'magic_dingus_box_cpp/data/qbit_paused_by_kiosk.services.claims',
             Path('/var/lib/magic-dingus/drive_guard/stopped_hashes'),
             Path('/var/lib/magic-dingus/drive_guard/add_stopped_prev.json'),
             Path('/tmp/mdb_drive_guard_paused')]
    result = {}
    for path in files:
        try:
            info = path.stat()
            if info.st_size > 1024*1024:
                raise FaultError('preservation_file_oversized')
            result[str(path)] = [info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode),
                                 hashlib.sha256(path.read_bytes()).hexdigest()]
        except FileNotFoundError:
            result[str(path)] = None
    return result


class LiveFault:
    def __init__(self, mode, scripts, gateway=None):
        self.mode, self.scripts, self.gateway = mode, str(Path(scripts).resolve()), gateway
        self.paths = Paths()
        self.adapter = SystemAdapters(self.paths)
        self.end = time.monotonic()+45
        self.spec = None
        self.manifest = None
        self.armed = False
        self.cleaned = False
        self.owner_token = None
        self.last_emit = None
        self.traffic_at = 0
        self.traffic = None
        self.kiosk_checked = 0
        self.wan = None
        self.clock_offset = time.time()-time.monotonic()

    def gen(self, end=None):
        return self.adapter.control.observe_generation(deadline=min(self.end, end or self.end))

    def allowed(self, generation=None):
        state = read_intent(self.paths)
        own = {self.owner_token} if self.owner_token else set()
        return (time.monotonic() < self.end and state.get('enabled') is True
                and state.get('revision') == self.spec['intent_revision']
                and set(state.get('inhibits', {})) == own
                and settings_revision(self.paths) == self.spec['settings_revision']
                and preserved(self.paths) == self.spec['preserved']
                and (generation is None or self.gen() == generation))

    def save(self):
        atomic_json(self.manifest, self.spec)

    def record(self, value):
        value = {'mono': time.monotonic(), 'wall': time.time(), **value}
        with (self.manifest.parent/'observations.jsonl').open('a') as handle:
            os.chmod(handle.name, 0o600)
            handle.write(json.dumps(value, sort_keys=True)+'\n')
        fingerprint = {k: value.get(k) for k in ('event', 'generation', 'endpoint', 'port', 'traffic', 'state')}
        if fingerprint != self.last_emit:
            emit(value)
            self.last_emit = fingerprint
        return value

    def preflight(self):
        if self.mode not in ('port-loss', 'first-lease', 'fallback'):
            raise FaultError('invalid_mode')
        status = read_status(self.paths)
        if not all(status.get(k) is True for k in ('ready', 'traffic_verified', 'qbit_synced', 'dependents_ok')):
            raise FaultError('fresh_Ready_required')
        generation = self.gen()
        observation = self.adapter.ports.observe(deadline=self.end)
        if observation.get('generation') != generation or observation.get('status') != 'synced':
            raise FaultError('fresh_port_alignment_required')
        intent = read_intent(self.paths)
        if not intent.get('enabled') or intent.get('inhibits'):
            raise FaultError('enabled_uninhibited_intent_required')
        token = secrets.token_hex(8)
        directory = Path('/run/mdb-vpn-network-'+token)
        directory.mkdir(mode=0o700)
        self.manifest = directory/'manifest.json'
        self.spec = {'version': 1, 'token': token, 'mode': self.mode,
                     'generation': generation, 'process': proc_identity(generation['pid']),
                     'settings_revision': settings_revision(self.paths),
                     'intent_revision': intent['revision'], 'preserved': preserved(self.paths),
                     'rules': [], 'created_wall': time.time()}
        self.save()
        namespace_fence(self.spec, self.end)
        kiosk = run(['systemctl', 'show', 'magic-dingus-box-cpp.service',
                     '--property=MainPID,ExecMainStartTimestampMonotonic,NRestarts,ActiveState'], self.end).stdout
        if 'ActiveState=active' not in kiosk or 'MainPID=0\n' in kiosk:
            raise FaultError('kiosk_running_required')
        self.spec['kiosk'] = kiosk
        self.save()
        disk_catalog = CatalogStore(self.paths).load()
        loaded = read_json(self.paths.state/'loaded.json', 9*1024*1024)
        self.catalog = validate_catalog(loaded.get('provider'), time.time())
        container = {key: generation[key] for key in ('container_id', 'pid', 'started_at')}
        if (loaded.get('container') != container
                or disk_catalog.identity != self.catalog.identity
                or status.get('catalog', {}).get('loaded') != self.catalog.identity
                or status.get('catalog', {}).get('downloaded') != self.catalog.identity
                or not self.adapter.attest_catalog(generation, self.catalog, self.end)):
            raise FaultError('current_file_and_loaded_catalog_attestation_required')
        self.candidates = [c for c in eligible_candidates(self.catalog, [])
                           if ipaddress.ip_address(c.endpoint_ip).version == 4]
        self.initial = next((c for c in self.candidates if c.endpoint_ip == status.get('effective', {}).get('endpoint_ip')), None)
        if (not self.initial or not self.adapter._traffic(generation, self.end)
                or not self.adapter._traffic(generation, self.end, host=True)):
            raise FaultError('effective_selection_and_fresh_traffic_required')
        actual_endpoint = self.endpoint()
        if actual_endpoint != self.initial.endpoint_ip:
            raise FaultError('actual_endpoint_mismatch')
        self.durable = read_json(self.paths.state/'controller.json')
        self.initial_failures = self.durable.get('failures', [])
        if self.mode == 'fallback':
            config = load_config(self.paths)
            if (self.initial.country != 'Canada' or config.get('preferred_countries') != ['Canada']
                    or config.get('fallback_countries') != ['United States']):
                raise FaultError('Canada_then_US_policy_and_initial_Canada_required')
            history = {'failures': bounded_history(self.initial_failures+[
                {'hostname': self.initial.hostname, 'endpoint_ip': self.initial.endpoint_ip, 'failed_at': time.time()}], time.time()),
                'round_attempts': list(dict.fromkeys(self.durable.get('round_attempts', [])+[self.initial.hostname]))}
            self.second = select_candidate(self.candidates, history, ['Canada'], ['United States'], time.time())
            if (not self.second or self.second.country != 'Canada' or self.second.hostname == self.initial.hostname
                    or self.second.endpoint_ip == self.initial.endpoint_ip):
                raise FaultError('history_does_not_allow_distinct_second_Canada')
            history['failures'].append({'hostname': self.second.hostname, 'endpoint_ip': self.second.endpoint_ip, 'failed_at': time.time()})
            history['round_attempts'].append(self.second.hostname)
            self.fallback = select_candidate(self.candidates, history, ['Canada'], ['United States'], time.time())
            if not self.fallback or self.fallback.country != 'United States':
                raise FaultError('history_does_not_project_US_after_two_Canada')
            # Never faulted: any allowed US server the controller moves on to
            # (the projected one may simply never lease a port) is proof.
            self.fallback_pool = {c.endpoint_ip: c for c in self.candidates if c.country == 'United States'}
        elif self.mode == 'first-lease':
            recent = {v['endpoint_ip'] for v in bounded_history(self.initial_failures, time.time()) if time.time()-v['failed_at'] < 1800}
            self.target = next((c for c in self.candidates if c.country == self.initial.country
                                and c.hostname != self.initial.hostname and c.endpoint_ip != self.initial.endpoint_ip
                                and c.endpoint_ip not in recent), None)
            if self.target is None:
                raise FaultError('distinct_same_country_candidate_required')
        if self.mode != 'fallback':
            if self.gateway is None:
                raise FaultError('reviewed_actual_NAT_PMP_gateway_required')
            parsed = ipaddress.IPv4Address(self.gateway)
            if not parsed.is_private or parsed.is_loopback or parsed.is_unspecified or parsed.is_link_local:
                raise FaultError('invalid_reviewed_gateway')
        self.record({'event': 'preflight', 'generation': generation, 'endpoint': actual_endpoint,
                     'country': self.initial.country, 'port': observation['forwarded_port'],
                     'second': self.second.hostname if self.mode == 'fallback' else None,
                     'projected_US': self.fallback.hostname if self.mode == 'fallback' else None})

    def arm(self):
        maximum = {'port-loss': 240, 'fallback': 600}.get(self.mode, 360)
        self.end = time.monotonic()+maximum-55
        self.spec['insertion_deadline_mono'] = self.end
        self.spec['maximum_fault_seconds'] = maximum
        self.save()
        unit = self.manifest.parent.name
        run(['systemd-run', '--quiet', '--unit='+unit, '--on-active='+str(maximum-50)+'s',
             '--timer-property=AccuracySec=1s', '--property=Type=oneshot',
             '--property=TimeoutStartSec=35s', '--property=TimeoutStopSec=3s',
             '--property=KillMode=control-group', sys.executable, str(Path(__file__).resolve()),
             '--scripts', self.scripts, '--cleanup', str(self.manifest)], self.end, cap=5)
        run(['systemctl', 'is-active', '--quiet', unit+'.timer'], self.end)
        expiration = run(['systemctl', 'show', '--value', '--property=NextElapseUSecMonotonic', unit+'.timer'], self.end).stdout.strip()
        if not expiration or expiration in ('0', 'n/a', 'infinity'):
            raise FaultError('independent_restoration_not_armed')
        self.armed = True

    def endpoint(self):
        output = run(['docker', 'exec', self.spec['generation']['container_id'], 'iptables', '-S', 'OUTPUT'], self.end).stdout
        return parse_endpoint(output)

    def add(self, address, *, natpmp, generation):
        with bounded_lock(self.paths.compose_lock, min(.25, max(.001, self.end-time.monotonic()))):
            if not self.armed or not self.allowed(generation):
                raise FaultError('insertion_fence_changed')
            if len(self.spec['rules']) >= (2 if self.mode == 'fallback' else 1):
                raise FaultError('rule_count_limit')
            rules = json.loads(net_ip(self.spec, ['-j', 'rule', 'show'], self.end).stdout)
            routes = json.loads(net_ip(self.spec, ['-j', 'route', 'show', 'table', 'all'], self.end).stdout)
            used_p = {r.get('priority') for r in rules}
            first = min([p for p in used_p if type(p) is int and p > 0] or [1000])
            options = [p for p in range(1, min(first, 1000)) if p not in used_p]
            if not options:
                raise FaultError('no_unused_early_priority')
            used_t = {table_number(r.get('table', 'main')) for r in routes+rules}
            tables = [t for t in range(20000, 30000) if t not in used_t]
            if not tables:
                raise FaultError('no_unused_table')
            item = {'destination': destination(address+'/32'), 'natpmp': natpmp,
                    'kind': 'unreachable' if natpmp else 'blackhole',
                    'priority': max(options), 'table': secrets.choice(tables)}
            if objects(self.spec, item, self.end) != (False, False):
                raise FaultError('allocated_objects_not_empty')
            self.spec['rules'].append(item)
            self.save()  # A lost add reply still has independently owned cleanup.
            if not self.allowed(generation):
                raise FaultError('insertion_fence_changed')
            net_ip(self.spec, ['route', 'add', item['kind'], item['destination'], 'table', str(item['table'])], self.end)
            if not self.allowed(generation):
                raise FaultError('generation_changed_before_active_rule')
            net_ip(self.spec, rule_args(item, 'add'), self.end)
            if objects(self.spec, item, self.end) != (True, True):
                raise FaultError('owned_rule_readback_failed')
            if not self.allowed(generation):
                raise FaultError('generation_changed_during_active_rule')
            self.record({'event': 'owned_fault_added', 'generation': generation,
                         'endpoint': address, 'selector': 'udp5351' if natpmp else 'peer32'})

    def first_lease(self):
        self.owner_token = secrets.token_hex(16)
        envelope = begin_owner(self.paths, self.owner_token, 'attended first lease test', 45, os.getpid())
        self.spec['intent_revision'] = envelope['MDB_VPN_INTENT_REVISION']
        self.save()
        try:
            generation = self.gen()
            self.add(self.gateway, natpmp=True, generation=generation)
            payload = {'provider': {'server_selection': {'countries': [self.target.country],
                       'hostnames': [self.target.hostname], 'wireguard': {'endpoint_ip': self.target.endpoint_ip}}}}
            outcome = self.adapter.control.put('/v1/vpn/settings', payload, generation['container_id'],
                precondition=lambda: self.allowed(generation), deadline=min(self.end, time.monotonic()+8))
            if isinstance(outcome, str) and 'settings left unchanged' in outcome:
                raise FaultError('settings_noop_invalid_first_lease_attempt')
            until = min(self.end, time.monotonic()+25)
            while time.monotonic() < until:
                try:
                    new = self.gen(until)
                    if new != generation:
                        if any(new[k] != generation[k] for k in ('container_id', 'pid', 'started_at')):
                            raise FaultError('replacement_namespace_invalid_first_lease_attempt')
                        port = self.adapter.control.get('/v1/portforward', new['container_id'], deadline=until).get('port')
                        if port != 0:
                            raise FaultError('first_observation_not_zero_with_working_traffic')
                        # The WireGuard handshake completes a moment after tun0
                        # appears (live 2026-11-15: False, then True 5 s later).
                        # Bounded settle; zero port and the same generation are
                        # re-proven before every retry.
                        settle = min(until, time.monotonic()+15)
                        while not self.adapter._traffic(new, settle):
                            if time.monotonic() >= settle:
                                raise FaultError('first_observation_not_zero_with_working_traffic')
                            time.sleep(1)
                            if (self.gen(settle) != new or self.adapter.control.get(
                                    '/v1/portforward', new['container_id'], deadline=settle).get('port') != 0):
                                raise FaultError('first_observation_not_zero_with_working_traffic')
                        if self.endpoint() != self.target.endpoint_ip:
                            raise FaultError('first_lease_selected_endpoint_mismatch')
                        if any(objects(self.spec, rule, until) != (True, True) for rule in self.spec['rules']):
                            raise FaultError('fault_did_not_survive_tunnel_change')
                        self.record({'event': 'new_generation_first_port_zero', 'generation': new,
                                     'endpoint': self.target.endpoint_ip, 'port': 0, 'traffic': True})
                        return new
                except FaultError:
                    raise
                except Exception:
                    pass  # Missing tun0 during the internal supported transition.
                time.sleep(min(.5, max(0, until-time.monotonic())))
            raise FaultError('new_tunnel_generation_deadline')
        finally:
            release_inhibit(self.paths, self.owner_token)
            self.owner_token = None
            self.spec['intent_revision'] = read_intent(self.paths).get('revision')
            self.save()

    def sample(self, *, force_traffic=False):
        namespace_fence(self.spec, self.end)
        if abs(time.time()-time.monotonic()-self.clock_offset) > 1:
            raise FaultError('wall_clock_step_invalidates_action_timing')
        if not self.allowed():
            raise FaultError('intent_settings_or_owner_journals_changed')
        if time.monotonic()-self.kiosk_checked >= 10:
            current = run(['systemctl', 'show', 'magic-dingus-box-cpp.service',
                           '--property=MainPID,ExecMainStartTimestampMonotonic,NRestarts,ActiveState'], self.end).stdout
            if current != self.spec['kiosk']:
                raise FaultError('kiosk_identity_or_restarts_changed')
            self.kiosk_checked = time.monotonic()
        if not self.cleaned and any(objects(self.spec, item, self.end) != (True, True) for item in self.spec['rules']):
            raise FaultError('fault_rule_disappeared')
        observed = self.adapter.ports.observe(deadline=min(self.end, time.monotonic()+8))
        generation = observed.get('generation')
        status = read_status(self.paths)
        durable = read_json(self.paths.state/'controller.json')
        endpoint = self.endpoint()
        now = time.monotonic()
        if generation and (force_traffic or now-self.traffic_at >= 15 or generation != getattr(self, 'traffic_generation', None)):
            self.traffic = self.adapter._traffic(generation, min(self.end, now+4))
            self.traffic_at, self.traffic_generation = time.monotonic(), generation
            if self.traffic is False:
                self.wan = self.adapter._traffic(generation, min(self.end, time.monotonic()+4), host=True)
                if self.wan is not True:
                    raise FaultError('host_WAN_unavailable_stop_injection')
        policy = durable.get('policy', {})
        record = {'event': 'observation', 'generation': generation, 'endpoint': endpoint,
                  'port': observed.get('forwarded_port'), 'qbit_port': observed.get('qbit_port'),
                  'port_status': observed.get('status'), 'traffic': self.traffic,
                  'traffic_at': self.traffic_at, 'traffic_generation': getattr(self, 'traffic_generation', None),
                  'wan': self.wan,
                  'state': status.get('state'), 'status_ready': status.get('ready'),
                  'dependents_ok': status.get('dependents_ok'),
                  'status_generation': status.get('generation'), 'effective': status.get('effective'),
                  'controller_saved_mono': durable.get('saved_mono'), 'controller_saved_wall': durable.get('saved_wall'),
                  'policy': {k: policy.get(k) for k in ('generation', 'started', 'had_port', 'port_lost', 'traffic_lost', 'candidate_until', 'last_restart')},
                  'failures': durable.get('failures', []),
                  'attempted': {k: v for k, v in (durable.get('attempted') or {}).items() if k != 'public_key'},
                  'round_attempts': durable.get('round_attempts', [])}
        return self.record(record)

    def remove(self):
        if self.cleaned:
            return
        result = cleanup(self.spec, time.monotonic()+30)
        self.cleaned = True
        self.record({'event': 'fault_removed', **result})
        if self.armed:
            run(['systemctl', 'stop', self.manifest.parent.name+'.timer'], time.monotonic()+3)

    def ready(self, expected_endpoint=None, *, sticky=False):
        self.end = time.monotonic()+120
        ready_gen = None
        traffic_checks = []
        next_check = 0
        while time.monotonic() < self.end:
            sample = self.sample(force_traffic=time.monotonic() >= next_check)
            fresh_traffic = sample['traffic_generation'] == sample['generation'] and sample['traffic'] is True
            valid = (sample['status_ready'] is True and sample['dependents_ok'] is True and sample['port_status'] == 'synced'
                     and sample['port'] == sample['qbit_port'] and sample['port']
                     and fresh_traffic and sample['status_generation'] == sample['generation']
                     and (expected_endpoint is None or sample['endpoint'] == expected_endpoint))
            if valid:
                if not sticky:
                    return sample
                if ready_gen is None:
                    ready_gen = sample['generation']
                if sample['generation'] != ready_gen:
                    raise FaultError('healthy_US_generation_changed_during_sticky_check')
                if time.monotonic() >= next_check:
                    traffic_checks.append(sample['traffic_at'])
                    next_check = time.monotonic()+30
                if len(traffic_checks) >= 4 and traffic_checks[-1]-traffic_checks[0] >= 90:
                    return sample
            elif ready_gen is not None:
                raise FaultError('healthy_US_did_not_remain_Ready')
            time.sleep(min(1, max(0, self.end-time.monotonic())))
        raise FaultError('Ready_or_sticky_deadline')

    def execute(self):
        result = {'ok': False, 'validity': 'unproven', 'mode': self.mode}
        try:
            self.preflight()
            self.arm()
            base = self.first_lease() if self.mode == 'first-lease' else self.gen()
            initial = self.target if self.mode == 'first-lease' else self.initial
            if self.mode != 'first-lease':
                self.add(self.initial.endpoint_ip if self.mode == 'fallback' else self.gateway,
                         natpmp=self.mode != 'fallback', generation=base)
            clocks, actions = {}, {}
            second_added = False
            second_generation = None
            zero_started = False
            while time.monotonic() < self.end:
                sample = self.sample()
                generation = sample['generation']
                policy = sample['policy']
                # Record the controller's own actions first: a sample taken
                # while its rotation replaces tun0 is unknown by nature.
                for failed in sample['failures']:
                    if failed not in self.initial_failures:
                        saved_wall, saved_mono = sample['controller_saved_wall'], sample['controller_saved_mono']
                        if type(saved_wall) in (int, float) and type(saved_mono) in (int, float):
                            actions.setdefault(failed['endpoint_ip'], failed['failed_at']-(saved_wall-saved_mono))
                if (self.mode != 'fallback' and (zero_started or self.mode == 'first-lease')
                        and (not isinstance(generation, dict) or not generation
                             or type(sample['port']) is not int or not 0 <= sample['port'] <= 65535)):
                    acted = actions.get(initial.endpoint_ip)
                    if type(acted) in (int, float) and type(sample.get('mono')) in (int, float) and acted <= sample['mono']:
                        # After the recorded action: proves nothing either way.
                        time.sleep(min(1, max(0, self.end-time.monotonic())))
                        continue
                    raise FaultError('unknown_sample_invalidates_continuous_zero')
                if generation and policy.get('generation') == generation:
                    clock = policy.get('traffic_lost') if self.mode == 'fallback' else policy.get('started') if self.mode == 'first-lease' else policy.get('port_lost')
                    if type(clock) in (int, float):
                        clocks[sample['endpoint']] = clock
                if self.mode == 'fallback':
                    endpoint = sample['endpoint']
                    if endpoint not in (self.initial.endpoint_ip, self.second.endpoint_ip, None) and endpoint not in self.fallback_pool:
                        raise FaultError('unexpected_endpoint_do_not_chase')
                    if endpoint == self.second.endpoint_ip and generation and generation != base and not second_added:
                        self.add(endpoint, natpmp=False, generation=generation)
                        second_added = True
                        second_generation = generation
                    if (endpoint in self.fallback_pool and sample['status_ready'] and sample['dependents_ok']
                            and sample['status_generation'] == generation
                            and sample['effective'].get('country') == 'United States'
                            and sample['port_status'] == 'synced' and sample['traffic'] is True
                            and sample['traffic_generation'] == generation):
                        if not second_added:
                            raise FaultError('US_without_second_Canada_fault_unproven')
                        reached = self.fallback_pool[endpoint]
                        if generation in (base, second_generation) or not {self.initial.hostname, self.second.hostname, reached.hostname}.issubset(set(sample['round_attempts'])):
                            raise FaultError('controller_three_selections_and_generations_unproven')
                        for candidate in (self.initial, self.second):
                            if candidate.endpoint_ip not in clocks or candidate.endpoint_ip not in actions or actions[candidate.endpoint_ip]-clocks[candidate.endpoint_ip] < 60:
                                raise FaultError('controller_60s_loss_action_timing_unproven')
                        self.remove()
                        self.ready(reached.endpoint_ip, sticky=True)
                        result.update(fallback_reached=reached.hostname, projected_fallback=self.fallback.hostname)
                        result.update(ok=True, validity='observed', controller_loss_to_action_s={ip: round(actions[ip]-clocks[ip], 3) for ip in clocks if ip in actions})
                        break
                else:
                    if generation == base:
                        if self.mode == 'first-lease' and sample['port'] != 0:
                            raise FaultError('first_generation_did_not_remain_actual_port_zero')
                        if sample['port'] == 0:
                            zero_started = True
                        elif zero_started:
                            raise FaultError('continuous_actual_zero_not_proven')
                        if sample['traffic'] is not True or sample['traffic_generation'] != generation:
                            raise FaultError('port_only_fault_traffic_not_working')
                    elif generation:
                        elapsed = actions.get(initial.endpoint_ip, -1)-clocks.get(initial.endpoint_ip, float('inf'))
                        threshold = 120 if self.mode == 'first-lease' else 90
                        if not zero_started or elapsed < threshold:
                            raise FaultError('controller_port_grace_action_timing_unproven')
                        target = next((c for c in self.candidates if c.endpoint_ip == sample['endpoint']), None)
                        if not target or target.endpoint_ip == initial.endpoint_ip:
                            raise FaultError('controller_distinct_candidate_transition_unproven')
                        self.remove()
                        recovered = self.ready(target.endpoint_ip)
                        if target.hostname not in recovered['round_attempts'] or target.hostname in self.durable.get('round_attempts', []):
                            raise FaultError('controller_candidate_issue_unproven')
                        result.update(ok=True, validity='observed', controller_loss_to_action_s=round(elapsed, 3))
                        break
                time.sleep(min(1, max(0, self.end-time.monotonic())))
            else:
                raise FaultError('bounded_fault_window_expired_without_proof')
        except FaultError as exc:
            result['reason'] = str(exc)
        except (Exception, KeyboardInterrupt):
            result['reason'] = 'operation_failed_or_interrupted'
        finally:
            if self.spec is not None:
                try:
                    self.remove()
                    result['cleanup'] = 'exact_objects_absent'
                    if not result['ok'] and self.armed:
                        try:
                            self.ready()
                            result['ready_after_cleanup'] = True
                        except Exception:
                            result['ready_after_cleanup'] = False
                except Exception:
                    result.update(ok=False, validity='unproven', cleanup='failed_or_namespace_changed_no_transfer')
                if preserved(self.paths) != self.spec['preserved']:
                    result.update(ok=False, validity='unproven', preservation='owner_journal_or_policy_drift')
                try:
                    current = run(['systemctl', 'show', 'magic-dingus-box-cpp.service',
                        '--property=MainPID,ExecMainStartTimestampMonotonic,NRestarts,ActiveState'], time.monotonic()+3).stdout
                    if current != self.spec.get('kiosk'):
                        result.update(ok=False, validity='unproven', kiosk='changed')
                except Exception:
                    result.update(ok=False, validity='unproven', kiosk='unverified')
                result['evidence_directory'] = str(self.manifest.parent)
        return result


def capability(scripts):
    # Existing owned dummy harness: no production paths, key, env or mounts.
    fixture = Path(scripts)/'tests/vpn_recovery_contract.py'
    module_spec = importlib.util.spec_from_file_location('mdb_fault_contract', fixture)
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    class CapabilityHarness(module.Harness):
        def own_object_diagnostics(self, spec, item):
            # Disposable-only and owned priority/table only. Never include a
            # command, stderr, env, HTTP response, auth or provider settings.
            end = min(self.deadline, time.monotonic()+5)
            fields = {'priority', 'src', 'srclen', 'dst', 'dstlen', 'table',
                      'ipproto', 'dport', 'dport_mask', 'dport_start', 'dport_end',
                      'sport', 'sport_mask', 'sport_start', 'sport_end',
                      'protocol', 'fwmark', 'fwmask', 'iif', 'oif', 'uid_start',
                      'uid_end', 'suppress_prefixlen', 'suppress_ifgroup',
                      'l3mdev', 'action', 'goto', 'not', 'tos', 'tun_id',
                      'type', 'scope', 'prefsrc', 'dev', 'gateway', 'flags', 'metric'}
            def safe(entry):
                result = {}
                for key, value in entry.items():
                    if key not in fields:
                        continue
                    if value is None or type(value) in (bool, int):
                        result[key] = value
                    elif isinstance(value, str):
                        result[key] = value[:128]
                    elif isinstance(value, list) and all(isinstance(v, str) for v in value):
                        result[key] = [v[:64] for v in value[:8]]
                result['other_field_names'] = sorted(k for k in entry if k not in fields and re.fullmatch('[A-Za-z0-9_]{1,64}', k))[:16]
                return result
            try:
                rules = json.loads(net_ip(spec, ['-j', 'rule', 'show'], end).stdout)
                routes = json.loads(net_ip(spec, ['-j', 'route', 'show', 'table', 'all'], end).stdout)
                rules = [r for r in rules if r.get('priority') == item['priority']]
                routes = [r for r in routes if table_number(r.get('table', 'main')) == item['table']]
                return {'owned_priority': item['priority'], 'owned_table': item['table'],
                        'rule_count': len(rules), 'route_count': len(routes),
                        'rules': [safe(r) for r in rules[:4]], 'routes': [safe(r) for r in routes[:4]]}
            except Exception:
                return {'snapshot': 'unavailable_under_original_namespace_fence'}

        def verify_contract(self):
            self.diagnostics['stage'] = 'await_dummy_generation'
            adapter = SystemAdapters(self.paths)
            generation = self.wait_for(lambda: adapter.control.observe_generation(self.cid, deadline=self.deadline))
            spec = {'generation': generation, 'process': proc_identity(generation['pid']), 'rules': []}
            item = {'destination': '10.2.0.1/32', 'natpmp': True, 'kind': 'unreachable', 'priority': 2, 'table': 20001}
            self.diagnostics['stage'] = 'verify_unused_tuple'
            self.check('capability_tuple_unused', objects(spec, item, self.deadline) == (False, False))
            spec['rules'].append(item)
            try:
                self.diagnostics['stage'] = 'add_unreachable_route'
                net_ip(spec, ['route', 'add', 'unreachable', item['destination'], 'table', '20001'], self.deadline)
                self.diagnostics['stage'] = 'add_udp5351_rule'
                net_ip(spec, rule_args(item, 'add'), self.deadline)
                self.diagnostics['stage'] = 'readback_owned_tuple'
                self.check('udp5351_rule_installed', objects(spec, item, self.deadline) == (True, True))
                probe = ('import socket,json\n'
                         'out={}\n'
                         'for port in (5351,5352,53):\n'
                         ' s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(.2)\n'
                         ' try:s.connect(("10.2.0.1",port));out[str(port)]=True\n'
                         ' except OSError:out[str(port)]=False\n'
                         ' s.close()\n'
                         'print(json.dumps(out))')
                def prove():
                    data = json.loads(self.command(['nsenter', '--target', str(generation['pid']), '--net', '--', 'python3', '-c', probe], cap=3).stdout)
                    return data == {'5351': False, '5352': True, '53': True}
                self.diagnostics['stage'] = 'prove_narrow_udp_selector'
                self.check('only_udp5351_unreachable', prove())
                target = self.snapshot.provider['servers'][1]
                self.diagnostics['stage'] = 'dummy_settings_put'
                adapter.control.put('/v1/vpn/settings', {'provider': {'server_selection': {'countries': ['Canada'],
                    'hostnames': [target['hostname']], 'wireguard': {'endpoint_ip': target['ips'][0]}}}}, self.cid, deadline=self.deadline)
                self.diagnostics['stage'] = 'await_dummy_new_generation'
                changed = self.wait_for(lambda: self._changed(adapter.control, generation))
                self.check('same_container_new_tun0', all(changed[k] == generation[k] for k in ('container_id', 'pid', 'started_at')) and changed['tunnel_ifindex'] != generation['tunnel_ifindex'])
                self.diagnostics['stage'] = 'prove_selector_persistence'
                self.check('selector_survived_tunnel_reset', objects(spec, item, self.deadline) == (True, True) and prove())
                self.check('dummy_endpoint_blackholes_preserved', self.blackholes())
            except Exception as exc:
                self.diagnostics['failure'] = str(exc) if isinstance(exc, FaultError) else 'sanitized_operation_failure'
                self.diagnostics['owned_objects_before_cleanup'] = self.own_object_diagnostics(spec, item)
                raise
            finally:
                try:
                    cleaned = cleanup(spec, time.monotonic()+30)
                    self.check('exact_selector_cleanup', cleaned['cleanup'] == 'exact_objects_absent')
                except Exception as exc:
                    self.diagnostics['cleanup_failure'] = str(exc) if isinstance(exc, FaultError) else 'sanitized_cleanup_failure'
                    self.diagnostics['owned_objects_after_cleanup_attempt'] = self.own_object_diagnostics(spec, item)
                    raise
    token = secrets.token_hex(8)
    name = 'mdb-vpn-contract-'+token
    root = Path('/tmp')/name
    harness = CapabilityHarness(container=name, fixture_dir=root, auth_dir=root/'services/config/gluetun/auth', opt_in=True)
    harness.diagnostics = {'stage': 'disposable_preflight'}
    result = harness.execute()
    result['diagnostics'] = harness.diagnostics
    return result


DEFAULT_BOX = 'magicpi5'
# One plain lowercase hostname label: no wildcard, no domain, no inference.
_BOX_NAME = re.compile(r'[a-z0-9][a-z0-9-]{0,62}')


def host_allowed(actual, requested):
    """Live faults run only on the box the operator names: magicpi5 unless
    --box names another (a bench Pi 4B). Naming another box never also
    allows the default one."""
    expected = DEFAULT_BOX if requested is None else requested
    return (isinstance(expected, str) and _BOX_NAME.fullmatch(expected) is not None
            and actual == expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scripts', default='/opt/magic_dingus_box/magic_dingus_box_cpp/scripts')
    parser.add_argument('--mode', choices=['port-loss', 'first-lease', 'fallback', 'capability'])
    parser.add_argument('--gateway', help='Actual reviewed NAT-PMP gateway; never inferred or widened')
    parser.add_argument('--cleanup')
    parser.add_argument('--opt-in-network-fault', action='store_true')
    parser.add_argument('--box', help='Hostname of the box to fault (default magicpi5); must equal this machine\'s hostname')
    args = parser.parse_args()
    if os.geteuid() != 0 or sys.platform != 'linux':
        raise SystemExit('Linux root required')
    dependencies(args.scripts)
    if args.cleanup:
        result = cleanup(validated_spec(args.cleanup), time.monotonic()+30)
        emit(result)
        return
    if not args.opt_in_network_fault or not args.mode:
        raise SystemExit('explicit opt-in and mode required')
    def interrupted(_signal, _frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    if args.mode == 'capability':
        result = capability(args.scripts)
    else:
        if not host_allowed(socket.gethostname(), args.box):
            raise SystemExit('production host identity mismatch')
        result = LiveFault(args.mode, args.scripts, args.gateway).execute()
    emit(result)
    if not result['ok']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
