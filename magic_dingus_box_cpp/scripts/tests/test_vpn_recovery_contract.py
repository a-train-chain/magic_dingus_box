"""Guardrail/cleanup failures must stop the opt-in contract recipe, never skip."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parent))
try:
    import vpn_recovery_contract as contract
except ImportError:
    contract=None


class HarnessCase(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(contract,'disposable contract harness is absent')
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)/'mdb-vpn-contract-a1b2c3d4'
        self.auth=self.root/'services/config/gluetun/auth'
        self.name='mdb-vpn-contract-a1b2c3d4'

    def harness(self, runner=None, **changes):
        options=dict(container=self.name,fixture_dir=self.root,auth_dir=self.auth,opt_in=True)
        options.update(changes)
        return contract.Harness(**options,runner=runner or subprocess.run)

class HarnessTests(HarnessCase):
    def test_no_default_target_and_production_names_refused_before_io(self):
        for name in (None,'mdb_gluetun','mdb-vpn-contract','other','mdb-vpn-contract-a1b2c3d4;true'):
            with self.subTest(name=name),self.assertRaises(ValueError):
                self.harness(container=name)
        with self.assertRaises(ValueError):self.harness(opt_in=False)
        import contextlib,io
        with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit) as caught:
            contract.parse_args([])
        self.assertNotEqual(caught.exception.code,0)

    def test_only_new_temporary_fixture_and_its_auth_directory_allowed(self):
        for root,auth in ((Path('/opt/magic_dingus_box'),self.auth),
                          (self.root,Path('/etc/gluetun/auth')),
                          (Path('/tmp'),Path('/tmp/services/config/gluetun/auth'))):
            with self.subTest(root=root),self.assertRaises(ValueError):
                self.harness(fixture_dir=root,auth_dir=auth)
        self.root.mkdir()
        (self.root/'owner-data').write_text('retain')
        with self.assertRaises(ValueError):self.harness()
        self.assertEqual((self.root/'owner-data').read_text(),'retain')

    def test_mount_option_delimiter_in_fixture_path_is_rejected(self):
        parent=self.root.parent/'directory,src=foreign';parent.mkdir()
        root=parent/self.name
        with self.assertRaises(ValueError):
            self.harness(fixture_dir=root,auth_dir=root/'services/config/gluetun/auth')

    def test_fixture_is_dummy_only_and_launch_has_no_public_ports_or_production_binds(self):
        h=self.harness();h.prepare()
        self.addCleanup(h.cleanup_files)
        env=(self.root/'dummy.env').read_text()
        self.assertIn('WIREGUARD_PRIVATE_KEY='+contract.DUMMY_PRIVATE_KEY,env)
        self.assertNotIn('PASSWORD=',env)
        self.assertNotIn('UPDATER_PROTONVPN_EMAIL',env)
        self.assertEqual(self.auth.joinpath('recovery.key').stat().st_mode & 0o777,0o600)
        args=h.create_command()
        self.assertNotIn('--publish',args);self.assertNotIn('-p',args)
        self.assertNotIn('--network=host',args)
        self.assertIn('--pull=never',args)
        self.assertIn(contract.PINNED_IMAGE,args)
        mounts=[args[i+1] for i,arg in enumerate(args) if arg=='--mount']
        self.assertEqual(len(mounts),1)
        self.assertIn('src='+str(self.root/'services/config/gluetun'),mounts[0])
        self.assertIn('ip rule add',args[-1]);self.assertIn('blackhole',args[-1])
        self.assertNotIn('nsenter',args[-1])
        self.assertEqual(h.paths.compose_lock,self.root/'compose.lock')
        self.assertTrue(all(s['wgpubkey']==contract.DUMMY_PUBLIC_KEY for s in h.snapshot.provider['servers']))

    def test_existing_container_is_never_reused_or_removed(self):
        calls=[]
        def runner(args,**kwargs):
            calls.append(args)
            return subprocess.CompletedProcess(args,0,'abcdef\n','')
        h=self.harness(runner)
        result=h.execute()
        self.assertFalse(result['ok'])
        self.assertFalse(self.root.exists())
        self.assertFalse(any('rm' in c or 'create' in c for c in calls))

    def test_contract_mismatch_fails_and_cleans_only_owned_container_and_files(self):
        calls=[];label={}
        def runner(args,**kwargs):
            calls.append(args)
            if args[:3]==['docker','container','ls']:out=''
            elif args[:3]==['docker','image','inspect']:out=json.dumps(['qmcgaw/gluetun@'+contract.PINNED_IMAGE.split('@')[1]])
            elif args[:2]==['docker','create']:
                label['owner']=args[args.index('--label')+1].split('=',1)[1];out='a'*64
            elif args[:2]==['docker','inspect']:
                out='a'*64+' '+label['owner']
            else:out=''
            return subprocess.CompletedProcess(args,0,out,'')
        h=self.harness(runner)
        with patch.object(h,'verify_contract',side_effect=RuntimeError('SECRET_KEY must never print')):
            result=h.execute()
        self.assertFalse(result['ok'])
        self.assertEqual(result['cleanup'],'removed')
        self.assertFalse(self.root.exists())
        self.assertEqual([c for c in calls if c[:3]==['docker','rm','--force']],
                         [['docker','rm','--force','a'*64]])
        self.assertNotIn('SECRET',json.dumps(result))
        self.assertNotIn('skip',json.dumps(result).lower())

    def test_create_timeout_still_finds_owned_container_and_cleanup_is_bounded(self):
        calls=[];owner={}
        def runner(args,**kwargs):
            calls.append((args,kwargs))
            if args[:3]==['docker','container','ls']:out=''
            elif args[:3]==['docker','image','inspect']:out=json.dumps(['qmcgaw/gluetun@'+contract.PINNED_IMAGE.split('@')[1]])
            elif args[:2]==['docker','create']:
                owner['id']=args[args.index('--label')+1].split('=',1)[1]
                raise subprocess.TimeoutExpired(args,1,output='SECRET')
            elif args[:2]==['docker','inspect']:out='a'*64+' '+owner['id']
            else:out=''
            return subprocess.CompletedProcess(args,0,out,'')
        result=self.harness(runner).execute()
        self.assertFalse(result['ok']);self.assertEqual(result['cleanup'],'removed')
        self.assertFalse(self.root.exists())
        self.assertTrue(all(0 < kw['timeout'] <= 10 for _,kw in calls))
        self.assertNotIn('SECRET',json.dumps(result))

    def test_cleanup_refuses_foreign_label_and_preserves_evidence_on_removal_failure(self):
        h=self.harness();h.prepare()
        self.addCleanup(h.cleanup_files)
        h.submitted=True
        h.runner=lambda args,**kwargs:subprocess.CompletedProcess(args,0,'foreign-label','')
        self.assertEqual(h.cleanup_container(),'failed')
        self.assertTrue(self.root.exists())

    def test_cleanup_timeout_reports_failure_and_retains_fixture(self):
        h=self.harness();h.prepare();h.submitted=True
        self.addCleanup(h.cleanup_files)
        def runner(args,**kwargs):
            if args[:2]==['docker','inspect']:
                return subprocess.CompletedProcess(args,0,'a'*64+' '+h.owner,'')
            raise subprocess.TimeoutExpired(args,kwargs['timeout'],output='private-error')
        h.runner=runner
        self.assertEqual(h.cleanup_container(),'failed')
        self.assertTrue(self.root.exists())

    def test_failed_cleanup_cannot_be_reported_as_success(self):
        h=self.harness()
        with patch.object(h,'preflight'),patch.object(h,'launch'),patch.object(h,'verify_contract'),patch.object(h,'cleanup_container',return_value='failed'):
            result=h.execute()
        self.assertFalse(result['ok'])
        self.assertEqual(result['cleanup'],'failed')
        self.assertTrue(self.root.exists())
        h.cleanup_files()

    def test_uncached_or_wrong_digest_fails_without_create_or_fixture(self):
        calls=[]
        def runner(args,**kwargs):
            calls.append(args)
            return subprocess.CompletedProcess(args,0,'[]' if 'image' in args else '','')
        result=self.harness(runner).execute()
        self.assertFalse(result['ok']);self.assertFalse(self.root.exists())
        self.assertEqual(result['assertions'][-1],{'name':'exact_image_cached','ok':False})
        self.assertFalse(any('create' in args or 'pull' in args for args in calls))

    def test_exhausted_aggregate_deadline_never_submits_a_command(self):
        import time
        calls=[]
        h=self.harness(lambda *args,**kwargs:calls.append(args))
        h.deadline=time.monotonic()-1
        result=h.execute()
        self.assertFalse(result['ok']);self.assertEqual(calls,[])
        self.assertFalse(self.root.exists())

    def test_replay_uses_real_adapters_and_recovers_only_after_new_tunnel_and_qbit_readback(self):
        result=contract.replay_failure_to_success()
        self.assertEqual(result['states'],['Ready','Recovering port forwarding','Waiting for candidate','Ready'])
        self.assertEqual(result['selected_countries'],['Canada'])
        self.assertEqual(result['preference_writes'],1)
        self.assertTrue(result['desired_unchanged'])
        self.assertTrue(result['stale_not_ready'])



class PinnedTransport:
    """Only process I/O is simulated; execute the full real contract recipe."""
    def __init__(self, defect=None):
        self.defect=defect;self.owner=None;self.tunnel=True;self.index=2;self.status='running'
        self.cid='a'*64;self.calls=[]
        self.selection={'countries':['Canada'],'hostnames':['node-ca-59.protonvpn.net'],
                        'wireguard':{'endpoint_ip':'84.20.16.29'}}

    def __call__(self,args,**kwargs):
        import copy,time
        self.calls.append(args);code=0;stderr=''
        if args[:3]==['docker','container','ls']:out=''
        elif args[:3]==['docker','image','inspect']:
            out=json.dumps(['qmcgaw/gluetun@'+contract.PINNED_IMAGE.split('@')[1]])
        elif args[:2]==['docker','create']:
            self.owner=args[args.index('--label')+1].split('=',1)[1];out=self.cid
        elif args[:2]==['docker','inspect']:
            if 'Labels' in args[3]:out=self.cid+' '+self.owner
            elif 'PortBindings' in args[3]:out=json.dumps({'8000/tcp':[{}]} if self.defect=='published' else {})
            else:out=self.cid+' 42 2026-11-14T00:00:00Z true'
        elif args[:2]==['docker','exec']:
            tail=args[3:]
            if tail[:2]==['ip','route']:
                out='\n'.join('blackhole '+ip+' table 200' for ip in ('84.20.16.29','84.20.16.1','185.156.46.33','89.222.100.65','185.107.44.110','212.92.104.225'))
                if self.defect=='route':out=''
            elif tail[:2]==['ip','rule']:
                out='\n'.join('1: from all to '+ip+' lookup 200' for ip in ('84.20.16.29','84.20.16.1','185.156.46.33','89.222.100.65','185.107.44.110','212.92.104.225'))
            elif tail[0]=='ip':
                out=f'{self.index}: tun0: <POINTOPOINT,UP> mtu 1320\n' if self.tunnel else ''
                code=0 if self.tunnel else 1
            elif tail[0]=='iptables':
                out='-P OUTPUT DROP\n'
                if self.tunnel:
                    out+='-A OUTPUT -d '+self.selection['wireguard']['endpoint_ip']+'/32 -o eth0 -p udp --dport 51820 -j ACCEPT\n'
                if self.defect=='ambiguous':out+=out
            elif tail==['timeout','--help']:out='';stderr='BusyBox v1.37.0 usage'
            elif tail==['timeout','3','wget','--version']:out='GNU Wget 1.25.0'
            elif tail==['timeout','-k','1','1','sleep','30']:out='';code=143
            elif tail[0]=='timeout':out='';code=1 if self.defect!='leak' else 0
            else:raise AssertionError(tail)
        elif args[0]=='nsenter':
            request=json.loads(kwargs['input']);route=request['path'];method=request['method']
            if request['key']=='' and route!='/v1/portforward':
                out=json.dumps({'ok':False,'status':401} if self.defect!='auth' else {'ok':True,'status':200,'body':'{}'})
            else:
                if route=='/v1/portforward':body={'port':0}
                elif route=='/v1/vpn/status':
                    if method=='PUT':
                        self.status=request['payload']['status'];self.tunnel=self.status=='running'
                        if self.tunnel:self.index+=1
                    body={'status':self.status}
                elif route=='/v1/vpn/settings':
                    if method=='PUT':
                        target=request['payload']['provider']['server_selection']
                        body='settings left unchanged' if target==self.selection else 'VPN settings updated'
                        if target!=self.selection:self.index+=1
                        self.selection=copy.deepcopy(target)
                        if self.defect=='identical' and body=='settings left unchanged':body='updated'
                    else:
                        key='changed' if self.defect=='key' and self.index>2 else contract.DUMMY_PRIVATE_KEY
                        body={'wireguard':{'private_key':key},'provider':{'server_selection':self.selection}}
                else:raise AssertionError(route)
                out=json.dumps({'ok':True,'status':200,'body':body if isinstance(body,str) else json.dumps(body)})
        elif args[:2]==['docker','logs']:
            out='INFO [storage] Using protonvpn servers from file which are 1 year more recent' if self.defect!='catalog' else ''
        elif args[:2]==['docker','events']:
            out=str(time.time_ns() if self.defect!='event' else int(time.time()))+' '+self.cid
        elif args[:2] in (['docker','start'],['docker','stop'],['docker','rm']):out=''
        else:raise AssertionError(args)
        return subprocess.CompletedProcess(args,code,out,stderr)


class FullContractTests(HarnessCase):
    def test_complete_recipe_and_each_pinned_mismatch_is_a_failure(self):
        for defect in (None,'published','route','auth','key','ambiguous','identical','leak','catalog','event'):
            with self.subTest(defect=defect):
                transport=PinnedTransport(defect)
                result=self.harness(transport).execute()
                self.assertEqual(result['ok'],defect is None,result)
                self.assertEqual(result['cleanup'],'removed')
                self.assertFalse(self.root.exists())
                if defect is not None:self.assertTrue(any(not row['ok'] for row in result['assertions']),result)

    def test_interrupt_after_create_always_removes_only_owned_container(self):
        transport=PinnedTransport()
        def runner(args,**kwargs):
            if args[:2]==['docker','start']:raise KeyboardInterrupt()
            return transport(args,**kwargs)
        result=self.harness(runner).execute()
        self.assertFalse(result['ok']);self.assertEqual(result['cleanup'],'removed')
        self.assertFalse(self.root.exists())

    def test_sigterm_main_runs_cleanup_and_restores_previous_handler(self):
        import contextlib,io,os,signal
        transport=PinnedTransport()
        def runner(args,**kwargs):
            if args[:2]==['docker','start']:os.kill(os.getpid(),signal.SIGTERM)
            return transport(args,**kwargs)
        original=contract.Harness
        previous=signal.getsignal(signal.SIGTERM)
        def factory(**kwargs):return original(**kwargs,runner=runner)
        with patch.object(contract,'Harness',side_effect=factory),patch.object(contract.sys,'platform','linux'), \
             patch.object(contract.os,'geteuid',return_value=0),contextlib.redirect_stdout(io.StringIO()) as output:
            code=contract.main(['--opt-in-disposable-contract','--container',self.name,
                                '--fixture-dir',str(self.root),'--auth-dir',str(self.auth)])
        self.assertEqual(code,1)
        self.assertEqual(json.loads(output.getvalue())['cleanup'],'removed')
        self.assertFalse(self.root.exists())
        self.assertEqual(signal.getsignal(signal.SIGTERM),previous)


if __name__=='__main__':unittest.main()
