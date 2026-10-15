"""
clear_radarr_cooldowns.py — boot-time reset of *arr indexer cooldowns.

Contract under test:
  * Cooldown rows are reset in BOTH radarr.db and sonarr.db (same
    IndexerStatus shape); a DB whose table lacks the cooldown columns is
    never written to.
  * Clean tables → nothing is stopped or started.
  * The container restart is try/finally: an exception between stop and
    start must not leave Radarr stopped until the next boot (it did).
  * A container that was not running (stopped for playback) is cleaned
    but never started.

Stdlib only (the CI job has no pip step).
"""
import importlib.util
import sqlite3
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPTS_DIR = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location(
    "clear_cooldowns", SCRIPTS_DIR / "clear_radarr_cooldowns.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

SCHEMA = """
CREATE TABLE IndexerStatus (
    Id INTEGER PRIMARY KEY, ProviderId INTEGER, InitialFailure DATETIME,
    MostRecentFailure DATETIME, EscalationLevel INTEGER NOT NULL,
    DisabledTill DATETIME);
"""


def make_db(path, rows):
    con = sqlite3.connect(path)
    con.executescript(SCHEMA)
    con.executemany(
        "INSERT INTO IndexerStatus (ProviderId, InitialFailure, "
        "MostRecentFailure, EscalationLevel, DisabledTill) VALUES (?,?,?,?,?)",
        rows)
    con.commit()
    con.close()


COOLED = (1, "2026-10-01", "2026-10-01", 3, "2099-01-01")
CLEAN = (2, None, None, 0, None)


class ClearCooldownsTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        tmp = Path(self._tmp.name)
        self.radarr_db = tmp / "radarr.db"
        self.sonarr_db = tmp / "sonarr.db"
        self.docker_calls = []
        self.running = {"mdb_radarr": True, "mdb_sonarr": True}
        patches = [
            mock.patch.object(mod, "APPS", [
                ("Radarr", "mdb_radarr", self.radarr_db, "http://r/ping"),
                ("Sonarr", "mdb_sonarr", self.sonarr_db, "http://s/ping"),
            ]),
            mock.patch.object(mod, "COMPOSE_LOCK", tmp / "compose.lock"),
            mock.patch.object(mod, "PAUSE_MARKER", tmp / "paused"),
            mock.patch.object(mod, "app_ready", lambda url: True),
            mock.patch.object(mod, "container_running",
                              lambda name: self.running.get(name, False)),
            mock.patch.object(mod, "docker",
                              lambda action, name: self.docker_calls.append(
                                  (action, name))),
            mock.patch.object(mod.time, "sleep", lambda s: None),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def cooled_count(self, db):
        con = sqlite3.connect(db)
        try:
            return con.execute(
                "SELECT COUNT(*) FROM IndexerStatus WHERE DisabledTill IS "
                "NOT NULL OR EscalationLevel > 0").fetchone()[0]
        finally:
            con.close()

    def test_clean_tables_touch_nothing(self):
        make_db(self.radarr_db, [CLEAN])
        make_db(self.sonarr_db, [CLEAN])
        self.assertEqual(mod.main(), 0)
        self.assertEqual(self.docker_calls, [])

    def test_sonarr_cooldowns_are_cleared_too(self):
        make_db(self.radarr_db, [CLEAN])
        make_db(self.sonarr_db, [COOLED, COOLED])
        self.assertEqual(mod.main(), 0)
        self.assertEqual(self.cooled_count(self.sonarr_db), 0)
        # Only Sonarr was bounced; Radarr had nothing to clear.
        self.assertEqual(self.docker_calls,
                         [("stop", "mdb_sonarr"), ("start", "mdb_sonarr")])

    def test_both_apps_cleared(self):
        make_db(self.radarr_db, [COOLED])
        make_db(self.sonarr_db, [COOLED])
        mod.main()
        self.assertEqual(self.cooled_count(self.radarr_db), 0)
        self.assertEqual(self.cooled_count(self.sonarr_db), 0)

    def test_missing_sonarr_db_is_fine(self):
        make_db(self.radarr_db, [COOLED])
        self.assertEqual(mod.main(), 0)
        self.assertEqual(self.cooled_count(self.radarr_db), 0)

    def test_exception_mid_clear_still_restarts_container(self):
        make_db(self.radarr_db, [COOLED])
        with mock.patch.object(mod, "clear_cooldowns",
                               side_effect=sqlite3.OperationalError("locked")):
            self.assertEqual(mod.main(), 0)
        self.assertEqual(self.docker_calls,
                         [("stop", "mdb_radarr"), ("start", "mdb_radarr")])

    def test_stopped_for_playback_is_cleared_but_not_started(self):
        make_db(self.radarr_db, [COOLED])
        self.running["mdb_radarr"] = False
        mod.PAUSE_MARKER.write_text("x")
        mod.main()
        self.assertEqual(self.cooled_count(self.radarr_db), 0)
        self.assertEqual(self.docker_calls, [])

    def test_unknown_schema_is_never_written(self):
        con = sqlite3.connect(self.sonarr_db)
        con.executescript(
            "CREATE TABLE IndexerStatus (Id INTEGER PRIMARY KEY, "
            "DisabledTill DATETIME);"
            "INSERT INTO IndexerStatus (DisabledTill) VALUES ('2099-01-01');")
        con.commit()
        con.close()
        self.assertEqual(mod.count_active_cooldowns(self.sonarr_db), 0)
        self.assertEqual(mod.clear_cooldowns(self.sonarr_db), 0)
        con = sqlite3.connect(self.sonarr_db)
        self.assertEqual(con.execute(
            "SELECT DisabledTill FROM IndexerStatus").fetchone()[0],
            "2099-01-01")
        con.close()


if __name__ == "__main__":
    unittest.main()
