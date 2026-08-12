import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..",
                                "missing_search"))
import missing_search  # noqa: E402


class WaitForPingTests(unittest.TestCase):
    # These originally targeted wait_for_radarr(); the module generalized
    # it to wait_for_ping(base) when the Sonarr pass landed (2026-08-02)
    # and, with no CI enrollment, the tests silently rotted against the
    # old name. Same intents, current API.
    def test_returns_true_when_ping_answers(self):
        fake = mock.MagicMock()
        fake.__enter__.return_value.status = 200
        with mock.patch.object(missing_search.urllib.request, "urlopen",
                               return_value=fake):
            self.assertTrue(missing_search.wait_for_ping(
                missing_search.RADARR_BASE, timeout_s=1))

    def test_returns_false_when_never_ready(self):
        # ConnectionResetError is what a mid-startup Radarr produces —
        # the boot catch-up timer run must poll through it, not crash.
        with mock.patch.object(missing_search.urllib.request, "urlopen",
                               side_effect=ConnectionResetError):
            self.assertFalse(missing_search.wait_for_ping(
                missing_search.RADARR_BASE, timeout_s=0.2, poll_s=0.05))


class MainErrorHandlingTests(unittest.TestCase):
    def test_unready_radarr_is_a_clean_retry_exit(self):
        # Not-ready returns 0 BY DESIGN: a oneshot exiting non-zero leaves
        # the unit 'failed' until the next 4h tick and verify_box counts
        # failed units (observed live 2026-08-03) — deferral is a
        # successful decision, and Persistent=true never re-runs a failed
        # unit anyway.
        with mock.patch.object(missing_search, "load_env",
                               return_value={"RADARR_API_KEY": "k"}), \
             mock.patch.object(missing_search, "in_boot_window",
                               return_value=False), \
             mock.patch.object(missing_search, "wait_for_ping",
                               return_value=False):
            self.assertEqual(missing_search.main(), 0)

    def test_connection_reset_mid_fetch_is_graceful(self):
        # A reset DURING the response read raises raw ConnectionResetError
        # (not URLError) — observed live on the post-boot catch-up run.
        # main() must skip the sweep cleanly, not crash.
        with mock.patch.object(missing_search, "load_env",
                               return_value={"RADARR_API_KEY": "k"}), \
             mock.patch.object(missing_search, "in_boot_window",
                               return_value=False), \
             mock.patch.object(missing_search, "wait_for_ping",
                               return_value=True), \
             mock.patch.object(missing_search, "download_client_unavailable",
                               return_value=False), \
             mock.patch.object(missing_search, "http",
                               side_effect=ConnectionResetError("peer")):
            self.assertEqual(missing_search.main(), 0)


if __name__ == "__main__":
    unittest.main()
