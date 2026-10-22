"""Real temporary files exercise the clone swap guard without touching host swap."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HELPER = Path(__file__).resolve().parents[2] / 'scripts/golden_image/clone_swap_cleanup.py'


class SwapCleanupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert HELPER.is_file(), 'clone preparation needs an allocated-swap scrubber'
        spec = importlib.util.spec_from_file_location('clone_swap_cleanup', HELPER)
        cls.mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.mod)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'etc').mkdir()
        (self.root / 'etc/fstab').write_text('')
        self.swaps = self.root / 'swaps'
        self.swaps.write_text('Filename\tType\tSize\tUsed\tPriority\n')
        self.formats = []

    def swapfile(self, path='/var/swap'):
        target = self.root / path.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        content = bytearray(b'synthetic-stale-memory-' * 1000)
        content[1036:1052] = bytes(range(16))
        content[4086:4096] = b'SWAPSPACE2'
        target.write_bytes(content)
        return target

    def format_swap(self, args, **kwargs):
        target = Path(args[-1])
        self.assertEqual(set(target.read_bytes()), {0})
        self.formats.append(args)
        return subprocess.CompletedProcess(args, 0)

    def run_cleanup(self, check_only=False):
        with patch.object(self.mod.subprocess, 'run', side_effect=self.format_swap), \
             patch.object(self.mod.shutil, 'which', return_value='/usr/sbin/mkswap'):
            self.mod.clean_swap(self.root, self.swaps, check_only=check_only)

    def test_inactive_legacy_swap_is_zeroed_and_reformatted_with_same_uuid(self):
        target = self.swapfile()
        size = target.stat().st_size
        self.run_cleanup()
        self.assertEqual(target.stat().st_size, size)
        self.assertEqual(set(target.read_bytes()), {0})
        self.assertEqual(self.formats[0][1:-1], ['--uuid', '00010203-0405-0607-0809-0a0b0c0d0e0f'])

    def test_check_is_read_only(self):
        target = self.swapfile()
        before = target.read_bytes()
        self.run_cleanup(check_only=True)
        self.assertEqual(target.read_bytes(), before)
        self.assertEqual(self.formats, [])

    def test_active_disk_file_or_partition_rejected_before_writing(self):
        target = self.swapfile()
        before = target.read_bytes()
        for source, kind in [('/var/swap', 'file'), ('/dev/mmcblk0p3', 'partition')]:
            with self.subTest(source=source):
                self.swaps.write_text('Filename Type Size Used Priority\n' + source + ' ' + kind + ' 2048 0 -2\n')
                with self.assertRaisesRegex(RuntimeError, 'active disk'):
                    self.run_cleanup()
                self.assertEqual(target.read_bytes(), before)

    def test_zram_allowed(self):
        self.swaps.write_text('Filename Type Size Used Priority\n/dev/zram0 partition 2048 10 100\n')
        self.swapfile()
        self.run_cleanup()
        self.assertEqual(len(self.formats), 1)

    def test_fstab_swap_file_with_escaped_space_is_scrubbed(self):
        target = self.swapfile('/swap area/pagefile')
        (self.root / 'etc/fstab').write_text('/swap\\040area/pagefile none swap sw 0 0\n')
        self.run_cleanup()
        self.assertEqual(set(target.read_bytes()), {0})

    def test_standard_swapfile_is_scrubbed(self):
        target = self.swapfile('/swapfile')
        self.run_cleanup()
        self.assertEqual(set(target.read_bytes()), {0})

    def test_symlink_file_and_parent_refused_without_touching_target(self):
        target = self.swapfile('/elsewhere/pagefile')
        before = target.read_bytes()
        (self.root / 'var').mkdir()
        alias = self.root / 'var/swap'
        alias.symlink_to(target)
        with self.assertRaisesRegex(RuntimeError, 'symlink'):
            self.run_cleanup()
        alias.unlink()
        (self.root / 'var').rmdir()
        (self.root / 'var').symlink_to(target.parent)
        (self.root / 'etc/fstab').write_text('/var/pagefile none swap sw 0 0\n')
        with self.assertRaisesRegex(RuntimeError, 'symlink'):
            self.run_cleanup()
        self.assertEqual(target.read_bytes(), before)

    def test_off_root_filesystem_refused_before_writing_any_candidate(self):
        target = self.swapfile()
        before = target.read_bytes()
        real_stat = self.mod.os.lstat
        def other_device(path):
            result = real_stat(path)
            if Path(path) == target:
                fields = list(result)
                fields[2] = result.st_dev + 1
                return os.stat_result(fields)
            return result
        with patch.object(self.mod.os, 'lstat', side_effect=other_device):
            with self.assertRaisesRegex(RuntimeError, 'root filesystem'):
                self.run_cleanup()
        self.assertEqual(target.read_bytes(), before)

    def test_unrecognized_file_and_hardlink_refused(self):
        target = self.swapfile()
        os.link(target, self.root / 'alias')
        with self.assertRaisesRegex(RuntimeError, 'hardlink'):
            self.run_cleanup()
        (self.root / 'alias').unlink()
        target.write_bytes(b'ordinary data, not swap')
        with self.assertRaisesRegex(RuntimeError, 'swap signature'):
            self.run_cleanup()
        self.assertEqual(target.read_bytes(), b'ordinary data, not swap')

    def test_fstab_partition_or_uuid_refused_even_if_inactive(self):
        for source in ['/dev/mmcblk0p3', 'UUID=fixture', '../../outside']:
            with self.subTest(source=source):
                (self.root / 'etc/fstab').write_text(source + ' none swap sw 0 0\n')
                with self.assertRaises(RuntimeError):
                    self.run_cleanup()

    def test_unreadable_or_malformed_swap_inventory_fails_closed(self):
        for content in ['', 'not a swaps table\n', 'Filename Type Size Used Priority\ncorrupt\n']:
            self.swaps.write_text(content)
            with self.assertRaises(RuntimeError):
                self.run_cleanup()

    def test_no_swap_is_noop(self):
        self.run_cleanup()
        self.assertEqual(self.formats, [])


if __name__ == '__main__':
    unittest.main()
