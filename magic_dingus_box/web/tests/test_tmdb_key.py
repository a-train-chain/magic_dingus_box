"""Tests for the customer-supplied TMDB API key.

Context: first_boot.sh wipes the developer's personal TMDB key from every
cloned unit, and until this feature existed there was no way to supply a
replacement short of SSH — so every shipped box had a permanently empty
Media Browser. These tests cover the endpoint that closes that gap.

SECURITY: every key in this file is synthetic. A real key must never be
committed as a fixture — a real WireGuard private key was committed to this
repo earlier and had to be revoked. "0" * 32 is a well-formed v3 key that
belongs to nobody.
"""
from __future__ import annotations

import json
import os
import shutil
import stat
import tempfile
from pathlib import Path

import pytest

import admin


# A syntactically valid v3 key that is not, and never will be, real.
FAKE_V3_KEY = "0" * 32
OTHER_FAKE_V3_KEY = "a1b2c3d4" * 4  # 32 hex chars, also synthetic
# Shape-accurate v4 Read Access Token. Payload decodes to {"sub":"test"}.
FAKE_V4_TOKEN = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ0ZXN0In0.c2lnbmF0dXJlLXBsYWNlaG9sZGVy"

# Resolved at import, before any app exists. create_app() repoints
# tempfile.tempdir at its own upload_temp directory, so pytest's tmp_path
# fixture lands inside a tree the app fixture later deletes. Pinning to the
# real system temp keeps these scratch paths independent of that.
_SYSTEM_TMP = tempfile.gettempdir()


@pytest.fixture
def scratch():
    """A disposable directory that survives the app fixture's teardown."""
    base = tempfile.mkdtemp(dir=_SYSTEM_TMP, prefix="mdb-tmdb-test-")
    try:
        yield Path(base)
    finally:
        shutil.rmtree(base, ignore_errors=True)


@pytest.fixture
def key_file(scratch: Path, monkeypatch) -> Path:
    """Point the key writer at a disposable path.

    Without this the app would write into the real
    ~/.config/magic_dingus_box/tmdb_api_key of whoever runs the suite.
    """
    path = scratch / "config" / "magic_dingus_box" / "tmdb_api_key"
    monkeypatch.setenv(admin._TMDB_KEY_FILE_ENV, str(path))
    return path


@pytest.fixture
def unlocked(scratch: Path, monkeypatch):
    """Satisfy Layer 1 of the Media Browser gate (kiosk unlock flag)."""
    settings = scratch / "settings.json"
    settings.write_text(json.dumps({"playback": {"media_browser_unlocked": True}}))
    monkeypatch.setattr(admin, "MEDIA_BROWSER_SETTINGS_PATH", str(settings))


@pytest.fixture
def no_systemd(monkeypatch):
    """Deterministic kiosk state — the test host has no kiosk unit."""
    monkeypatch.setattr(admin, "check_service_status", lambda name: "inactive")
    monkeypatch.setattr(admin, "_kiosk_started_at", lambda: None)


@pytest.fixture
def verify_ok(monkeypatch):
    """TMDB accepts whatever key it is given. No network in tests."""
    monkeypatch.setattr(admin, "_tmdb_verify_key", lambda key, timeout=10.0: ("valid", ""))


def _post(client, payload):
    return client.post(
        "/admin/media-browser/tmdb",
        data=json.dumps(payload),
        content_type="application/json",
    )


# ===== Key classification (pure, no HTTP) =====

class TestClassifyKey:
    def test_v3_key_accepted(self):
        assert admin._tmdb_classify_key(FAKE_V3_KEY) == ("v3", FAKE_V3_KEY)

    def test_surrounding_whitespace_is_stripped(self):
        """Pasting from a browser routinely drags in a trailing newline."""
        assert admin._tmdb_classify_key(f"  {FAKE_V3_KEY}\n ") == ("v3", FAKE_V3_KEY)

    def test_uppercase_hex_accepted(self):
        key = "ABCDEF01" * 4
        assert admin._tmdb_classify_key(key)[0] == "v3"

    def test_v4_token_identified_separately(self):
        """Not merely 'invalid' — it needs its own explanation to the user."""
        assert admin._tmdb_classify_key(FAKE_V4_TOKEN)[0] == "v4"

    @pytest.mark.parametrize("bad", [
        "",
        "   ",
        "0" * 31,              # one short
        "0" * 33,              # one long
        "g" * 32,              # right length, not hex
        "0123456789abcdef 0123456789abcde",  # embedded space
        "not-a-key",
    ])
    def test_malformed_rejected(self, bad):
        assert admin._tmdb_classify_key(bad)[0] in ("invalid", "empty")


class TestRedaction:
    def test_key_stripped_from_messages(self):
        msg = f"failed for https://api.themoviedb.org/3/x?api_key={FAKE_V3_KEY}"
        out = admin._tmdb_redact(msg, FAKE_V3_KEY)
        assert FAKE_V3_KEY not in out
        assert "<redacted>" in out


# ===== Save endpoint =====

class TestSaveKey:
    def test_valid_key_accepted_and_written(self, client, key_file, unlocked,
                                            no_systemd, verify_ok):
        resp = _post(client, {"api_key": FAKE_V3_KEY})
        assert resp.status_code == 200, resp.get_data(as_text=True)
        body = resp.get_json()
        assert body["ok"] is True
        assert body["data"]["configured"] is True
        assert body["data"]["verified"] is True
        # The file the kiosk reads (main.cpp) now holds the key.
        assert key_file.read_text().strip() == FAKE_V3_KEY

    def test_written_file_is_0600(self, client, key_file, unlocked, no_systemd,
                                  verify_ok):
        """Both the kiosk and the web app run as `magic`; nobody else needs it."""
        _post(client, {"api_key": FAKE_V3_KEY})
        mode = stat.S_IMODE(key_file.stat().st_mode)
        assert mode == 0o600, f"expected 0600, got {oct(mode)}"

    def test_malformed_key_rejected(self, client, key_file, unlocked, no_systemd,
                                    verify_ok):
        resp = _post(client, {"api_key": "nope"})
        assert resp.status_code == 400
        assert resp.get_json()["error"]["code"] == "invalid_key_format"
        assert not key_file.exists()

    def test_v4_token_rejected_with_specific_guidance(self, client, key_file,
                                                      unlocked, no_systemd,
                                                      verify_ok):
        """TmdbClient only sends ?api_key=, so a Bearer token cannot work.

        Accepting it would produce a 401 on every kiosk call — the exact
        silent blank-Browse failure this feature exists to prevent.
        """
        resp = _post(client, {"api_key": FAKE_V4_TOKEN})
        assert resp.status_code == 400
        assert resp.get_json()["error"]["code"] == "unsupported_key_type"
        assert not key_file.exists()

    def test_empty_key_rejected(self, client, key_file, unlocked, no_systemd,
                                verify_ok):
        resp = _post(client, {"api_key": "   "})
        assert resp.status_code == 400
        assert not key_file.exists()

    def test_key_rejected_by_tmdb_is_not_saved(self, client, key_file, unlocked,
                                               no_systemd, monkeypatch):
        monkeypatch.setattr(
            admin, "_tmdb_verify_key",
            lambda key, timeout=10.0: ("invalid", "Invalid API key"))
        resp = _post(client, {"api_key": FAKE_V3_KEY})
        assert resp.status_code == 400
        assert resp.get_json()["error"]["code"] == "key_rejected"
        assert not key_file.exists()


class TestExistingKeyNotClobbered:
    """A failed save must leave a working key exactly as it was.

    This is the regression that matters most in the field: a customer with a
    working box mistypes a replacement key, the save fails, and Media Browser
    must keep working.
    """

    def _seed(self, key_file: Path) -> None:
        key_file.parent.mkdir(parents=True, exist_ok=True)
        key_file.write_text(OTHER_FAKE_V3_KEY + "\n")
        os.chmod(key_file, 0o600)

    def test_malformed_input_leaves_existing_key(self, client, key_file, unlocked,
                                                 no_systemd, verify_ok):
        self._seed(key_file)
        resp = _post(client, {"api_key": "garbage"})
        assert resp.status_code == 400
        assert key_file.read_text().strip() == OTHER_FAKE_V3_KEY

    def test_tmdb_rejection_leaves_existing_key(self, client, key_file, unlocked,
                                                no_systemd, monkeypatch):
        self._seed(key_file)
        monkeypatch.setattr(
            admin, "_tmdb_verify_key",
            lambda key, timeout=10.0: ("invalid", "Invalid API key"))
        resp = _post(client, {"api_key": FAKE_V3_KEY})
        assert resp.status_code == 400
        assert key_file.read_text().strip() == OTHER_FAKE_V3_KEY

    def test_unreachable_tmdb_leaves_existing_key(self, client, key_file, unlocked,
                                                  no_systemd, monkeypatch):
        self._seed(key_file)
        monkeypatch.setattr(
            admin, "_tmdb_verify_key",
            lambda key, timeout=10.0: ("unreachable", "no dns"))
        resp = _post(client, {"api_key": FAKE_V3_KEY})
        assert resp.status_code == 503
        assert key_file.read_text().strip() == OTHER_FAKE_V3_KEY

    def test_permissions_preserved_after_overwrite(self, client, key_file, unlocked,
                                                   no_systemd, verify_ok):
        self._seed(key_file)
        _post(client, {"api_key": FAKE_V3_KEY})
        assert stat.S_IMODE(key_file.stat().st_mode) == 0o600
        assert key_file.read_text().strip() == FAKE_V3_KEY


class TestOfflineHandling:
    """No internet must not be treated as 'bad key', and must not save blind."""

    def test_unreachable_refuses_by_default(self, client, key_file, unlocked,
                                            no_systemd, monkeypatch):
        monkeypatch.setattr(
            admin, "_tmdb_verify_key",
            lambda key, timeout=10.0: ("unreachable", "Could not reach TMDB"))
        resp = _post(client, {"api_key": FAKE_V3_KEY})
        assert resp.status_code == 503
        assert resp.get_json()["error"]["code"] == "tmdb_unreachable"
        assert not key_file.exists()

    def test_allow_unverified_saves_with_a_warning(self, client, key_file, unlocked,
                                                   no_systemd, monkeypatch):
        monkeypatch.setattr(
            admin, "_tmdb_verify_key",
            lambda key, timeout=10.0: ("unreachable", "Could not reach TMDB"))
        resp = _post(client, {"api_key": FAKE_V3_KEY, "allow_unverified": True})
        assert resp.status_code == 200
        data = resp.get_json()["data"]
        assert data["verified"] is False
        assert data["verify_warning"]
        assert key_file.read_text().strip() == FAKE_V3_KEY


class TestVerifyKeyNetworkFailures:
    """_tmdb_verify_key must never raise, and never echo the key."""

    def test_network_error_reports_unreachable_without_key(self, monkeypatch):
        import urllib.request

        def boom(*a, **kw):
            raise OSError(f"connect failed for api_key={FAKE_V3_KEY}")

        monkeypatch.setattr(urllib.request, "urlopen", boom)
        result, detail = admin._tmdb_verify_key(FAKE_V3_KEY, timeout=0.1)
        assert result == "unreachable"
        assert FAKE_V3_KEY not in detail


# ===== Status readout =====

class TestStatusReadout:
    def test_reports_not_configured_when_no_file(self, client, key_file, unlocked,
                                                 no_systemd):
        resp = client.get("/admin/media-browser/tmdb")
        assert resp.status_code == 200
        data = resp.get_json()["data"]
        assert data["configured"] is False
        assert data["key_length"] == 0

    def test_masked_readout_never_returns_the_key(self, client, key_file, unlocked,
                                                  no_systemd, verify_ok):
        _post(client, {"api_key": FAKE_V3_KEY})
        resp = client.get("/admin/media-browser/tmdb")
        raw = resp.get_data(as_text=True)
        assert FAKE_V3_KEY not in raw, "the key itself must never reach the client"
        data = resp.get_json()["data"]
        assert data["configured"] is True
        # Only the LENGTH is exposed, so the UI can size the dot mask.
        assert data["key_length"] == 32
        assert data["signup_url"].startswith("https://www.themoviedb.org/")

    def test_save_response_never_echoes_the_key(self, client, key_file, unlocked,
                                                no_systemd, verify_ok):
        resp = _post(client, {"api_key": FAKE_V3_KEY})
        assert FAKE_V3_KEY not in resp.get_data(as_text=True)

    def test_empty_file_counts_as_not_configured(self, client, key_file, unlocked,
                                                 no_systemd):
        """first_boot.sh removes the file, but a truncated one must not pass."""
        key_file.parent.mkdir(parents=True, exist_ok=True)
        key_file.write_text("\n")
        data = client.get("/admin/media-browser/tmdb").get_json()["data"]
        assert data["configured"] is False

    def test_kiosk_reported_stale_when_started_before_the_key(
            self, client, key_file, unlocked, monkeypatch):
        """The whole point: a running kiosk keeps the key it booted with."""
        monkeypatch.setattr(admin, "check_service_status", lambda name: "active")
        monkeypatch.setattr(admin, "_kiosk_started_at", lambda: 1000.0)
        key_file.parent.mkdir(parents=True, exist_ok=True)
        key_file.write_text(FAKE_V3_KEY + "\n")
        os.utime(key_file, (2000.0, 2000.0))  # key written AFTER kiosk start

        data = client.get("/admin/media-browser/tmdb").get_json()["data"]
        assert data["kiosk_service_active"] is True
        assert data["kiosk_has_current_key"] is False

    def test_kiosk_reported_current_when_started_after_the_key(
            self, client, key_file, unlocked, monkeypatch):
        monkeypatch.setattr(admin, "check_service_status", lambda name: "active")
        monkeypatch.setattr(admin, "_kiosk_started_at", lambda: 3000.0)
        key_file.parent.mkdir(parents=True, exist_ok=True)
        key_file.write_text(FAKE_V3_KEY + "\n")
        os.utime(key_file, (2000.0, 2000.0))  # kiosk restarted since

        data = client.get("/admin/media-browser/tmdb").get_json()["data"]
        assert data["kiosk_has_current_key"] is True

    def test_save_flags_restart_required_while_kiosk_runs(
            self, client, key_file, unlocked, verify_ok, monkeypatch):
        monkeypatch.setattr(admin, "check_service_status", lambda name: "active")
        monkeypatch.setattr(admin, "_kiosk_started_at", lambda: 1000.0)
        data = _post(client, {"api_key": FAKE_V3_KEY}).get_json()["data"]
        assert data["kiosk_restart_required"] is True

    def test_no_restart_prompt_when_kiosk_is_not_running(
            self, client, key_file, unlocked, no_systemd, verify_ok):
        """A stopped kiosk will read the key when it next starts."""
        data = _post(client, {"api_key": FAKE_V3_KEY}).get_json()["data"]
        assert data["kiosk_restart_required"] is False


# ===== Gating =====

class TestGating:
    def test_status_403_when_media_browser_locked(self, client, key_file, scratch,
                                                  monkeypatch):
        settings = scratch / "locked.json"
        settings.write_text(json.dumps({"playback": {"media_browser_unlocked": False}}))
        monkeypatch.setattr(admin, "MEDIA_BROWSER_SETTINGS_PATH", str(settings))
        resp = client.get("/admin/media-browser/tmdb")
        assert resp.status_code == 403
        assert resp.get_json()["error"]["code"] == "media_browser_locked"

    def test_save_403_when_media_browser_locked(self, client, key_file, scratch,
                                                monkeypatch):
        settings = scratch / "locked.json"
        settings.write_text(json.dumps({"playback": {"media_browser_unlocked": False}}))
        monkeypatch.setattr(admin, "MEDIA_BROWSER_SETTINGS_PATH", str(settings))
        resp = _post(client, {"api_key": FAKE_V3_KEY})
        assert resp.status_code == 403
        assert not key_file.exists()

    def test_reachable_without_vpn_configured(self, client, key_file, unlocked,
                                              no_systemd, verify_ok):
        """TMDB traffic exits via the host, not Gluetun — no VPN gate.

        The customer should be able to set the key before, during or after
        VPN setup.
        """
        resp = _post(client, {"api_key": FAKE_V3_KEY})
        assert resp.status_code == 200
