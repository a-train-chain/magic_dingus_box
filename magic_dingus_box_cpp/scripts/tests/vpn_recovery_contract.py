#!/usr/bin/env python3
"""Explicit opt-in, disposable v3.41.3 contract test. Never targets the stack.

Run as Linux root only after review; see the validation runbook. No image pull,
production credentials, production mounts, published ports, or host net changes.
Only fixed assertion names/booleans cross the output boundary, never responses.
"""
import argparse
import base64
from concurrent.futures import Future
import contextlib
import copy
import json
import math
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from vpn_recovery.common import Paths, atomic_json, atomic_write
from vpn_recovery.control import ControlClient, ControlError, ensure_control_credentials, _HTTP_SCRIPT
from vpn_recovery.catalog import validate_catalog
from vpn_recovery.intent import set_intent
from vpn_recovery.runtime import Controller, SystemAdapters, parse_endpoint

PINNED_IMAGE='qmcgaw/gluetun:v3.41.3@sha256:fa19cc76b2af13d57a8d3dc3066f2ada061b1c761b8aecf989b3877c0486e027'
LABEL='com.magic-dingus.vpn-contract-owner'
DUMMY_PRIVATE_KEY=base64.b64encode(bytes([1])*32).decode()
DUMMY_PUBLIC_KEY=base64.b64encode(bytes([2])*32).decode()
FIXTURE=Path(__file__).parent/'fixtures/vpn_recovery/proton-v4-sample.json'


class ContractFailure(RuntimeError):
    pass


def dummy_catalog():
    provider=json.loads(FIXTURE.read_text())
    provider['timestamp']=int(time.time())-60
    for server in provider['servers']:
        server['wgpubkey']=DUMMY_PUBLIC_KEY
        server['ips']=[ip for ip in server['ips'] if ':' not in ip]
    return validate_catalog(provider,time.time())


class Harness:
    def __init__(self, *, container, fixture_dir, auth_dir, opt_in=False, runner=subprocess.run, budget=180):
        if opt_in is not True or not isinstance(container,str) or not re.fullmatch(r'mdb-vpn-contract-[0-9a-f]{8,32}',container):
            raise ValueError('explicit disposable container and opt-in required')
        root=Path(fixture_dir).absolute()
        auth=Path(auth_dir).absolute()
        temp_roots=[Path(p).resolve() for p in ('/tmp','/private/tmp',tempfile.gettempdir())]
        if (root.name!=container or ',' in str(root) or root.exists() or root.is_symlink()
                or not root.parent.is_dir() or not any(root.resolve().is_relative_to(p) and root.resolve()!=p for p in temp_roots)
                or auth != root/'services/config/gluetun/auth'):
            raise ValueError('new temporary fixture and contained auth path required')
        if type(budget) not in (int,float) or not 1 <= budget <= 180:
            raise ValueError('invalid contract deadline')
        self.name,self.root,self.auth=container,root,auth
        self.runner,self.deadline=runner,time.monotonic()+budget
        self.owner=secrets.token_hex(16)
        self.paths=Paths(base=root,state=root/'state',runtime=root/'run',compose_lock=root/'compose.lock')
        self.assertions=[];self.submitted=False;self.prepared=False;self.cid=None

    def check(self,name,condition):
        self.assertions.append({'name':name,'ok':bool(condition)})
        if not condition:raise ContractFailure('contract mismatch')

    def command(self,args,*,input=None,cap=10,cleanup=False,allow_failure=False):
        timeout=cap if cleanup else min(cap,self.deadline-time.monotonic())
        if timeout<=0:raise ContractFailure('contract deadline exceeded')
        try:
            result=self.runner(args,input=input,capture_output=True,text=True,timeout=timeout,check=False)
            if len(result.stdout)>1048576 or (result.returncode and not allow_failure):raise ValueError()
            return result
        except Exception:
            raise ContractFailure('contract command failed or timed out') from None

    def preflight(self):
        found=self.command(['docker','container','ls','-a','--filter','name=^/'+self.name+'$','--format','{{.ID}}']).stdout
        self.check('disposable_name_absent',not found.strip())
        digests=json.loads(self.command(['docker','image','inspect','--format','{{json .RepoDigests}}',PINNED_IMAGE]).stdout)
        self.check('exact_image_cached',isinstance(digests,list) and PINNED_IMAGE.split(':v3.41.3')[0]+'@'+PINNED_IMAGE.split('@')[1] in digests)

    def prepare(self):
        self.root.mkdir(mode=0o700);self.prepared=True
        self.paths.services.mkdir();self.paths.compose_lock.touch(mode=0o600)
        ensure_control_credentials(self.paths)
        self.snapshot=dummy_catalog()
        atomic_json(self.paths.services/'config/gluetun/servers.json',{'version':1,'protonvpn':self.snapshot.provider})
        first=self.snapshot.provider['servers'][0]
        env=('VPN_SERVICE_PROVIDER=protonvpn\nVPN_TYPE=wireguard\nVPN_PORT_FORWARDING=on\n'
             'VPN_COUNTRIES=Canada\nSERVER_COUNTRIES=Canada\nSERVER_HOSTNAMES='+first['hostname']+'\n'
             'WIREGUARD_ENDPOINT_IP='+first['ips'][0]+'\nWIREGUARD_PRIVATE_KEY='+DUMMY_PRIVATE_KEY+'\n'
             'VPN_INTERFACE=tun0\nWIREGUARD_ADDRESSES=10.2.0.2/32\nDNS_SERVER=off\n'
             'HEALTH_RESTART_VPN=off\nUPDATER_PERIOD=0\nPUBLICIP_ENABLED=off\n'
             'LOG_LEVEL=info\nVERSION_INFORMATION=off\n')
        atomic_write(self.root/'dummy.env',env.encode())
        atomic_write(self.paths.services/'.env',env.encode())

    def create_command(self):
        # Priority-1 policy blackholes are outside WireGuard's own routing table.
        # The IPs remain global for the exact production partial-PUT validator;
        # they cannot leave this disposable namespace, even during startup.
        ips=sorted({ip for s in self.snapshot.provider['servers'] for ip in s['ips']})
        commands=[]
        for ip in ips:
            commands += [f'ip route add blackhole {ip}/32 table 200',
                         f'ip rule add priority 1 to {ip}/32 table 200']
        bootstrap=' && '.join(commands+['exec /gluetun-entrypoint'])
        return ['docker','create','--pull=never','--name',self.name,'--label',LABEL+'='+self.owner,
                '--cap-add=NET_ADMIN','--device=/dev/net/tun','--network=bridge',
                '--sysctl=net.ipv6.conf.all.disable_ipv6=1','--env-file',str(self.root/'dummy.env'),
                '--mount','type=bind,src='+str(self.paths.services/'config/gluetun')+',dst=/gluetun',
                '--entrypoint=/bin/sh',PINNED_IMAGE,'-ec',bootstrap]

    def launch(self):
        self.submitted=True  # A timeout after dispatch may still have created it.
        self.cid=self.command(self.create_command()).stdout.strip()
        self.check('full_disposable_id',bool(re.fullmatch('[a-f0-9]{64}',self.cid)))
        self.command(['docker','start',self.cid])

    def wait_for(self,predicate,cap=25):
        until=min(self.deadline,time.monotonic()+cap)
        while time.monotonic()<until:
            try:
                value=predicate()
                if value:return value
            except (ControlError,ContractFailure):pass
            time.sleep(min(.2,max(0,until-time.monotonic())))
        raise ContractFailure('contract readiness timed out')

    def exec(self,*args,allow_failure=False):
        return self.command(['docker','exec',self.cid,*args],allow_failure=allow_failure)

    def blackholes(self):
        routes=self.exec('ip','route','show','table','200').stdout
        rules=self.exec('ip','rule','show').stdout
        ips={ip for s in self.snapshot.provider['servers'] for ip in s['ips']}
        return all(re.search(r'blackhole '+re.escape(ip)+r'(?:/32)?(?:\s|$)',routes)
                   and re.search(r'1:\s+from all to '+re.escape(ip)+r'(?:/32)? lookup 200',rules) for ip in ips)

    def verify_contract(self):
        client=ControlClient(self.paths,self.runner)
        generation=self.wait_for(lambda:client.observe_generation(self.cid,deadline=self.deadline))
        self.check('initial_tun0_identity',generation['tunnel_ifindex']>0)
        bindings=json.loads(self.command(['docker','inspect','--format','{{json .HostConfig.PortBindings}}',self.cid]).stdout)
        self.check('no_published_control_or_other_ports',not bindings)
        self.check('unroutable_dummy_endpoints_before',self.blackholes())
        self.check('authenticated_status_get',client.get('/v1/vpn/status',self.cid,deadline=self.deadline).get('status')=='running')
        before=client.get('/v1/vpn/settings',self.cid,deadline=self.deadline)
        self.check('dummy_private_key_only',before['wireguard']['private_key']==DUMMY_PRIVATE_KEY)
        for method,path,payload in [('GET','/v1/vpn/settings',None),('PUT','/v1/vpn/settings',{'provider':{'server_selection':{'countries':['Canada']}}}),
                                    ('PUT','/v1/vpn/status',{'status':'stopped'})]:
            envelope=json.loads(self.command(['nsenter','--target',str(generation['pid']),'--net','python3','-c',_HTTP_SCRIPT],
                input=json.dumps({'key':'','method':method,'path':path,'payload':payload})).stdout)
            self.check('unauthorized_'+method.lower()+'_'+path.rsplit('/',1)[1],envelope.get('ok') is False and envelope.get('status') in (401,403))
        # GET only: port forwarding has no PUT/restart method in this pin.
        self.check('public_forwarding_get',client.get('/v1/portforward',self.cid,deadline=self.deadline).get('port')==0)
        adapter=SystemAdapters(self.paths,self.runner)
        self.check('info_catalog_file_wins',adapter.attest_catalog(generation,self.snapshot,self.deadline))
        target=self.snapshot.provider['servers'][1]
        payload={'provider':{'server_selection':{'countries':['Canada'],'hostnames':[target['hostname']],
                    'wireguard':{'endpoint_ip':target['ips'][0]}}}}
        client.put('/v1/vpn/settings',payload,self.cid,deadline=self.deadline)
        changed=self.wait_for(lambda:self._changed(client,generation))
        after=client.get('/v1/vpn/settings',self.cid,deadline=self.deadline)
        selection=after['provider']['server_selection']
        self.check('partial_hostname_endpoint_applied',selection['hostnames']==[target['hostname']] and selection['wireguard']['endpoint_ip']==target['ips'][0])
        self.check('private_key_unchanged',after['wireguard']['private_key']==before['wireguard']['private_key'])
        self.check('unique_current_firewall_endpoint',parse_endpoint(self.exec('iptables','-S','OUTPUT').stdout)==target['ips'][0])
        outcome=client.put('/v1/vpn/settings',payload,self.cid,deadline=self.deadline)
        self.check('identical_settings_outcome',isinstance(outcome,str) and 'settings left unchanged' in outcome)
        self.check('identical_settings_preserve_tun0',client.observe_generation(self.cid,deadline=self.deadline)==changed)
        client.put('/v1/vpn/status',{'status':'stopped'},self.cid,deadline=self.deadline)
        self.check('authenticated_stop_status',client.get('/v1/vpn/status',self.cid,deadline=self.deadline).get('status')=='stopped')
        self.wait_for(lambda:self.exec('ip','-o','link','show','dev','tun0',allow_failure=True).returncode!=0)
        # Assert the firewall independently, while the VPN is explicitly down.
        firewall=self.exec('iptables','-S','OUTPUT').stdout
        self.check('firewall_output_default_drop','-P OUTPUT DROP' in firewall)
        self.check('pinned_gnu_wget_available',self.exec('timeout','3','wget','--version').stdout.startswith('GNU Wget'))
        help_result=self.exec('timeout','--help',allow_failure=True)
        self.check('pinned_busybox_timeout_available','BusyBox' in help_result.stdout+help_result.stderr)
        bound_started=time.monotonic()
        bounded=self.exec('timeout','-k','1','1','sleep','30',allow_failure=True)
        self.check('inner_child_is_bounded',bounded.returncode in (124,137,143) and time.monotonic()-bound_started<4)
        numeric=self.exec('timeout','3','wget','-qO-','--tries=1','--timeout=1','http://1.0.0.1',allow_failure=True)
        self.check('vpn_down_numeric_tcp_blocked',numeric.returncode!=0)
        # Actual pinned GNU wget + BusyBox timeout syntax; a successful network
        # probe while down is an immediate failure, not a fake VPN success.
        self.check('vpn_down_real_traffic_probe_blocked',not adapter._traffic(changed,self.deadline))
        self.check('unroutable_dummy_endpoints_after',self.blackholes())
        client.put('/v1/vpn/status',{'status':'running'},self.cid,deadline=self.deadline)
        restarted=self.wait_for(lambda:self._changed(client,changed))
        self.check('status_cycle_new_tun0',restarted['tunnel_ifindex']!=changed['tunnel_ifindex'])
        since=int(time.time())-2
        self.command(['docker','stop','--time','2',self.cid])
        events=self.command(['docker','events','--since',str(since),'--until',str(int(time.time())+1),
                            '--filter','container='+self.cid,'--filter','event=stop','--format','{{.TimeNano}} {{.Actor.ID}}']).stdout
        self.check('nanosecond_stop_event',any(len(f:=line.split())==2 and f[0].isdigit() and int(f[0])>10**18 and f[1]==self.cid for line in events.splitlines()))

    def _changed(self,client,old):
        generation=client.observe_generation(self.cid,deadline=self.deadline)
        return generation if generation!=old else None

    def cleanup_container(self):
        if not self.submitted:return 'not_created'
        try:
            result=self.command(['docker','inspect','--format','{{.Id}} {{index .Config.Labels "'+LABEL+'"}}',self.name],cleanup=True,cap=3)
            fields=result.stdout.split()
            if (len(fields)!=2 or not re.fullmatch('[a-f0-9]{64}',fields[0]) or fields[1]!=self.owner
                    or self.cid is not None and fields[0]!=self.cid):return 'failed'
            self.command(['docker','rm','--force',fields[0]],cleanup=True,cap=7)
            return 'removed'
        except Exception:return 'failed'

    def cleanup_files(self):
        if self.prepared:
            shutil.rmtree(self.root)
            self.prepared=False

    def execute(self):
        ok=False
        try:
            self.preflight();self.prepare();self.launch();self.verify_contract();ok=True
        except (Exception,KeyboardInterrupt):pass
        finally:
            cleanup=self.cleanup_container()
            if cleanup!='failed':
                try:self.cleanup_files()
                except Exception:cleanup='failed'
        return {'ok':ok and cleanup!='failed','assertions':self.assertions,
                'error':None if ok and cleanup!='failed' else 'contract_failed','cleanup':cleanup}


class _Immediate:
    def submit(self,fn,*args):
        future=Future()
        try:future.set_result(fn(*args))
        except Exception as exc:future.set_exception(exc)
        return future
    def shutdown(self,**kwargs):pass


class _ReplayTransport:
    """Deterministic external-process boundary; policy/adapters stay real."""
    def __init__(self):
        # Whole-second start (never behind the real clock deadlines use): the
        # replay lands exactly on policy thresholds, and offset+100-(offset+10)
        # must be exactly 90 whatever the host's uptime is.
        self.elapsed=0;self.offset=float(math.ceil(time.monotonic()));self.wall=time.time()
        self.generation={'container_id':'a'*64,'pid':42,'started_at':'2026-11-14T00:00:00Z','tunnel_ifindex':2}
        self.port=12345;self.prefs={'listen_port':12345,'upnp':False,'random_port':False}
        self.endpoint='84.20.16.29';self.writes=0;self.selections=[]

    def __call__(self,args,**kwargs):
        code=0
        if args[:2]==['systemctl','is-active']:out='active'
        elif args[:2]==['docker','inspect']:
            g=self.generation
            if args[3].startswith('{{.Name}}'):
                # Dependent namespace proof: Gluetun plus each running dependent.
                out='\n'.join(f"/{name} {(g['container_id'] if name=='mdb_gluetun' else format(i,'x')*64)} "
                              f"{g['pid'] if name=='mdb_gluetun' else 100+i} {g['started_at']} true"
                              for i,name in enumerate(args[4:]))+'\n'
            else:
                out=f"{g['container_id']} {g['pid']} {g['started_at']} true"
                if '.State.OOMKilled' in args[3]:out+=' false running'
        elif args[:2]==['docker','events']:out=''
        elif args[:2]==['docker','ps']:
            out='\n'.join('mdb_'+name+' running Up' for name in ('radarr','sonarr','prowlarr','qbittorrent','byparr'))
        elif args[:2]==['docker','exec']:
            if args[3]=='ip':out=f"{self.generation['tunnel_ifindex']}: tun0: <POINTOPOINT,UP> mtu 1320\n"
            elif args[3]=='iptables':out=f'-P OUTPUT DROP\n-A OUTPUT -d {self.endpoint}/32 -o eth0 -p udp -m udp --dport 51820 -j ACCEPT\n'
            elif args[3]=='timeout':out='ip=198.51.100.2\n'
            else:raise AssertionError('unexpected Docker operation')
        elif args[0]=='nsenter':
            request=json.loads(kwargs['input']);route=request['path']
            if request['method']=='GET' and route=='/v1/portforward':body=json.dumps({'port':self.port})
            elif request['method']=='PUT' and route=='/v1/vpn/settings':
                selection=request['payload']['provider']['server_selection']
                self.selections.append(copy.deepcopy(selection))
                self.endpoint=selection['wireguard']['endpoint_ip']
                body='VPN settings updated'
            else:raise AssertionError('unexpected control operation')
            out=json.dumps({'ok':True,'status':200,'body':body})
        elif args[0]==sys.executable and kwargs.get('input'):
            request=json.loads(kwargs['input'])
            if request['method']=='POST':self.prefs=request['payload'];self.writes+=1
            body=json.dumps(self.prefs) if request['method']=='GET' else ''
            out=json.dumps({'status':200,'body':body})
        else:raise AssertionError('unexpected external operation')
        return subprocess.CompletedProcess(args,code,out,'')


def replay_failure_to_success():
    """Replay is adapter/policy proof, explicitly not a live VPN success."""
    with tempfile.TemporaryDirectory(prefix='mdb-vpn-replay-') as temporary:
        root=Path(temporary);paths=Paths(base=root,state=root/'state',runtime=root/'run',compose_lock=root/'lock')
        paths.services.mkdir();paths.compose_lock.touch()
        atomic_write(paths.services/'.env',b'VPN_SERVICE_PROVIDER=protonvpn\nVPN_TYPE=wireguard\nVPN_PORT_FORWARDING=on\nVPN_COUNTRIES=Canada\nMDB_QBIT_PASS=dummy-only\n')
        atomic_json(paths.config/'vpn_recovery.json',{'fallback_countries':['United States']})
        ensure_control_credentials(paths)
        snapshot=dummy_catalog()
        atomic_json(paths.services/'config/gluetun/servers.json',{'version':1,'protonvpn':snapshot.provider})
        set_intent(paths,True,'fixture setup')
        transport=_ReplayTransport();adapters=SystemAdapters(paths,transport)
        adapters.mono=lambda:transport.offset+transport.elapsed
        adapters.wall=lambda:transport.wall+transport.elapsed
        adapters.executor=lambda:_Immediate()
        controller=Controller(paths,adapters)
        controller.loaded=snapshot;controller.loaded_container=controller.container_key(transport.generation)
        controller.next_refresh=adapters.mono()+86400
        before=(paths.services/'.env').read_bytes();states=[]
        def tick(elapsed):
            transport.elapsed=elapsed
            return controller.tick()
        # Only TCP reachability is stubbed outside the command transport. Keep
        # the real dependent parser, observations, intent, fencing and storage.
        try:
            # Kernel namespace identity is read from procfs, outside the
            # command transport: every simulated container shares Gluetun's.
            real_readlink=os.readlink
            def readlink(path,*args,**kwargs):
                if re.fullmatch(r'/proc/[0-9]+/ns/net',str(path)):return 'net:[4026532001]'
                return real_readlink(path,*args,**kwargs)
            with patch('socket.create_connection',return_value=contextlib.nullcontext()),\
                 patch('os.readlink',side_effect=readlink):
                states.append(tick(0)['state'])
                transport.port=0
                states.append(tick(10)['state'])
                tick(100)  # established port absent for 90s -> actual partial PUT
                # Even fresh traffic+port from the prior tunnel cannot be Ready.
                transport.port=34567
                stale=tick(110)
                states.append(stale['state'])
                transport.generation={**transport.generation,'tunnel_ifindex':3}
                tick(120)  # real PortSynchronizer writes and reads back qBit
                final=tick(130);states.append(final['state'])
        finally:controller.close()
        return {'states':states,'selected_countries':[s['countries'][0] for s in transport.selections],
                'preference_writes':transport.writes,'desired_unchanged':before==(paths.services/'.env').read_bytes(),
                'stale_not_ready':stale['ready'] is False,'ready':final['ready']}


def parse_args(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--opt-in-disposable-contract',action='store_true',required=True)
    parser.add_argument('--container',required=True)
    parser.add_argument('--fixture-dir',required=True,type=Path)
    parser.add_argument('--auth-dir',required=True,type=Path)
    return parser.parse_args(argv)


def main(argv=None):
    args=parse_args(argv)
    try:
        h=Harness(container=args.container,fixture_dir=args.fixture_dir,auth_dir=args.auth_dir,opt_in=args.opt_in_disposable_contract)
        if not sys.platform.startswith('linux') or os.geteuid()!=0:raise ValueError()
        def interrupted(*_):raise KeyboardInterrupt()
        previous=signal.signal(signal.SIGTERM,interrupted)
        try:result=h.execute()
        finally:signal.signal(signal.SIGTERM,previous)
    except Exception:
        result={'ok':False,'assertions':[],'error':'unsafe_or_unavailable_contract_target','cleanup':'not_created'}
    print(json.dumps(result,sort_keys=True))
    return 0 if result['ok'] else 1


if __name__=='__main__':sys.exit(main())
