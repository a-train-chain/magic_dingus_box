"""Cross-site requests are refused using the browser's Fetch Metadata.

The Host allowlist (DNS-rebinding defence) has to accept IP literals — the
pairing QR and the Connect screen use the LAN IP — so a page on ANY website
could still make the visitor's browser hit http://<box-ip>:5000/...: spawn an
`update.sh check` (and burn the shared GitHub rate limit), mint CSRF tokens
into an unbounded dict, or spend the pairing attempt budget with
/?pair=000000. Browsers stamp every request with Sec-Fetch-Site, which a page
cannot forge, so: refuse 'cross-site'. Zero friction —
  * header absent (curl, Retro Ripper, older Safari) -> allowed
  * 'none' (camera-app QR scan, typed URL, home-screen app launch) -> allowed
  * same-origin / same-site -> allowed
  * a cross-site TOP-LEVEL navigation to a page (a link from a router's
    device list, a help article) still opens it; one that carries a pairing
    code lands on the Connect page instead of spending an attempt.
"""
from __future__ import annotations

import json
import threading
import time
from pathlib import Path

import pytest

import admin
import admin_security  # owns the CSRF token store (patch it there, not on admin)
from admin import create_app

XSITE_FETCH = {"Sec-Fetch-Site": "cross-site", "Sec-Fetch-Mode": "cors",
               "Sec-Fetch-Dest": "empty"}
XSITE_NAV = {"Sec-Fetch-Site": "cross-site", "Sec-Fetch-Mode": "navigate",
             "Sec-Fetch-Dest": "document"}


@pytest.fixture
def counting_update_script(temp_data_dir: Path):
    marker = temp_data_dir.parent / "check_runs"
    script = temp_data_dir.parent / "scripts" / "update.sh"
    script.write_text(f"""#!/bin/bash
echo run >> "{marker}"
echo '{{"ok": true, "data": {{"current_version": "1.0.7", "latest_version": "1.0.8", "update_available": true}}}}'
""")
    script.chmod(0o755)
    return marker


def _runs(marker: Path) -> int:
    return len(marker.read_text().splitlines()) if marker.exists() else 0


def test_cross_site_fetch_of_update_check_is_refused_without_spawning(
        client, counting_update_script):
    rv = client.get("/admin/update/check", headers=XSITE_FETCH)
    assert rv.status_code == 403
    assert rv.get_json()["error"]["code"] == "CROSS_SITE_REQUEST"
    assert _runs(counting_update_script) == 0


@pytest.mark.parametrize("headers", [
    {},
    {"Sec-Fetch-Site": "same-origin", "Sec-Fetch-Mode": "cors"},
    {"Sec-Fetch-Site": "same-site", "Sec-Fetch-Mode": "cors"},
    {"Sec-Fetch-Site": "none", "Sec-Fetch-Mode": "navigate", "Sec-Fetch-Dest": "document"},
], ids=["absent", "same-origin", "same-site", "none"])
def test_non_cross_site_requests_pass(client, counting_update_script, headers):
    assert client.get("/admin/csrf-token", headers=headers).status_code == 200
    assert client.get("/admin/update/check", headers=headers).status_code == 200


def test_cross_site_csrf_token_and_post_refused(client):
    assert client.get("/admin/csrf-token", headers=XSITE_FETCH).status_code == 403
    # CSRF is disabled in the test app, so this proves the gate itself.
    rv = client.post("/admin/update/rollback", headers=XSITE_FETCH)
    assert rv.status_code == 403


def test_cross_site_iframe_of_the_ui_is_refused(client):
    rv = client.get("/", headers={"Sec-Fetch-Site": "cross-site",
                                  "Sec-Fetch-Mode": "navigate",
                                  "Sec-Fetch-Dest": "iframe"})
    assert rv.status_code == 403


def test_cross_site_link_to_the_box_still_opens_it(client):
    rv = client.get("/", headers=XSITE_NAV)
    assert rv.status_code == 200


# ---- pairing ---------------------------------------------------------------

@pytest.fixture
def pair_app(tmp_path):
    app = create_app(data_dir=tmp_path)
    app.config["TESTING"] = True
    app.config["SECRET_KEY"] = "test-secret-key"
    return app


def _session(tmp_path: Path, attempts=5):
    p = tmp_path / "pairing_session.json"
    p.write_text(json.dumps({
        "schema": 1, "code": "847291", "issued_at": int(time.time()),
        "expires_at": int(time.time()) + 120, "attempts_remaining": attempts,
        "nonce": "abc123" * 8}))
    return p


def test_cross_site_pair_attempt_does_not_spend_the_budget(pair_app, tmp_path):
    p = _session(tmp_path)
    rv = pair_app.test_client().get("/?pair=000000", headers=XSITE_NAV)
    assert rv.status_code in (302, 303)
    assert rv.headers["Location"].endswith("/connect?code=000000")
    assert json.loads(p.read_text())["attempts_remaining"] == 5

    # And a cross-site subresource/fetch never reaches the pairing code.
    rv = pair_app.test_client().get("/?pair=000000", headers=XSITE_FETCH)
    assert rv.status_code == 403
    assert json.loads(p.read_text())["attempts_remaining"] == 5


def test_qr_scan_pairing_still_works(pair_app, tmp_path):
    """Camera-app QR scans and home-screen launches arrive as 'none'."""
    _session(tmp_path)
    client = pair_app.test_client()
    rv = client.get("/connect?code=847291",
                    headers={"Sec-Fetch-Site": "none", "Sec-Fetch-Mode": "navigate",
                             "Sec-Fetch-Dest": "document"})
    assert rv.status_code == 200
    rv = client.get("/?pair=847291&tab=remote", follow_redirects=False,
                    headers={"Sec-Fetch-Site": "same-origin",
                             "Sec-Fetch-Mode": "navigate", "Sec-Fetch-Dest": "document"})
    assert rv.status_code in (302, 303)
    assert "mdb_remote" in rv.headers.get("Set-Cookie", "")


def test_direct_pair_link_with_sec_fetch_none_pairs(pair_app, tmp_path):
    _session(tmp_path)
    rv = pair_app.test_client().get(
        "/?pair=847291&tab=remote", follow_redirects=False,
        headers={"Sec-Fetch-Site": "none", "Sec-Fetch-Mode": "navigate",
                 "Sec-Fetch-Dest": "document"})
    assert rv.status_code in (302, 303)
    assert "mdb_remote" in rv.headers.get("Set-Cookie", "")


# ---- CSRF token store ------------------------------------------------------

def test_csrf_token_store_is_capped(monkeypatch):
    monkeypatch.setattr(admin_security, "_csrf_tokens", {})
    for _ in range(admin._CSRF_TOKEN_MAX + 50):
        admin._generate_csrf_token()
    assert len(admin_security._csrf_tokens) <= admin._CSRF_TOKEN_MAX
    newest = admin._generate_csrf_token()
    assert admin._validate_csrf_token(newest)


def test_csrf_token_store_is_thread_safe(monkeypatch):
    monkeypatch.setattr(admin_security, "_csrf_tokens", {})
    errors = []

    def worker():
        try:
            for _ in range(300):
                t = admin._generate_csrf_token()
                admin._validate_csrf_token(t)
        except Exception as e:  # "dictionary changed size during iteration"
            errors.append(e)

    threads = [threading.Thread(target=worker) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert errors == []


# ---- update check single-flight + cache ------------------------------------

def test_update_check_is_cached_briefly(client, counting_update_script):
    assert client.get("/admin/update/check").status_code == 200
    assert client.get("/admin/update/check").status_code == 200
    assert _runs(counting_update_script) == 1


def test_concurrent_update_checks_share_one_run(app, counting_update_script):
    script = counting_update_script.parent / "scripts" / "update.sh"
    body = script.read_text().replace("echo run", "sleep 0.5; echo run")
    script.write_text(body)
    results = []

    def hit():
        results.append(app.test_client().get("/admin/update/check").status_code)

    threads = [threading.Thread(target=hit) for _ in range(5)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert results == [200] * 5
    assert _runs(counting_update_script) == 1
