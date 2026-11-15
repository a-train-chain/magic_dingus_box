from datetime import datetime, timezone
import json
from pathlib import Path
import time
import pytest
try:
    from vpn_recovery_support import snapshot, native, Maintenance
except ImportError:
    snapshot = native = Maintenance = None

def test_native_status_absent_stale_and_future_are_unknown(tmp_path):
    assert snapshot is not None, 'sanitized native status adapter absent'
    path=tmp_path/'status.json'
    assert snapshot(path)['fresh'] is False
    for age in (46,-1):
        path.write_text(json.dumps({'checked_at':datetime.fromtimestamp(1000-age,timezone.utc).isoformat(), 'ready':True,'traffic_verified':True,'qbit_synced':True,'forwarded_port':1234}))
        view=snapshot(path,now=1000)
        assert view['fresh'] is False and view['ready'] is False
        assert view['forwarded_port'] is None and view['qbit_synced'] is False

def test_native_status_reports_only_allowlisted_fresh_fields(tmp_path):
    assert snapshot is not None, 'sanitized native status adapter absent'
    path=tmp_path/'status.json'
    path.write_text(json.dumps({'checked_at':datetime.fromtimestamp(1000,timezone.utc).isoformat(),'ready':True,'state':'Ready','traffic_verified':True,'forwarded_port':1234,'qbit_synced':True,'effective':{'hostname':'ca-01','country':'Canada','private_key':'fixture-secret'},'catalog':{'age_s':4,'secret':'fixture-secret'},'private_key':'fixture-secret','last_success':'now'}))
    view=snapshot(path,now=1001)
    assert view['fresh'] and view['ready'] and view['country']=='Canada' and view['hostname']=='ca-01'
    assert view['forwarded_port']==1234 and view['catalog_age_s']==4
    assert 'fixture-secret' not in json.dumps(view)

@pytest.mark.parametrize('env', [{'VPN_SERVICE_PROVIDER':'custom','VPN_PORT_FORWARDING':'off'}, {'VPN_SERVICE_PROVIDER':'protonvpn','VPN_PORT_FORWARDING':'off'}])
def test_unsupported_config_does_not_claim_native_failure(env):
    assert native is not None, 'native eligibility adapter absent'
    assert native(env) is False

def test_config_mutations_fail_closed_if_shared_lock_missing(tmp_path):
    assert Maintenance is not None, 'maintenance adapter absent'
    ctl=tmp_path/'vpn_recoveryctl.py';ctl.touch()
    changed=[]
    with pytest.raises(RuntimeError,match='compose lock unavailable'):
        with Maintenance(ctl,tmp_path,compose_lock=tmp_path/'missing'):
            changed.append(True)
    assert changed==[]

def test_detached_handoff_releases_intent_while_retaining_compose_lock(tmp_path,monkeypatch):
    import fcntl
    ctl=tmp_path/'vpn_recoveryctl.py';ctl.touch()
    lock=tmp_path/'compose.lock';lock.touch()
    calls=[]
    monkeypatch.setattr(Maintenance,'command',lambda self,*args:calls.append(args))
    with Maintenance(ctl,tmp_path,compose_lock=lock) as owner:
        owner.release()
        other=lock.open('rb')
        try:
            with pytest.raises(BlockingIOError):
                fcntl.flock(other,fcntl.LOCK_EX|fcntl.LOCK_NB)
        finally:other.close()
        assert [c[:2] for c in calls]==[('inhibit','begin'),('inhibit','end')]
    assert [c[:2] for c in calls]==[('inhibit','begin'),('inhibit','end')]

@pytest.mark.parametrize('age', [0, 100])
@pytest.mark.parametrize('value', [10**400, -10**400, float('inf'), float('nan'), True, '12'], ids=['huge','negative-huge','infinite','nan','boolean','string'])
def test_corrupt_numeric_status_is_unknown_even_when_stale(tmp_path, age, value):
    path=tmp_path/'status.json'
    path.write_text(json.dumps({'checked_at':datetime.fromtimestamp(1000-age,timezone.utc).isoformat(),
                                'catalog':{'age_s':value}}))
    assert snapshot(path,now=1000)['catalog_age_s'] is None
