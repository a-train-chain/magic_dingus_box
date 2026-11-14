"""Password synchronization must not clear an operator/playback cap."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'sync_qbit_password.sh'
CURL = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
p = Path(os.environ['QBIT_TEST_STATE']); s = json.loads(p.read_text()); a = sys.argv[1:]
url = next(v for v in a if v.startswith('http://'))
with open(os.environ['QBIT_TEST_LOG'], 'a') as f: f.write(json.dumps(a) + '\\n')
if url.endswith('/auth/login'):
    requested = next(v for v in a if 'password=' in v).split('password=')[-1]
    print('Ok.' if requested == s['password'] else 'Fails.')
elif url.endswith('/speedLimitsMode'): print('1' if s['alt'] else '0')
elif url.endswith('/toggleSpeedLimitsMode'): s['alt'] = not s['alt']
elif url.endswith('/setPreferences'):
    prefs = json.loads(next(v for v in a if v.startswith('json='))[5:])
    if 'web_ui_password' in prefs: s['password'] = prefs['web_ui_password']
else: print('200')
p.write_text(json.dumps(s))
'''

class PasswordOwnershipTests(unittest.TestCase):
    def test_both_login_paths_preserve_alt_mode_and_converge_rates(self):
        for starting_password in ('fixture-password', 'adminadmin'):
            with self.subTest(starting_password=starting_password), tempfile.TemporaryDirectory() as d:
                root = Path(d); curl = root / 'curl'; curl.write_text(CURL); curl.chmod(0o755)
                state = root / 'state'; state.write_text(json.dumps({'password': starting_password, 'alt': True}))
                env_file = root / 'env'; env_file.write_text('QBITTORRENT_ADMIN_PASSWORD=fixture-password\n')
                log = root / 'log'
                env = dict(os.environ, PATH=d + ':' + os.environ['PATH'], ENV_FILE=str(env_file),
                           QBIT_TEST_STATE=str(state), QBIT_TEST_LOG=str(log))
                result = subprocess.run(['bash', str(SCRIPT)], env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                final = json.loads(state.read_text()); self.assertEqual(final['password'], 'fixture-password')
                requests = [json.loads(line) for line in log.read_text().splitlines()]
                prefs = [json.loads(arg[5:]) for args in requests for arg in args if arg.startswith('json=')]
                self.assertIn({'alt_dl_limit': 2097152, 'alt_up_limit': 8192}, prefs)
                if starting_password == 'adminadmin':
                    self.assertIn({'web_ui_username': 'admin', 'web_ui_password': 'fixture-password',
                                   'bypass_local_auth': False, 'bypass_auth_subnet_whitelist_enabled': False}, prefs)
                self.assertTrue(final['alt'], 'password sync cleared an operator/playback cap')

if __name__ == '__main__': unittest.main()
