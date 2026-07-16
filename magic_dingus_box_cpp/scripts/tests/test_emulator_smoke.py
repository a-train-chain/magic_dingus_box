import os
import tempfile
import time
import unittest
from unittest import mock

import emulator_smoke_test as smoke


class ReadinessTests(unittest.TestCase):
    def test_stale_marker_is_not_ready(self):
        with tempfile.NamedTemporaryFile(mode="w") as marker:
            marker.write(str(os.getpid()))
            marker.flush()
            old = time.time() - 60
            os.utime(marker.name, (old, old))

            self.assertIsNone(smoke.read_ready_pid(marker.name, time.time()))

    @mock.patch.object(smoke, "retroarch_pid_is_live", return_value=True)
    def test_fresh_marker_with_live_pid_is_ready(self, _live):
        with tempfile.NamedTemporaryFile(mode="w") as marker:
            marker.write("1234\n")
            marker.flush()

            self.assertEqual(
                smoke.read_ready_pid(marker.name, time.time() - 1), 1234)

    @mock.patch.object(smoke, "retroarch_pid_is_live", return_value=False)
    def test_fresh_marker_with_dead_pid_is_not_ready(self, _live):
        with tempfile.NamedTemporaryFile(mode="w") as marker:
            marker.write("1234\n")
            marker.flush()

            self.assertIsNone(
                smoke.read_ready_pid(marker.name, time.time() - 1))

    def test_wayland_without_direct_display_and_swapchain_errors_are_fatal(self):
        self.assertIn(
            "Wayland",
            smoke.launch_log_failure(
                "[ERROR] [Wayland]: Failed to connect to Wayland server."),
        )
        self.assertIn(
            "QueuePresent",
            smoke.launch_log_failure(
                "[Vulkan]: QueuePresent failed, destroying swapchain"),
        )

    def test_wayland_probe_followed_by_khr_display_is_not_fatal(self):
        self.assertIsNone(
            smoke.launch_log_failure(
                "[ERROR] [Wayland]: Failed to connect to Wayland server.\n"
                "[INFO] [Vulkan]: Found vulkan context: \"khr_display\".\n"
                "[INFO] [Vulkan]: Using resolution 1920x1080.\n"
            )
        )

    def test_clean_launch_log_has_no_failure(self):
        self.assertIsNone(
            smoke.launch_log_failure("[KMS]: New FB: 1920x1080\nRunning content"))


if __name__ == "__main__":
    unittest.main()
