"""Exercise first-boot SSH recovery without touching host services."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class FirstBootSSH(unittest.TestCase):
    def run_block(self, active=False, enabled=True):
        source = (ROOT / 'scripts/golden_image/first_boot.sh').read_text()
        block = source.split('# Restart sshd so', 1)[1].split('# Step 1b:', 1)[0]
        block = '# Restart sshd so' + block
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            fake = p / 'systemctl'
            fake.write_text('''#!/bin/bash
printf '%s\\n' "$*" >> "$CALLS"
case "$1" in
 is-active) test "$ACTIVE" = 1 ;;
 is-enabled) test "$ENABLED" = 1 ;;
 *) exit 0 ;;
esac
''')
            fake.chmod(0o755)
            env = dict(os.environ, PATH=directory + ':' + os.environ['PATH'],
                       ACTIVE=str(int(active)), ENABLED=str(int(enabled)), CALLS=str(p/'calls'))
            result = subprocess.run(['bash', '-eu', '-c', 'log() { :; };\n'+block], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return (p/'calls').read_text()

    def test_enabled_failed_server_is_restarted_without_waiting(self):
        self.assertIn('--no-block restart ssh', self.run_block())

    def test_active_server_is_restarted_without_waiting(self):
        self.assertIn('--no-block restart ssh', self.run_block(active=True))

    def test_intentionally_disabled_server_stays_off(self):
        self.assertNotIn('restart', self.run_block(enabled=False))

    def test_key_generation_precedes_ssh_start(self):
        unit = (ROOT/'systemd/magic-first-boot.service').read_text()
        before = ' '.join(line[7:] for line in unit.splitlines() if line.startswith('Before='))
        self.assertIn('ssh.service', before.split())

if __name__ == '__main__':
    unittest.main()
