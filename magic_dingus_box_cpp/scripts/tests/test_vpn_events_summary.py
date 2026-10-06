"""vpn_events_summary.py: the VPN tunnel line on the Box Health card.

The owner's box dropped its tunnel ~170 times over 2026-10-03/04 and
nothing on any screen said so. gluetun_cascade_restart.sh now records each
transition; these tests pin how that record becomes one plain-language
PASS/WARN line — counts, down time, the WARN thresholds, the "down right
now" case and the port-forwarding-only cause. Stdlib only.
"""
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "vpn_events_summary.py"
_spec = importlib.util.spec_from_file_location("vpn_events_summary", SCRIPT)
ves = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ves)

NOW = 1_800_000_000
H = 3600
M = 60


def lines(*events):
    return [" ".join(str(x) for x in e) for e in events]


class SummarizeTests(unittest.TestCase):
    def test_empty_log_has_no_history(self):
        s = ves.summarize([], NOW)
        self.assertFalse(s["has_history"])
        self.assertEqual(ves.verdict(s)[0], "pass")
        self.assertIn("no history", ves.verdict(s)[1])

    def test_quiet_box_passes(self):
        s = ves.summarize(lines((NOW - 30 * H, "watch")), NOW)
        self.assertEqual(s["drops"], 0)
        level, msg = ves.verdict(s)
        self.assertEqual(level, "pass")
        self.assertEqual(msg, "VPN tunnel steady: no drops in the last 24 h")

    def test_counts_down_time_and_longest(self):
        log = lines(
            (NOW - 30 * H, "watch"),
            (NOW - 5 * H, "unhealthy", "tunnel"),
            (NOW - 5 * H + 6 * M, "healthy"),
            (NOW - 2 * H, "unhealthy", "tunnel"),
            (NOW - 2 * H + 5 * M, "restart"),
            (NOW - 2 * H + 9 * M, "healthy"),
        )
        s = ves.summarize(log, NOW)
        self.assertEqual(s["drops"], 2)
        self.assertEqual(s["down_s"], 15 * M)
        self.assertEqual(s["longest_s"], 9 * M)
        self.assertEqual(s["restarts"], 1)
        self.assertEqual(s["last_drop_ts"], NOW - 2 * H)
        level, msg = ves.verdict(s)
        self.assertEqual(level, "pass")
        self.assertEqual(
            msg, "VPN tunnel dropped 2 times in the last 24 h "
                 "(down 15 min total, longest 9 min, last 2 h ago)")

    def test_owner_box_burst_warns(self):
        # One drop every ~11 min for 4 h, each ~4-5 min long.
        ev = [(NOW - 30 * H, "watch")]
        t = NOW - 5 * H
        for _ in range(23):
            ev.append((t, "unhealthy", "tunnel"))
            ev.append((t + 4 * M + 20, "healthy"))
            t += 11 * M
        s = ves.summarize(lines(*ev), NOW)
        self.assertEqual(s["drops"], 23)
        self.assertEqual(s["down_s"], 23 * (4 * M + 20))
        level, msg = ves.verdict(s)
        self.assertEqual(level, "warn")
        self.assertTrue(msg.startswith(
            "VPN tunnel dropped 23 times in the last 24 h (down 1 h 39 min total, "
            "longest 4 min"), msg)
        self.assertIn("try another VPN country", msg)

    def test_drop_threshold(self):
        def n_drops(n):
            ev = [(NOW - 30 * H, "watch")]
            for i in range(n):
                ev += [(NOW - 10 * H + i * H, "unhealthy", "tunnel"),
                       (NOW - 10 * H + i * H + M, "healthy")]
            return ves.verdict(ves.summarize(lines(*ev), NOW))[0]
        self.assertEqual(n_drops(ves.WARN_DROPS - 1), "pass")
        self.assertEqual(n_drops(ves.WARN_DROPS), "warn")

    def test_down_time_threshold_alone_warns(self):
        log = lines((NOW - 30 * H, "watch"),
                    (NOW - 3 * H, "unhealthy", "tunnel"),
                    (NOW - 3 * H + 31 * M, "healthy"))
        level, msg = ves.verdict(ves.summarize(log, NOW))
        self.assertEqual(level, "warn")
        self.assertIn("dropped once", msg)
        self.assertIn("down 31 min total", msg)

    def test_outage_straddling_the_window_is_clipped_not_counted(self):
        log = lines((NOW - 30 * H, "watch"),
                    (NOW - 25 * H, "unhealthy", "tunnel"),
                    (NOW - 24 * H + 10 * M, "healthy"))
        s = ves.summarize(log, NOW)
        self.assertEqual(s["drops"], 0)          # began before the window
        self.assertEqual(s["down_s"], 10 * M)    # only the part inside it
        self.assertEqual(s["longest_s"], 10 * M)

    def test_old_drops_only_mention_last_drop(self):
        log = lines((NOW - 60 * H, "unhealthy", "tunnel"),
                    (NOW - 60 * H + M, "healthy"))
        level, msg = ves.verdict(ves.summarize(log, NOW))
        self.assertEqual(level, "pass")
        self.assertEqual(msg, "VPN tunnel steady: no drops in the last 24 h "
                              "(last drop 2 d 12 h ago)")

    def test_ongoing_outage_warns_down_right_now(self):
        log = lines((NOW - 30 * H, "watch"),
                    (NOW - 25 * M, "unhealthy", "tunnel"),
                    (NOW - 20 * M, "restart"))
        s = ves.summarize(log, NOW)
        self.assertTrue(s["ongoing"])
        self.assertEqual(s["down_s"], 25 * M)
        level, msg = ves.verdict(s)
        self.assertEqual(level, "warn")
        self.assertTrue(msg.startswith("VPN tunnel is DOWN right now (for 25 min)"), msg)

    def test_fresh_ongoing_blip_is_not_down_right_now(self):
        log = lines((NOW - 30 * H, "watch"), (NOW - 2 * M, "unhealthy", "tunnel"))
        level, msg = ves.verdict(ves.summarize(log, NOW))
        self.assertEqual(level, "pass")
        self.assertIn("dropped once", msg)

    def test_open_outage_closed_when_gluetun_is_healthy_now(self):
        # The recovery fired while no watcher was subscribed; the box knows
        # it is healthy now, so the outage is not still running.
        log = lines((NOW - 30 * H, "watch"),
                    (NOW - 6 * H, "unhealthy", "tunnel"),
                    (NOW - 6 * H + 5 * M, "restart"))
        s = ves.summarize(log, NOW, current_health="healthy")
        self.assertFalse(s["ongoing"])
        self.assertEqual(s["down_s"], 5 * M)

    def test_repeated_unhealthy_is_one_outage(self):
        log = lines((NOW - 2 * H, "unhealthy", "tunnel"),
                    (NOW - 2 * H + M, "watch"),
                    (NOW - 2 * H + 2 * M, "unhealthy", "tunnel"),
                    (NOW - 2 * H + 3 * M, "healthy"))
        s = ves.summarize(log, NOW)
        self.assertEqual(s["drops"], 1)
        self.assertEqual(s["down_s"], 3 * M)

    def test_healthy_without_outage_is_ignored(self):
        s = ves.summarize(lines((NOW - H, "healthy"), (NOW - M, "healthy")), NOW)
        self.assertEqual(s["drops"], 0)
        self.assertEqual(s["down_s"], 0)

    def test_out_of_order_lines_are_sorted(self):
        # The watcher writes `restart` after its confirm sleep, after a
        # queued healthy event that it stamps with Docker's earlier time.
        log = lines((NOW - H, "unhealthy", "tunnel"),
                    (NOW - H + 5 * M, "restart"),
                    (NOW - H + 2 * M, "healthy"))
        s = ves.summarize(log, NOW)
        self.assertEqual(s["down_s"], 2 * M)
        self.assertFalse(s["ongoing"])

    def test_portfwd_cause_is_named(self):
        log = lines((NOW - 30 * H, "watch"),
                    (NOW - 3 * H, "unhealthy", "portfwd"),
                    (NOW - 3 * H + 10 * M, "healthy"),
                    (NOW - 2 * H, "unhealthy", "tunnel"),
                    (NOW - 2 * H + M, "healthy"))
        s = ves.summarize(log, NOW)
        self.assertEqual((s["drops_portfwd"], s["drops_tunnel"]), (1, 1))
        self.assertIn("1 of those, the tunnel itself still worked",
                      ves.verdict(s)[1])
        only_pf = lines((NOW - 30 * H, "watch"),
                        (NOW - 3 * H, "unhealthy", "portfwd"),
                        (NOW - 3 * H + 10 * M, "healthy"))
        self.assertIn("each time the tunnel itself still worked",
                      ves.verdict(ves.summarize(only_pf, NOW))[1])

    def test_recording_younger_than_window_says_so(self):
        log = lines((NOW - 5 * H, "watch"))
        msg = ves.verdict(ves.summarize(log, NOW))[1]
        self.assertEqual(msg, "VPN tunnel steady: no drops in the 5 h since "
                              "recording began")

    def test_malformed_and_future_lines_are_skipped(self):
        log = ["", "garbage", "abc unhealthy tunnel", f"{NOW + H} unhealthy tunnel",
               f"{NOW - H} watch"]
        s = ves.summarize(log, NOW)
        self.assertEqual(s["drops"], 0)
        self.assertTrue(s["has_history"])


class FormatTests(unittest.TestCase):
    def test_durations(self):
        self.assertEqual(ves.fmt_duration(10), "under 1 min")
        self.assertEqual(ves.fmt_duration(9 * M + 59), "9 min")
        self.assertEqual(ves.fmt_duration(H), "1 h")
        self.assertEqual(ves.fmt_duration(H + 40 * M), "1 h 40 min")
        self.assertEqual(ves.fmt_duration(51 * H), "2 d 3 h")
        self.assertEqual(ves.fmt_duration(48 * H), "2 d")


class CliTests(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *args],
                              capture_output=True, text=True, timeout=30)

    def test_missing_file_prints_a_pass_verdict(self):
        r = self.run_cli("--file", "/nonexistent/vpn_events.log", "--now", str(NOW))
        self.assertEqual(r.returncode, 0, r.stderr)
        level, _, msg = r.stdout.strip().partition("\t")
        self.assertEqual(level, "pass")
        self.assertIn("no history", msg)

    def test_file_and_json(self):
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write("\n".join(lines((NOW - 2 * H, "unhealthy", "tunnel"),
                                    (NOW - 2 * H + 3 * M, "healthy"))) + "\n")
            path = f.name
        try:
            r = self.run_cli("--file", path, "--now", str(NOW), "--json")
            self.assertEqual(r.returncode, 0, r.stderr)
            doc = json.loads(r.stdout)
            self.assertEqual(doc["drops"], 1)
            self.assertEqual(doc["down_s"], 3 * M)
            r = self.run_cli("--file", path, "--now", str(NOW))
            self.assertTrue(r.stdout.startswith("pass\tVPN tunnel dropped once"), r.stdout)
        finally:
            Path(path).unlink()


if __name__ == "__main__":
    unittest.main()
