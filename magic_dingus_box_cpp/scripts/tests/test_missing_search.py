import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..",
                                "missing_search"))
import missing_search  # noqa: E402


class WaitForRadarrTests(unittest.TestCase):
    def test_returns_true_when_ping_answers(self):
        fake = mock.MagicMock()
        fake.__enter__.return_value.status = 200
        with mock.patch.object(missing_search.urllib.request, "urlopen",
                               return_value=fake):
            self.assertTrue(missing_search.wait_for_radarr(timeout_s=1))

    def test_returns_false_when_never_ready(self):
        # ConnectionResetError is what a mid-startup Radarr produces —
        # the boot catch-up timer run must poll through it, not crash.
        with mock.patch.object(missing_search.urllib.request, "urlopen",
                               side_effect=ConnectionResetError):
            self.assertFalse(missing_search.wait_for_radarr(timeout_s=0.2,
                                                            poll_s=0.05))


class MainErrorHandlingTests(unittest.TestCase):
    def test_unready_radarr_is_a_clean_retry_exit(self):
        with mock.patch.object(missing_search, "load_env"), \
             mock.patch.object(missing_search, "wait_for_radarr",
                               return_value=False):
            missing_search.RADARR_API_KEY = "k"
            self.assertEqual(missing_search.main(), 1)

    def test_connection_reset_mid_fetch_is_graceful(self):
        # A reset DURING the response read raises raw ConnectionResetError
        # (not URLError) — observed live on the post-boot catch-up run.
        with mock.patch.object(missing_search, "load_env"), \
             mock.patch.object(missing_search, "wait_for_radarr",
                               return_value=True), \
             mock.patch.object(missing_search, "http",
                               side_effect=ConnectionResetError("peer")):
            missing_search.RADARR_API_KEY = "k"
            self.assertEqual(missing_search.main(), 1)


if __name__ == "__main__":
    unittest.main()
