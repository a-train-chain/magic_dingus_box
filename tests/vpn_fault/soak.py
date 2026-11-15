"""Finite read-only post-deployment validation. Never starts/stops or syncs."""
import argparse,json,pathlib,subprocess,sys,time
sys.path.insert(0,'/opt/magic_dingus_box/magic_dingus_box_cpp/scripts')
from vpn_recovery.common import Paths
from vpn_recovery.cli import read_status
from vpn_recovery.runtime import SystemAdapters
p=argparse.ArgumentParser();p.add_argument('--seconds',type=int,default=1800);p.add_argument('--kiosk-pid',type=int,required=True);p.add_argument('--kiosk-restarts',type=int,default=0)
a=p.parse_args()
if not 1800<=a.seconds<=3600:raise SystemExit('duration must cover at least 30 minutes')
paths=Paths();ad=SystemAdapters(paths);started=time.monotonic();finish=started+a.seconds;next_probe=0;count=0;generations=set();ports=set()
def emit(value): print(json.dumps(value,separators=(',',':')),flush=True)
UNIT='gluetun-cascade-restart.service'
def unit_prop(name):
 return subprocess.run(['systemctl','show',UNIT,'-p',name,'--value'],capture_output=True,text=True,timeout=3,check=True).stdout.strip()
def rounds():
 try:return json.loads((paths.state/'controller.json').read_text()).get('round_attempts')
 except Exception:return 'unreadable'
def rss_kib(pid):
 for line in pathlib.Path('/proc/%s/status'%pid).read_text().splitlines():
  if line.startswith('VmRSS:'):return int(line.split()[1])
gluetun_start=subprocess.run(['docker','inspect','-f','{{.State.StartedAt}}','mdb_gluetun'],capture_output=True,text=True,timeout=5,check=True).stdout.strip()
def vpn_restarts():
 out=subprocess.run(['docker','logs','--since',soak_since,'mdb_gluetun'],capture_output=True,text=True,timeout=10).stdout+''
 return out.count('restarting VPN')
soak_since=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
cpu0=int(unit_prop('CPUUsageNSec'));watcher_pid=unit_prop('MainPID');rss=[]
def kiosk():
 r=subprocess.run(['systemctl','show','magic-dingus-box-cpp.service','-p','MainPID','-p','NRestarts','-p','ActiveState'],capture_output=True,text=True,timeout=3,check=True)
 return dict(line.split('=',1) for line in r.stdout.splitlines())
try:
 while True:
  now=time.monotonic();s=read_status(paths);count+=1
  row={'sample':count,'utc':time.time(),'elapsed_s':round(now-started,2)}
  for key in ('checked_at','state','ready','traffic_verified','qbit_synced','forwarded_port','dependents_ok','last_success','last_recovery_reason'):
   row[key]=s.get(key)
  generation=s.get('generation') or {};effective=s.get('effective') or {}
  row['generation']=generation;row['effective']={k:effective.get(k) for k in ('country','hostname','endpoint_ip')}
  if generation:generations.add(json.dumps(generation,sort_keys=True))
  if isinstance(s.get('forwarded_port'),int):ports.add(s['forwarded_port'])
  healthy=all(s.get(k) is True for k in ('ready','traffic_verified','qbit_synced','dependents_ok')) and isinstance(s.get('forwarded_port'),int) and 0<s['forwarded_port']<65536
  if now>=next_probe or now>=finish:
   k=kiosk();row['kiosk']=k
   independent=ad.ports.observe(deadline=time.monotonic()+8)
   row['independent_port']={key:independent.get(key) for key in ('status','forwarded_port','qbit_port','generation','error')}
   observed=independent.get('generation') or {}
   row['independent_traffic']=bool(observed) and ad._traffic(observed,time.monotonic()+3)
   healthy=healthy and k.get('MainPID')==str(a.kiosk_pid) and k.get('ActiveState')=='active' and k.get('NRestarts')==str(a.kiosk_restarts) and independent.get('status')=='synced' and independent.get('generation')==generation and row['independent_traffic']
   next_probe=time.monotonic()+60
  row['round_attempts']=rounds()
  try:rss.append(rss_kib(watcher_pid));row['watcher_rss_kib']=rss[-1]
  except Exception:row['watcher_rss_kib']=None
  row['ok']=bool(healthy);emit(row)
  if not healthy:raise RuntimeError('verification failed; end soak and investigate')
  if now>=finish:break
  time.sleep(min(10,max(0,finish-time.monotonic())))
 elapsed=time.monotonic()-started;cpu1=int(unit_prop('CPUUsageNSec'))
 emit({'summary':True,'ok':True,'samples':count,'elapsed_s':round(elapsed,2),'generation_count':len(generations),'ports':sorted(ports),'finished_utc':time.time(),
       'watcher_pid_unchanged':unit_prop('MainPID')==watcher_pid,'watcher_cpu_pct_one_core':round((cpu1-cpu0)/1e9/elapsed*100,2),
       'watcher_rss_kib_min_max':[min(rss),max(rss)] if rss else None,'gluetun_vpn_restarts':vpn_restarts(),'round_attempts_final':rounds()})
except Exception as exc:
 emit({'summary':True,'ok':False,'samples':count,'elapsed_s':round(time.monotonic()-started,2),'error_type':type(exc).__name__})
 raise SystemExit(1)
