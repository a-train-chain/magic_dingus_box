"""Common I/O boundaries: policy must fail closed and never execute .env."""
import fcntl
import json
from pathlib import Path
import stat
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    from vpn_recovery.common import Paths, atomic_json, bounded_lock, load_config
except ImportError:
    Paths = None


class CommonTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(Paths, 'vpn_recovery common I/O package is not implemented')
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.paths = Paths(root, root/'services', root/'config', root/'state', root/'run', root/'compose.lock')
        self.paths.services.mkdir()
        self.paths.config.mkdir()

    def test_policy_defaults_preserve_country(self):
        (self.paths.services/'.env').write_text('VPN_COUNTRIES="Canada,United States"\nVPN_PORT_FORWARDING=on\n')
        self.assertEqual(load_config(self.paths), {
            'provider': 'protonvpn', 'type': 'wireguard', 'forwarding': True,
            'preferred_countries': ['Canada', 'United States'], 'fallback_countries': []})
        (self.paths.config/'vpn_recovery.json').write_text('{"fallback_countries":["Netherlands"]}')
        self.assertEqual(load_config(self.paths)['fallback_countries'], ['Netherlands'])

    def test_env_quotes_duplicates_and_no_execution(self):
        marker = self.paths.base/'executed'
        (self.paths.services/'.env').write_text(
            f'# comment\nIGNORED=$(touch {marker})\nVPN_COUNTRIES=Canada\nVPN_COUNTRIES=\'Netherlands\'\n'
            'VPN_SERVICE_PROVIDER=custom\nVPN_TYPE=openvpn\nVPN_PORT_FORWARDING=off\n')
        config = load_config(self.paths)
        self.assertEqual(config['provider'], 'custom')
        self.assertEqual(config['type'], 'openvpn')
        self.assertFalse(config['forwarding'])
        self.assertEqual(config['preferred_countries'], ['Netherlands'])
        self.assertFalse(marker.exists())

    def test_invalid_config_fails_closed(self):
        for value in [{"typo": True}, {"preferred_countries": "Canada"},
                      {"fallback_countries": [1]}, {"provider": "protonvpn"},
                      {"fallback_countries": [""]}, [], None]:
            with self.subTest(value=value):
                (self.paths.config/'vpn_recovery.json').write_text(json.dumps(value))
                with self.assertRaises(ValueError):
                    load_config(self.paths)
        (self.paths.config/'vpn_recovery.json').write_text('{')
        with self.assertRaises(ValueError):
            load_config(self.paths)

    def test_invalid_env_fails_closed(self):
        for line in ['VPN_TYPE=oops', 'VPN_PORT_FORWARDING=maybe', 'VPN_COUNTRIES=$(secret)']:
            with self.subTest(line=line):
                (self.paths.services/'.env').write_text(line)
                with self.assertRaises(ValueError) as caught:
                    load_config(self.paths)
                self.assertNotIn('secret', str(caught.exception))

    def test_atomic_json_mode_and_replacement(self):
        target = self.paths.state/'status.json'
        atomic_json(target, {'port': 1234})
        self.assertEqual(json.loads(target.read_text()), {'port': 1234})
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)
        atomic_json(target, {'port': 4321})
        self.assertEqual(json.loads(target.read_text()), {'port': 4321})
        self.assertEqual(list(target.parent.iterdir()), [target])

    def test_lock_read_open_missing_and_busy_are_bounded(self):
        lock = self.paths.compose_lock
        with self.assertRaises(OSError):
            with bounded_lock(lock, .01):
                self.fail('missing lock granted')
        lock.touch(mode=0o400)
        with lock.open('rb') as holder:
            fcntl.flock(holder, fcntl.LOCK_EX | fcntl.LOCK_NB)
            start = time.monotonic()
            with self.assertRaises(TimeoutError):
                with bounded_lock(lock, .03):
                    self.fail('busy lock granted')
            self.assertLess(time.monotonic()-start, .5)
        with bounded_lock(lock, .03):
            pass
        self.assertEqual(lock.read_bytes(), b'')


if __name__ == '__main__':
    unittest.main()
