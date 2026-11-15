"""Execute the real setup entrypoint; only host/Docker boundaries are fakes."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

SCRIPTS=Path(__file__).resolve().parents[1]

class SetupTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.s=self.root/'magic_dingus_box_cpp/scripts';self.s.mkdir(parents=True)
        for name in ('setup_services.sh','vpn_recoveryctl.py'):
            shutil.copyfile(SCRIPTS/name,self.s/name)
        shutil.copytree(SCRIPTS/'vpn_recovery',self.s/'vpn_recovery')
        services=self.root/'services';services.mkdir()
        (services/'.env').write_text('VPN_COUNTRIES=Canada\nVPN_PORT_FORWARDING=on\n')
        (services/'docker-compose.yml').write_text('services: {}\n')
        self.bin=self.root/'bin';self.bin.mkdir()
        self.env=dict(os.environ,PATH=str(self.bin)+':'+os.environ['PATH'],SERVICES_DIR=str(services),
                      STORAGE_ROOT=str(self.root/'storage'),MDB_SETUP_LOCK=str(self.root/'setup.lock'),
                      MDB_COMPOSE_LOCK=str(self.root/'compose.lock'),MDB_VPN_STATE=str(self.root/'state'),
                      MDB_VPN_RUNTIME=str(self.root/'run'),TEST_ROOT=str(self.root))
        for name in ('usermod','chown','timedatectl'): self.script(name,'#!/bin/sh\nexit 0\n')
        self.script('systemctl','#!/bin/sh\necho active\n')
        self.script('sudo','#!/bin/sh\n[ "$1" = chown ] && exit 0\nexec "$@"\n')
        self.script('flock',f'''#!{sys.executable}
import fcntl,sys
fd=int(sys.argv[-1]);fcntl.flock(fd,fcntl.LOCK_UN if '-u' in sys.argv else fcntl.LOCK_EX|fcntl.LOCK_NB)
''')
        self.script('docker',f'''#!{sys.executable}
import json,os,sys,time
from pathlib import Path
r=Path(os.environ['TEST_ROOT']);a=sys.argv[1:]
with (r/'calls').open('a') as f:f.write(json.dumps(a)+'\\n')
def health():
    (r/'waiting').touch()
    until=time.monotonic()+10
    while not (r/'release').exists() and time.monotonic()<until:time.sleep(.02)
if a[:2]==['compose','up']:
    if '--no-deps' not in a:health()
    if a[-1]!='gluetun':sys.exit(17)  # stop before unrelated API provisioning
elif a[:2]==['compose','down']:(r/'stopped').touch()
elif a[:1]==['inspect']:
    if 'Health.Status' in ' '.join(a):health();print('healthy')
    else:print('a'*64+' 42 fixture-start true'+(' false running' if 'OOMKilled' in ' '.join(a) else ''))
elif 'tun0' in ' '.join(a):print('2: tun0: <UP>')
''')
    def script(self,name,text):
        p=self.bin/name;p.write_text(text);p.chmod(0o755)
    def start(self):
        self.p=subprocess.Popen(['bash',str(self.s/'setup_services.sh'),'--skip-host-networking'],
                                env=self.env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
        self.addCleanup(self.cleanup_process)
        until=time.monotonic()+8
        while time.monotonic()<until and self.p.poll() is None:
            if (self.root/'waiting').exists():return
            time.sleep(.02)
        self.fail('setup never reached health observation; exit='+str(self.p.poll()))
    def cleanup_process(self):
        import signal
        if self.p.poll() is None:
            os.killpg(self.p.pid,signal.SIGTERM)
            try:self.p.wait(timeout=3)
            except subprocess.TimeoutExpired:os.killpg(self.p.pid,signal.SIGKILL);self.p.wait()
    def calls(self):return [json.loads(l) for l in (self.root/'calls').read_text().splitlines()]
    def test_actual_setup_delayed_health_allows_stop_and_never_starts_dependents(self):
        self.start()
        with (self.root/'compose.lock').open('rb') as fd:
            # Regression: aggregate setup Compose held this lock during health.
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
            fcntl.flock(fd,fcntl.LOCK_UN)
        stop=subprocess.run([sys.executable,str(self.s/'vpn_recoveryctl.py'),'--base',str(self.root),
                             'lifecycle','stop'],env=self.env,timeout=5,capture_output=True)
        self.assertEqual(stop.returncode,0,stop.stderr)
        (self.root/'release').touch();self.assertEqual(self.p.wait(timeout=5),3)
        ups=[a for a in self.calls() if a[:2]==['compose','up']]
        self.assertEqual(ups,[['compose','up','-d','--no-deps','--remove-orphans','gluetun']])
        self.assertTrue((self.root/'stopped').exists())
        self.assertFalse(json.loads((self.root/'state/intent.json').read_text())['enabled'])
    def test_actual_setup_starts_all_provisioning_dependents_without_implicit_wait(self):
        self.start();(self.root/'release').touch()
        self.assertEqual(self.p.wait(timeout=5),17)
        ups=[a for a in self.calls() if a[:2]==['compose','up']]
        self.assertEqual(ups[-1],['compose','up','-d','--no-deps','radarr','sonarr','prowlarr','qbittorrent','byparr'])

if __name__=='__main__':unittest.main()
