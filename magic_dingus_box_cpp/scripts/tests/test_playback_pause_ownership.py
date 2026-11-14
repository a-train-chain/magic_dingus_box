"""Run the real pause script against stateful Docker and lock boundaries."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'playback_services_pause.sh'
DOCKER = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
p = Path(os.environ['DOCKER_STATE']); states = json.loads(p.read_text()); a = sys.argv[1:]
with open(os.environ['DOCKER_LOG'], 'a') as f: f.write(' '.join(a) + '\\n')
names = [n for n in a if n.startswith('mdb_')]
if a[0] == 'inspect':
    n = names[0]
    if os.environ.get('DOCKER_UNAVAILABLE') == '1': sys.exit(1)
    if os.environ.get('INSPECT_FAIL_AFTER_START') == '1' and n == os.environ.get('INSPECT_FAIL_NAME', n) and 'start ' in Path(os.environ['DOCKER_LOG']).read_text(): sys.exit(1)
    if n not in states: sys.exit(1)
    if '-f' in a:
        print(('true false' if states[n] == 'running' else ('true true' if states[n] == 'paused' else 'false false')) if 'State.Paused' in ' '.join(a) else ('true' if states[n] == 'running' else 'false'))
elif a[0] == 'ps':
    if os.environ.get('DOCKER_UNAVAILABLE') == '1' or os.environ.get('INSPECT_FAIL_AFTER_START') == '1': sys.exit(1)
    print('\\n'.join(states))
elif a[0] == 'stop':
    for n in names: states[n] = 'stopped'
elif a[0] == 'start':
    for n in names: states[n] = 'running'
p.write_text(json.dumps(states))
'''
FLOCK = '''#!/usr/bin/env python3
import fcntl, os, sys
from pathlib import Path
if os.environ.get('EXPECT_PRELOCK_MARKER') == '1':
    if not Path(os.environ['PAUSE_MARKER']).exists(): sys.exit(2)
if os.environ.get('FAIL_LOCK') == '1': sys.exit(1)
fcntl.flock(int(sys.argv[-1]), fcntl.LOCK_EX)
'''
# Instrument the real Python fsync calls used by the shell journal helper.
# This lives only in each temporary fixture; production has no failure flags.
SYNC_PROBE = '''
import os, stat
from pathlib import Path
_real_fsync = os.fsync
_sync_log = os.environ.get('SYNC_LOG')
if _sync_log:
    def fsync(fd):
        path = Path(_sync_log)
        events = path.read_text().splitlines() if path.exists() else []
        kind = 'dir' if stat.S_ISDIR(os.fstat(fd).st_mode) else 'file'
        events.append(kind); path.write_text('\\n'.join(events) + '\\n')
        if len(events) == int(os.environ.get('FAIL_SYNC_AT', '0')):
            raise OSError('injected disk synchronization failure')
        return _real_fsync(fd)
    os.fsync = fsync
'''
INITIAL = {'mdb_radarr': 'running', 'mdb_sonarr': 'stopped', 'mdb_prowlarr': 'running', 'mdb_byparr': 'paused'}

class OwnershipTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        for name, source in [('docker', DOCKER), ('flock', FLOCK)]:
            path = self.root / name; path.write_text(source); path.chmod(0o755)
        self.state = self.root / 'state'; self.state.write_text(json.dumps(INITIAL))
        self.env = dict(os.environ, PATH=str(self.root) + ':' + os.environ['PATH'], DOCKER_STATE=str(self.state),
                        DOCKER_LOG=str(self.root / 'log'), PAUSE_MARKER=str(self.root / 'paused'),
                        PLAYBACK_OWNED_FILE=str(self.root / 'owned'), MDB_COMPOSE_LOCK=str(self.root / 'lock'))
        self.ota = 'ota:' + str(os.getpid())
        (self.root / 'sitecustomize.py').write_text(SYNC_PROBE)
        self.env.update(PYTHONPATH=str(self.root), SYNC_LOG=str(self.root / 'sync-log'))

    def tearDown(self): self.tmp.cleanup()

    def run_action(self, action, owner='kiosk', **env):
        result = subprocess.run(['bash', str(SCRIPT), action, owner], env=dict(self.env, **env), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def assert_paused(self):
        self.assertEqual(json.loads(self.state.read_text()), dict(INITIAL, mdb_radarr='stopped', mdb_prowlarr='stopped'))
        self.assertTrue((self.root / 'paused').exists())

    def test_claim_and_service_intent_both_sync_before_stopping(self):
        for failure in range(1, 5):
            with self.subTest(failure=failure):
                (self.root / 'sync-log').write_text('')
                result = subprocess.run(['bash', str(SCRIPT), 'pause'],
                                        env=dict(self.env, FAIL_SYNC_AT=str(failure)), capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(json.loads(self.state.read_text()), INITIAL)
        self.run_action('pause')
        self.assert_paused()
        self.assertIn('file\ndir\nfile\ndir\n', (self.root / 'sync-log').read_text())

    def test_retirement_sync_failure_keeps_retry_without_replaying_cleared_services(self):
        self.run_action('pause')
        (self.root / 'sync-log').write_text('')
        # Claims publication (2), owned-record preflight (2), empty checkpoint
        # publication (2), then deletion's parent-directory fsync (1).
        result = subprocess.run(['bash', str(SCRIPT), 'unpause'],
                                env=dict(self.env, FAIL_SYNC_AT='7'), capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)
        before = (self.root / 'log').read_text().count('start ')
        self.run_action('unpause')
        self.assertEqual((self.root / 'log').read_text().count('start '), before)
        self.assertFalse((self.root / 'owned').exists())
        self.assertFalse((self.root / 'owned.claims').exists())

    def test_recovery_publication_failure_leaves_original_owned_names_for_retry(self):
        self.run_action('pause')
        (self.root / 'sync-log').write_text('')
        result = subprocess.run(['bash', str(SCRIPT), 'unpause'],
                                env=dict(self.env, FAIL_SYNC_AT='5'), capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('mdb_radarr', (self.root / 'owned').read_text())
        self.run_action('unpause')
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)

    def test_pause_during_docker_outage_makes_no_unowned_changes(self):
        result = subprocess.run(['bash', str(SCRIPT), 'pause'],
                                env=dict(self.env, DOCKER_UNAVAILABLE='1'), capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)
        self.run_action('pause'); self.assert_paused()
        self.run_action('unpause')
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)

    def test_docker_unavailable_at_initial_observation_preserves_retry(self):
        self.run_action('pause')
        result = subprocess.run(['bash', str(SCRIPT), 'unpause'],
                                env=dict(self.env, DOCKER_UNAVAILABLE='1'), capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.root / 'owned').exists())
        self.assertTrue((self.root / 'owned.claims').exists())
        self.run_action('unpause')
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)

    def test_docker_unavailable_at_final_verification_preserves_retry(self):
        self.run_action('pause')
        result = subprocess.run(['bash', str(SCRIPT), 'unpause'],
                                env=dict(self.env, INSPECT_FAIL_AFTER_START='1'), capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('mdb_radarr', (self.root / 'owned').read_text())
        self.assertIn('mdb_prowlarr', (self.root / 'owned').read_text())
        self.run_action('unpause')
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)

    def test_partial_recovery_checkpoints_only_uncertain_names(self):
        self.run_action('pause')
        result = subprocess.run(['bash', str(SCRIPT), 'unpause'],
                                env=dict(self.env, INSPECT_FAIL_AFTER_START='1', INSPECT_FAIL_NAME='mdb_radarr'), capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.root / 'owned').read_text(), 'mdb_radarr\n')
        states = json.loads(self.state.read_text()); states['mdb_prowlarr'] = 'stopped'
        self.state.write_text(json.dumps(states))  # operator stopped the already-restored service
        self.run_action('unpause')
        self.assertEqual(json.loads(self.state.read_text()), dict(INITIAL, mdb_prowlarr='stopped'))

    def test_confirmed_container_removal_retires_only_its_ownership(self):
        self.run_action('pause')
        states = json.loads(self.state.read_text()); del states['mdb_radarr']; self.state.write_text(json.dumps(states))
        self.run_action('unpause')
        self.assertEqual(json.loads(self.state.read_text()), {k: v for k, v in INITIAL.items() if k != 'mdb_radarr'})
        self.assertFalse((self.root / 'owned').exists())

    def test_only_originally_running_containers_resume_even_after_reentry(self):
        for action in ('pause', 'pause', 'unpause', 'unpause'): self.run_action(action)
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)
        self.assertFalse((self.root / 'owned').exists())

    def test_unpause_without_owned_record_does_not_start_operator_services(self):
        self.state.write_text(json.dumps({'mdb_radarr': 'stopped'}))
        self.run_action('unpause')
        self.assertEqual(json.loads(self.state.read_text()), {'mdb_radarr': 'stopped'})

    def test_both_overlap_orders_wait_for_final_owner(self):
        for first, second in [('kiosk', self.ota), (self.ota, 'kiosk')]:
            with self.subTest(first=first):
                self.run_action('pause', first); self.run_action('pause', second)
                self.run_action('unpause', first); self.assert_paused()
                self.run_action('unpause', second)
                self.assertEqual(json.loads(self.state.read_text()), INITIAL)

    def test_kiosk_startup_recovery_preserves_a_live_ota_lease(self):
        self.run_action('pause', self.ota)
        self.run_action('unpause')
        self.assert_paused()
        self.run_action('unpause', self.ota)
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)

    def test_killed_ota_lease_is_recovered_at_kiosk_startup(self):
        worker = subprocess.Popen(['python3', '-c', 'import sys; sys.stdin.read()'], stdin=subprocess.PIPE)
        try:
            self.run_action('pause', 'ota:' + str(worker.pid)); self.assert_paused()
        finally:
            worker.kill(); worker.wait(); worker.stdin.close()
        self.run_action('unpause')
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)

    def test_updater_helpers_pass_a_matching_process_scoped_lease(self):
        shim = self.root / 'pause_shim'
        shim.write_text('#!/bin/bash\nprintf "%s %s\\n" "$1" "${2:-}" >> "$LEASE_LOG"\n')
        shim.chmod(0o755)
        log = self.root / 'lease_log'
        updater = SCRIPT.parent / 'update.sh'
        env = dict(self.env, MAGIC_BASE_PATH=str(self.root), MAGIC_BACKUP_DIR=str(self.root / 'backup'),
                   MAGIC_TEMP_DIR=str(self.root / 'tmp'), MAGIC_SKIP_SYSTEMCTL='true',
                   MAGIC_PLAYBACK_PAUSE_SCRIPT=str(shim), LEASE_LOG=str(log))
        result = subprocess.run(['bash', '-c', 'source "$1"; pause_services_for_build; resume_build_paused_services', '_', str(updater)],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = log.read_text().splitlines()
        self.assertEqual(len(calls), 2)
        self.assertRegex(calls[0], r'^pause ota:[0-9]+$')
        self.assertEqual(calls[1], 'unpause ' + calls[0].split()[1])

    def test_lock_failure_preserves_existing_records_and_later_release_recovers(self):
        self.run_action('pause')
        before = {name: (self.root / name).read_bytes() for name in ('owned', 'owned.claims')}
        for action in ('pause', 'unpause'):
            result = subprocess.run(['bash', str(SCRIPT), action], env=dict(self.env, FAIL_LOCK='1'), capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assert_paused()
            self.assertEqual({name: (self.root / name).read_bytes() for name in before}, before)
        self.run_action('unpause')
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)

    def test_unusable_lock_fails_closed_and_later_recovery_invents_no_ownership(self):
        result = subprocess.run(['bash', str(SCRIPT), 'pause'],
                                env=dict(self.env, MDB_COMPOSE_LOCK=str(self.root / 'absent' / 'lock')), capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)
        self.assertFalse((self.root / 'owned').exists())
        self.assertFalse((self.root / 'owned.claims').exists())
        self.run_action('unpause')
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)
        self.assertFalse((self.root / 'paused').exists())

    def test_corrupt_owner_record_cannot_authorize_a_resume(self):
        self.run_action('pause')
        (self.root / 'owned.claims').write_text('unrecognized-owner\n')
        result = subprocess.run(['bash', str(SCRIPT), 'unpause'], env=self.env, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assert_paused()

    def test_pause_marker_precedes_the_lock_and_lock_failure_changes_no_services(self):
        self.run_action('pause', EXPECT_PRELOCK_MARKER='1')
        self.run_action('unpause')
        result = subprocess.run(['bash', str(SCRIPT), 'pause'], env=dict(self.env, FAIL_LOCK='1'), capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(self.state.read_text()), INITIAL)
        self.run_action('unpause')
        self.assertFalse((self.root / 'paused').exists())
        self.assertFalse((self.root / 'owned').exists())

if __name__ == '__main__': unittest.main()
