"""Real temporary storage and recorded public data; network never required."""
import copy
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vpn_recovery.common import Paths
from vpn_recovery.common import bounded_lock
try:
    from vpn_recovery.catalog import (CatalogStore, FetchResponse, validate_catalog,
                                      eligible_candidates, select_candidate, bounded_history,
                                      fetch_public_catalog, CATALOG_URL)
except ImportError:
    CatalogStore = None

FIXTURE = Path(__file__).parent/'fixtures/vpn_recovery/proton-v4-sample.json'
NOW = 1794700000


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(CatalogStore, 'catalog implementation missing')
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.paths = Paths(base=root, state=root/'state', runtime=root/'run', compose_lock=root/'compose.lock')
        self.paths.compose_lock.touch()
        self.file = self.paths.services/'config/gluetun/servers.json'
        self.file.parent.mkdir(parents=True)
        self.provider = json.loads(FIXTURE.read_text())
        self.old = copy.deepcopy(self.provider)
        self.old['timestamp'] -= 100
        self.write({'version': 4, 'protonvpn': self.old, 'other': {'untouched': [1, 2]}})
        self.paths.services.joinpath('.env').write_text('VPN_COUNTRIES=Canada\n')
        self.calls = []
        self.response = lambda: FetchResponse(200, json.dumps(self.provider).encode(), {'ETag': '"sample"', 'Last-Modified': 'Sat, 14 Nov 2026 00:00:00 GMT'})
        def fetcher(url, headers, timeout_s, max_bytes):
            self.calls.append((url, headers, timeout_s, max_bytes))
            return self.response()
        self.store = CatalogStore(self.paths, fetcher, jitter=lambda: 123, clock=lambda: NOW)

    def write(self, value):
        self.file.write_text(json.dumps(value))
        self.file.chmod(0o640)

    def test_recorded_fixture_and_candidates(self):
        cat = validate_catalog(self.provider, NOW)
        candidates = eligible_candidates(cat, ['Canada'])
        self.assertEqual([c.hostname for c in candidates], ['node-ca-59.protonvpn.net', 'node-ca-60.protonvpn.net'])
        self.assertEqual(candidates[0].endpoint_ip, '84.20.16.29')
        self.assertEqual(candidates[0].public_key, 'uUM9J8bziO0yZfsdYoruEfP3m6YuSu+Mek9W/J8mT1Y=')
        self.assertEqual(eligible_candidates(cat, ['Nowhere']), [])

    def test_identity_survives_gluetun_server_sort_without_changing_selection_order(self):
        before = copy.deepcopy(self.provider)
        original = validate_catalog(self.provider, NOW)
        # v3.41.3 flush.go sorts Canada, Netherlands, United States.
        reordered = copy.deepcopy(self.provider)
        reordered['servers'] = [reordered['servers'][i] for i in (0, 1, 4, 5, 2, 3)]
        self.assertEqual(validate_catalog(reordered, NOW).identity, original.identity)
        self.assertEqual(self.provider, before)
        self.assertEqual(original.provider, before)
        self.assertEqual([c.country for c in eligible_candidates(original, [])],
                         ['Canada', 'Canada', 'United States', 'United States',
                          'United States', 'United States', 'Netherlands', 'Netherlands'])

    def test_identity_binds_all_provider_server_fields_and_ip_order(self):
        original = validate_catalog(self.provider, NOW).identity
        changes = [lambda p: p.update(timestamp=p['timestamp']-1),
                   lambda p: p.update(extra='metadata'),
                   lambda p: p['servers'][0].update(city='Other city'),
                   lambda p: p['servers'][0].update(port_forward=False),
                   lambda p: p['servers'][0].update(wgpubkey=p['servers'][1]['wgpubkey']),
                   lambda p: p['servers'][0].update(ips=['84.20.16.2']),
                   lambda p: p['servers'][2]['ips'].reverse(),
                   lambda p: p['servers'].append(copy.deepcopy(p['servers'][0])),
                   lambda p: p['servers'].pop()]
        for change in changes:
            with self.subTest(change=change):
                changed = copy.deepcopy(self.provider)
                change(changed)
                self.assertNotEqual(validate_catalog(changed, NOW).identity, original)

    def test_schema_key_ip_and_timestamp_validation(self):
        mutations = [lambda p: p.update(version=3), lambda p: p.update(version=True),
                     lambda p: p.update(timestamp=NOW+1), lambda p: p.update(timestamp=-1),
                     lambda p: p['servers'][0].update(wgpubkey='bad'),
                     lambda p: p['servers'][0].update(ips=['127.0.0.1']),
                     lambda p: p['servers'][0].update(ips=['bad']),
                     lambda p: p['servers'][0].update(hostname='bad\nname'),
                     lambda p: p['servers'][0].update(port_forward='true'),
                     lambda p: p.update(servers=[])]
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                value = copy.deepcopy(self.provider)
                mutate(value)
                with self.assertRaises(ValueError):
                    validate_catalog(value, NOW)
        self.provider['timestamp'] = NOW
        self.assertEqual(validate_catalog(self.provider, NOW+.9).timestamp, NOW)

    def test_future_publication_preserves_good_data_then_recovers_with_clock(self):
        before=self.file.read_bytes()
        self.provider['timestamp']=NOW+600
        self.assertEqual(self.store.refresh(NOW,force=True)['status'],'failed')
        self.assertEqual(self.file.read_bytes(),before)
        self.assertFalse(self.file.with_name('servers.json.known-good').exists())
        self.store.clock=lambda: NOW+600
        self.assertEqual(self.store.refresh(NOW+600)['status'],'updated')
        self.assertEqual(json.loads(self.file.read_text())['protonvpn']['timestamp'],NOW+600)
        self.assertEqual(self.file.with_name('servers.json.known-good').read_bytes(),before)

    def test_clock_rollback_during_fetch_cannot_publish_future_provider(self):
        before=self.file.read_bytes()
        self.provider['timestamp']=NOW
        self.store.clock=lambda: NOW-1
        self.assertEqual(self.store.refresh(NOW,force=True)['status'],'failed')
        self.assertEqual(self.file.read_bytes(),before)

    def test_duplicates_are_deduplicated_and_other_protocols_excluded(self):
        self.provider['servers'].append(copy.deepcopy(self.provider['servers'][0]))
        self.provider['servers'][1]['port_forward'] = False
        extra = copy.deepcopy(self.provider['servers'][0])
        extra.update(vpn='openvpn')
        extra.pop('wgpubkey')
        self.provider['servers'].append(extra)
        cat = validate_catalog(self.provider, NOW)
        self.assertEqual(len(eligible_candidates(cat, ['Canada'])), 1)

    def test_published_country_with_comma_is_valid(self):
        self.provider['servers'][0]['country'] = 'Palestine, State of'
        cat = validate_catalog(self.provider, NOW)
        self.assertEqual(len(eligible_candidates(cat, ['Palestine, State of'])), 1)

    def test_merge_preserves_other_providers_mode_owner_and_backup(self):
        before = self.file.read_bytes()
        owner = (self.file.stat().st_uid, self.file.stat().st_gid)
        result = self.store.refresh(NOW)
        self.assertEqual(result['status'], 'updated')
        self.assertTrue(result['requires_load'])
        self.assertEqual(result['downloaded_identity'], self.store.load().identity)
        self.assertEqual(json.loads(self.file.read_text())['other'], {'untouched': [1, 2]})
        self.assertEqual(self.file.with_name('servers.json.known-good').read_bytes(), before)
        self.assertEqual(stat.S_IMODE(self.file.stat().st_mode), 0o640)
        self.assertEqual((self.file.stat().st_uid, self.file.stat().st_gid), owner)
        self.assertEqual(result['next_due'], NOW+86400+123)
        self.assertEqual(self.calls[0][2:], (15, 8*1024*1024))
        self.assertEqual(self.store.refresh(NOW+1)['status'], 'not_due')
        self.assertEqual(len(self.calls), 1)

    def test_conditional_304_preserves_file_and_retains_identity(self):
        self.store.refresh(NOW)
        before = self.file.read_bytes()
        self.response = lambda: FetchResponse(304, b'', {})
        result = self.store.refresh(NOW+90000)
        self.assertEqual(result['status'], 'not_modified')
        self.assertEqual(self.file.read_bytes(), before)
        self.assertEqual(self.calls[-1][1], {'If-None-Match': '"sample"', 'If-Modified-Since': 'Sat, 14 Nov 2026 00:00:00 GMT'})

    def test_bad_fetches_leave_file_intact(self):
        cases = [lambda: FetchResponse(200, b'{' , {}),
                 lambda: FetchResponse(200, b' '*(8*1024*1024+1), {}),
                 lambda: FetchResponse(503, b'', {}),
                 lambda: (_ for _ in ()).throw(TimeoutError())]
        for response in cases:
            self.response = response
            before = self.file.read_bytes()
            self.assertEqual(self.store.refresh(NOW, force=True)['status'], 'failed')
            self.assertEqual(self.file.read_bytes(), before)

    def test_older_or_empty_preferred_pool_rejected(self):
        for stamp, countries in [(self.old['timestamp']-1, 'Canada'), (self.provider['timestamp'], 'Nowhere')]:
            self.provider['timestamp'] = stamp
            self.paths.services.joinpath('.env').write_text('VPN_COUNTRIES='+countries+'\n')
            before = self.file.read_bytes()
            self.assertEqual(self.store.refresh(NOW, force=True)['status'], 'failed')
            self.assertEqual(self.file.read_bytes(), before)

    def test_stale_writer_cannot_overwrite_newer_catalog(self):
        newer = copy.deepcopy(self.provider)
        newer['timestamp'] += 100
        def response():
            self.write({'protonvpn': newer, 'concurrent': True})
            return FetchResponse(200, json.dumps(self.provider).encode(), {})
        self.response = response
        self.assertEqual(self.store.refresh(NOW)['status'], 'failed')
        self.assertEqual(json.loads(self.file.read_text())['protonvpn']['timestamp'], newer['timestamp'])
        self.assertTrue(json.loads(self.file.read_text())['concurrent'])

    def test_partial_replace_failure_retains_good_catalog(self):
        before = self.file.read_bytes()
        replace = os.replace
        def fail_final(source, target):
            if Path(target) == self.file:
                raise OSError('disk failure')
            return replace(source, target)
        with patch('vpn_recovery.catalog.os.replace', side_effect=fail_final):
            result = self.store.refresh(NOW)
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(self.file.read_bytes(), before)
        self.assertEqual(self.file.with_name('servers.json.known-good').read_bytes(), before)

    def test_unavailable_lock_defers_without_mutation(self):
        self.paths.compose_lock.unlink()
        before = self.file.read_bytes()
        self.assertEqual(self.store.refresh(NOW)['status'], 'deferred')
        self.assertEqual(self.file.read_bytes(), before)

    def test_busy_lock_defers_without_mutation(self):
        before = self.file.read_bytes()
        with self.paths.compose_lock.open('rb') as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertEqual(self.store.refresh(NOW)['status'], 'deferred')
        self.assertEqual(self.file.read_bytes(), before)

    def test_concurrent_other_provider_changes_survive_merge(self):
        def response():
            self.write({'protonvpn': self.old, 'other': {'concurrent': 'retained'}})
            return FetchResponse(200, json.dumps(self.provider).encode(), {})
        self.response = response
        self.assertEqual(self.store.refresh(NOW)['status'], 'updated')
        self.assertEqual(json.loads(self.file.read_text())['other'], {'concurrent': 'retained'})

    def test_corrupt_schedule_cannot_strand_refresh(self):
        self.paths.state.mkdir()
        (self.paths.state/'catalog.json').write_text('{"next_due": 999999999999999999}')
        self.assertEqual(self.store.refresh(NOW)['status'], 'updated')

    def test_oversized_schedule_timestamps_do_not_strand_refresh(self):
        self.paths.state.mkdir()
        for field in ('checked_at', 'next_due'):
            for value in (10**400, -(10**400)):
                with self.subTest(field=field, value=value):
                    state = {'version': 1, 'checked_at': NOW,
                             'next_due': NOW+100, 'downloaded_identity': 'a'*64}
                    state[field] = value
                    (self.paths.state/'catalog.json').write_text(json.dumps(state))
                    self.assertIn(self.store.refresh(NOW)['status'], ('updated', 'not_modified'))

    def test_oversized_history_timestamps_are_discarded(self):
        valid = {'hostname': 'node-ca-59.protonvpn.net',
                 'endpoint_ip': '84.20.16.29', 'failed_at': NOW-10}
        bad = [dict(valid, failed_at=value) for value in (10**400, -(10**400))]
        self.assertEqual(bounded_history([valid, *bad], NOW), [valid])

    def test_invalid_json_clock_values_are_rejected_safely(self):
        before = self.file.read_bytes()
        for value in (None, True, False, '0', {}, [], 10**400, -(10**400)):
            with self.subTest(value=value):
                self.assertEqual(self.store.refresh(value), {'status': 'failed', 'reason': 'invalid_clock'})
                self.assertEqual(bounded_history([], value), [])
                with self.assertRaises(ValueError):
                    validate_catalog(self.provider, value)
        self.assertEqual(self.calls, [])
        self.assertEqual(self.file.read_bytes(), before)

    def test_nested_corrupt_state_and_catalog_fail_safely(self):
        self.paths.state.mkdir()
        (self.paths.state/'catalog.json').write_text('['*2000+'0'+']'*2000)
        self.assertEqual(self.store.refresh(NOW)['status'], 'updated')
        self.response = lambda: FetchResponse(200, ('['*2000+'0'+']'*2000).encode(), {})
        self.assertEqual(self.store.refresh(NOW, force=True)['status'], 'failed')

    def test_not_due_state_must_match_current_download(self):
        self.store.refresh(NOW)
        newer = copy.deepcopy(self.provider)
        newer['timestamp'] += 100
        self.write({'protonvpn': newer})
        self.assertEqual(self.store.refresh(NOW+1)['status'], 'failed')

    def test_preferred_pool_revalidated_after_fetch_under_lock(self):
        @contextmanager
        def change_policy(path, timeout):
            with bounded_lock(path, timeout):
                self.paths.services.joinpath('.env').write_text('VPN_COUNTRIES=Nowhere\n')
                yield
        before = self.file.read_bytes()
        with patch('vpn_recovery.catalog.bounded_lock', change_policy):
            self.assertEqual(self.store.refresh(NOW)['status'], 'failed')
        self.assertEqual(self.file.read_bytes(), before)

    def test_selection_two_distinct_preferred_before_fallback(self):
        candidates = eligible_candidates(validate_catalog(self.provider, NOW), ['Canada', 'United States'])
        history = {'failures': [], 'round_attempts': []}
        first = select_candidate(candidates, history, ['Canada'], ['United States'], NOW)
        self.assertEqual(first.hostname, 'node-ca-59.protonvpn.net')
        history['failures'].append({'hostname': first.hostname, 'endpoint_ip': first.endpoint_ip, 'failed_at': NOW})
        history['round_attempts'].append(first.hostname)
        second = select_candidate(candidates, history, ['Canada'], ['United States'], NOW)
        self.assertEqual(second.hostname, 'node-ca-60.protonvpn.net')
        history['failures'].append({'hostname': second.hostname, 'endpoint_ip': second.endpoint_ip, 'failed_at': NOW})
        history['round_attempts'].append(second.hostname)
        third = select_candidate(candidates, history, ['Canada'], ['United States'], NOW)
        self.assertEqual(third.country, 'United States')

    def _add(self, hostname, country, ip):
        extra = copy.deepcopy(next(s for s in self.provider['servers'] if s['country'] == country))
        extra.update(hostname=hostname, ips=[ip])
        self.provider['servers'].append(extra)

    def test_next_candidate_avoids_the_failed_servers_network(self):
        # Catalog order puts same-/24 neighbours together (ca-59 and ca-60 are
        # both 84.20.16.x): a site-wide outage used to waste the second
        # preferred attempt on the same block.
        self._add('node-ca-24.protonvpn.net', 'Canada', '176.113.74.82')
        candidates = eligible_candidates(validate_catalog(self.provider, NOW), ['Canada', 'United States'])
        history = {'failures': [{'hostname': 'node-ca-59.protonvpn.net', 'endpoint_ip': '84.20.16.29', 'failed_at': NOW}],
                   'round_attempts': ['node-ca-59.protonvpn.net']}
        self.assertEqual(select_candidate(candidates, history, ['Canada'], ['United States'], NOW).hostname,
                         'node-ca-24.protonvpn.net')
        # An attempt this round counts too, even without a recorded failure.
        history = {'failures': [], 'round_attempts': ['node-ca-59.protonvpn.net']}
        self.assertEqual(select_candidate(candidates, history, ['Canada'], ['United States'], NOW).hostname,
                         'node-ca-24.protonvpn.net')

    def test_same_network_is_still_used_rather_than_leaving_the_country(self):
        # Diversity only reorders within a pool: it never skips to the
        # fallback country while a preferred server remains.
        candidates = eligible_candidates(validate_catalog(self.provider, NOW), ['Canada', 'United States'])
        history = {'failures': [{'hostname': 'node-ca-59.protonvpn.net', 'endpoint_ip': '84.20.16.29', 'failed_at': NOW}],
                   'round_attempts': ['node-ca-59.protonvpn.net']}
        self.assertEqual(select_candidate(candidates, history, ['Canada'], ['United States'], NOW).hostname,
                         'node-ca-60.protonvpn.net')

    def test_fallback_pool_also_avoids_recently_failed_networks(self):
        self._add('node-us-391.protonvpn.net', 'United States', '89.222.100.66')
        self._add('node-us-69.protonvpn.net', 'United States', '89.187.170.135')
        candidates = [c for c in eligible_candidates(validate_catalog(self.provider, NOW), ['Canada', 'United States'])
                      if ':' not in c.endpoint_ip]
        history = {'failures': [{'hostname': 'node-us-119.protonvpn.net', 'endpoint_ip': '185.156.46.33', 'failed_at': NOW-60},
                                {'hostname': 'node-us-390.protonvpn.net', 'endpoint_ip': '89.222.100.65', 'failed_at': NOW}],
                   'round_attempts': ['node-ca-59.protonvpn.net', 'node-ca-60.protonvpn.net',
                                      'node-us-119.protonvpn.net', 'node-us-390.protonvpn.net']}
        # us-391 shares 89.222.100.x with the server that just failed.
        self.assertEqual(select_candidate(candidates, history, ['Canada'], ['United States'], NOW).hostname,
                         'node-us-69.protonvpn.net')

    def test_round_attempt_cap_does_not_reuse_twenty_four_hour_failures(self):
        extra = copy.deepcopy(self.provider['servers'][0])
        extra.update(hostname='node-ca-61.protonvpn.net', ips=['84.20.16.30'])
        self.provider['servers'].append(extra)
        candidates = eligible_candidates(validate_catalog(self.provider, NOW), ['Canada', 'United States'])
        history = {'failures': [], 'round_attempts': ['node-ca-59.protonvpn.net', 'node-ca-60.protonvpn.net']}
        self.assertEqual(select_candidate(candidates, history, ['Canada'], ['United States'], NOW).country, 'United States')
        history = {'failures': [{'hostname': 'node-ca-59.protonvpn.net', 'endpoint_ip': '84.20.16.29', 'failed_at': NOW-2000}], 'round_attempts': []}
        self.assertEqual(select_candidate(candidates, history, ['Canada'], ['United States'], NOW).country, 'Canada')

    def test_exclusion_expiry_history_cap_and_all_excluded_oldest(self):
        candidates = eligible_candidates(validate_catalog(self.provider, NOW), ['Canada'])
        a, b = candidates
        hist = [{'hostname': a.hostname, 'endpoint_ip': a.endpoint_ip, 'failed_at': NOW-100},
                {'hostname': b.hostname, 'endpoint_ip': b.endpoint_ip, 'failed_at': NOW-50}]
        self.assertEqual(select_candidate(candidates, hist, ['Canada'], [], NOW), a)
        hist.pop()
        self.assertEqual(select_candidate(candidates, hist, ['Canada'], [], NOW), b)
        self.assertEqual(select_candidate(candidates, hist, ['Canada'], [], NOW+1700), a)
        self.assertEqual(bounded_history(hist, NOW+86400), [])
        self.assertEqual(bounded_history([{'failed_at': float('nan')}], NOW), [])
        self.assertEqual(bounded_history([*hist]*300, NOW), hist)
        self.assertEqual(select_candidate(candidates, {}, ['Canada'], [], NOW), a)
        self.assertIsNone(select_candidate(candidates, [], [], [], NOW))

    def test_distinct_history_retains_newest_256_without_input_mutation(self):
        history=[{'hostname':f'node-ca-{i}.protonvpn.net',
                  'endpoint_ip':f'84.20.{i//256}.{i%256}',
                  'failed_at':NOW-300+i} for i in range(300)]
        original=copy.deepcopy(history)
        # Scramble order so a tail slice cannot masquerade as newest retention.
        actual=bounded_history(list(reversed(history)),NOW)
        self.assertEqual(len(actual),256)
        self.assertEqual(actual,history[44:])
        self.assertEqual(history,original)

    def test_static_fetch_has_parent_deadline_for_blocked_dns(self):
        # Exercise a real local child blocked before any network work. This is
        # the same failure boundary as a libc resolver that ignores socket timeouts.
        original = subprocess.Popen
        children = []
        def blocked_child(*args, **kwargs):
            child = original([sys.executable, '-c', 'import time; time.sleep(60)'], **kwargs)
            children.append(child)
            return child
        with patch('vpn_recovery.catalog.subprocess.Popen', blocked_child):
            with self.assertRaises(TimeoutError):
                fetch_public_catalog(CATALOG_URL, {}, .05, 1024)
        self.assertIsNotNone(children[0].poll(), 'blocked resolver worker must be reaped')

    def test_static_fetch_worker_failure_is_sanitized(self):
        original = subprocess.Popen
        def failed_child(*args, **kwargs):
            return original([sys.executable, '-c', 'import sys; sys.stderr.write("private diagnostic"); sys.exit(1)'], **kwargs)
        with patch('vpn_recovery.catalog.subprocess.Popen', failed_child):
            with self.assertRaises(ValueError) as caught:
                fetch_public_catalog(CATALOG_URL, {}, 1, 1024)
        self.assertNotIn('private diagnostic', str(caught.exception))


if __name__ == '__main__':
    unittest.main()
