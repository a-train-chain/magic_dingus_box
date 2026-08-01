"""
Marker-aware Gluetun cascade + unpause compose fallback.

Background: playback_services_pause.sh stops mdb_radarr / mdb_prowlarr /
mdb_byparr during games/movies (frees RAM on the 2 GB boxes) and maintains
/tmp/mdb_playback_services_paused for the duration. The cascade watcher
(gluetun_cascade_restart.sh) reacts to Gluetun start events by re-linking
the four netns-sharing dependents — which, pre-fix, brought the paused
three back UP mid-game (observed live 2026-07-31: Super Mario 64 running
with the full stack Up, defeating the pause).

Contract under test:
  * cascade, no marker  → full re-link: restart + up -d of all four.
  * cascade, marker     → qbittorrent-only re-link (it stays up during
    playback and needs the re-link for active downloads), plus an
    enforcement `docker stop` of the three so any zombie revived by an
    earlier race converges back to the kiosk's intent.
  * unpause, start fails → `compose up -d` fallback for the failed
    services. Needed because network_mode:service:gluetun resolves to
    Gluetun's CONTAINER ID at create time (verified on magicpi5), so a
    plain `docker start` of a dependent whose Gluetun was RECREATED
    tries to join a dead netns; only compose recreates against the new
    one.
  * unpause, start succeeds → no compose call at all (fallback dormant).

Both scripts run for real with a stub `docker` on PATH that logs every
invocation; assertions grep the log. The stub's `events` subcommand emits
one start event and exits, which ends the watcher loop via pipe EOF.
"""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parent.parent
CASCADE = SCRIPTS_DIR / "gluetun_cascade_restart.sh"
PAUSE = SCRIPTS_DIR / "playback_services_pause.sh"

# Must match both scripts and admin.py PLAYBACK_PAUSE_MARKER.
MARKER = Path("/tmp/mdb_playback_services_paused")

DOCKER_STUB = """#!/bin/bash
echo "$@" >> "$DOCKER_LOG"
case "$1" in
    events)
        printf '2026-07-31T00:00:00 start\\n'
        ;;
    inspect)
        # Pause script: bare `inspect NAME` (existence) and
        # `inspect -f {{.State.Running}} NAME` (health). Cascade's
        # unhealthy branch never runs in these tests.
        echo "${INSPECT_RUNNING:-true}"
        ;;
esac
exit 0
"""


class StubDockerTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        tmp = Path(self._tmp.name)
        stub_bin = tmp / "bin"
        stub_bin.mkdir()
        docker = stub_bin / "docker"
        docker.write_text(DOCKER_STUB)
        docker.chmod(0o755)

        self.compose_dir = tmp / "services"
        self.compose_dir.mkdir()
        (self.compose_dir / "docker-compose.yml").write_text("services: {}\n")

        self.log = tmp / "docker.log"
        self.log.write_text("")
        self.env = dict(
            os.environ,
            PATH=f"{stub_bin}:{os.environ['PATH']}",
            DOCKER_LOG=str(self.log),
            COMPOSE_DIR=str(self.compose_dir),
            STABILIZE_SLEEP="0",
        )
        MARKER.unlink(missing_ok=True)
        self.addCleanup(lambda: MARKER.unlink(missing_ok=True))
        self.addCleanup(self._tmp.cleanup)

    def run_script(self, script, *args, extra_env=None):
        env = dict(self.env, **(extra_env or {}))
        return subprocess.run(
            ["bash", str(script), *args],
            env=env, capture_output=True, text=True, timeout=30)

    def log_lines(self):
        return self.log.read_text().splitlines()

    def compose_lines(self):
        return [l for l in self.log_lines() if l.startswith("compose")]


class CascadeStartEventTests(StubDockerTestCase):
    def test_no_marker_full_cascade(self):
        result = self.run_script(CASCADE)
        self.assertEqual(result.returncode, 0, result.stderr)
        compose = self.compose_lines()
        self.assertTrue(
            any("restart radarr prowlarr qbittorrent byparr" in l
                for l in compose), compose)
        self.assertTrue(
            any("up -d radarr prowlarr qbittorrent byparr" in l
                for l in compose), compose)
        # No enforcement stop when nothing is paused.
        self.assertFalse(
            any(l.startswith("stop") for l in self.log_lines()))

    def test_marker_restricts_cascade_to_qbittorrent(self):
        MARKER.write_text("2026-07-31T00:00:00Z\n")
        result = self.run_script(CASCADE)
        self.assertEqual(result.returncode, 0, result.stderr)
        compose = self.compose_lines()
        self.assertTrue(
            any("restart qbittorrent" in l for l in compose), compose)
        self.assertTrue(
            any("up -d qbittorrent" in l for l in compose), compose)
        # The paused three must appear in NO compose action.
        for svc in ("radarr", "prowlarr", "byparr"):
            self.assertFalse(
                any(svc in l for l in compose),
                f"{svc} re-linked despite pause marker: {compose}")

    def test_marker_enforces_stop_of_paused_three(self):
        MARKER.write_text("2026-07-31T00:00:00Z\n")
        result = self.run_script(CASCADE)
        self.assertEqual(result.returncode, 0, result.stderr)
        stops = [l for l in self.log_lines() if l.startswith("stop")]
        self.assertEqual(len(stops), 1, self.log_lines())
        for name in ("mdb_radarr", "mdb_prowlarr", "mdb_byparr"):
            self.assertIn(name, stops[0])
        self.assertNotIn("mdb_qbittorrent", stops[0])


class UnpauseFallbackTests(StubDockerTestCase):
    def test_failed_start_falls_back_to_compose_up(self):
        result = self.run_script(
            PAUSE, "unpause", extra_env={"INSPECT_RUNNING": "false"})
        self.assertEqual(result.returncode, 0, result.stderr)
        compose = self.compose_lines()
        self.assertTrue(
            any("up -d radarr prowlarr byparr" in l for l in compose),
            f"no compose fallback despite failed starts: {self.log_lines()}")
        # The failure WARN must survive for journal grep-ability.
        self.assertIn("WARN: failed to bring", result.stderr)

    def test_successful_start_never_touches_compose(self):
        result = self.run_script(
            PAUSE, "unpause", extra_env={"INSPECT_RUNNING": "true"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.compose_lines(), [],
                         "compose fallback fired on healthy starts")
        self.assertNotIn("WARN", result.stderr)

    def test_pause_branch_unaffected_by_fallback(self):
        result = self.run_script(PAUSE, "pause")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.compose_lines(), [])
        self.assertTrue(MARKER.exists())


if __name__ == "__main__":
    unittest.main()
