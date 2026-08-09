"""iOS home-screen install pairing: dynamic manifest + durable device token.

An installed home-screen web app has a SEPARATE cookie jar from Safari, so
the pairing cookie Safari holds never reaches the installed app. The fix
under test: the /admin/remote manifest is dynamic — served to a validly
paired cookie it embeds a per-device install token in start_url; the
installed app redeems that token on first launch for its own cookie.

Security contract exercised here:
  - unauthenticated manifest carries NO token;
  - the token redeems ONLY on GET /admin/remote (no other endpoint);
  - revocation (pending_revocations.txt) kills the token;
  - the token value never appears in logs or at rest.
"""
import json
import logging
import time
from pathlib import Path

import pytest

from admin import create_app


@pytest.fixture
def app(tmp_path):
    app = create_app(data_dir=tmp_path)
    app.config["TESTING"] = True
    app.config["SECRET_KEY"] = "test-secret-key"
    return app


@pytest.fixture
def client(app):
    return app.test_client()


def write_session(tmp_path: Path, code="847291", attempts=5, expires_in=120):
    payload = {
        "schema": 1,
        "code": code,
        "issued_at": int(time.time()),
        "expires_at": int(time.time()) + expires_in,
        "attempts_remaining": attempts,
        "nonce": "abc123" * 8,
    }
    (tmp_path / "pairing_session.json").write_text(json.dumps(payload))


def pair(client, tmp_path, code="847291"):
    """Run the QR pairing flow; install the cookie on the client.
    Returns (device_id, cookie_value)."""
    write_session(tmp_path, code=code)
    rv = client.get(f"/?pair={code}&tab=remote", follow_redirects=False)
    assert rv.status_code in (302, 303)
    cookie = rv.headers["Set-Cookie"].split(";")[0]
    name, value = cookie.split("=", 1)
    client.set_cookie(domain="localhost", key=name, value=value)
    paired = json.loads((tmp_path / "paired_remotes.json").read_text())
    return paired["devices"][0]["id"], value


def fetch_manifest(client, path="/admin/remote/manifest.webmanifest"):
    rv = client.get(path)
    assert rv.status_code == 200
    return rv, json.loads(rv.data)


def token_from_start_url(start_url: str, base="/admin/remote") -> str:
    assert start_url.startswith(f"{base}?device_token=")
    return start_url.split("device_token=", 1)[1]


# ---------------------------------------------------------------------------
# (a) Unauthenticated manifest: correct shape, no token.
# ---------------------------------------------------------------------------

def test_manifest_unauthenticated_has_no_token(client):
    rv, manifest = fetch_manifest(client)
    assert rv.mimetype == "application/manifest+json"
    assert rv.headers.get("Cache-Control") == "no-store"
    assert manifest["start_url"] == "/admin/remote"
    assert "device_token" not in manifest["start_url"]
    assert manifest["scope"] == "/admin/remote"
    assert manifest["display"] == "standalone"
    assert manifest["name"] == "Dingus Remote"
    assert manifest["short_name"] == "Dingus Remote"
    assert len(manifest["icons"]) == 4


# ---------------------------------------------------------------------------
# (b) Paired manifest carries a token that redeems into a cookie + 303.
# ---------------------------------------------------------------------------

def test_manifest_with_cookie_carries_token_that_redeems(app, client, tmp_path):
    device_id, _ = pair(client, tmp_path)
    rv, manifest = fetch_manifest(client)
    assert rv.headers.get("Cache-Control") == "no-store"
    token = token_from_start_url(manifest["start_url"])
    assert len(token) >= 32  # presence proved by length only — never log it

    # Stability: iOS may fetch the manifest again between install and first
    # launch; a rotated token would strand the installed icon. Same token
    # on a second fetch is load-bearing, not incidental.
    _, manifest2 = fetch_manifest(client)
    assert manifest2["start_url"] == manifest["start_url"]

    # First launch of the installed app: SEPARATE jar → fresh client.
    installed = app.test_client()
    rv2 = installed.get(f"/admin/remote?device_token={token}",
                        follow_redirects=False)
    assert rv2.status_code == 303
    assert rv2.headers["Location"].endswith("/admin/remote")
    set_cookie = rv2.headers.get("Set-Cookie", "")
    assert "mdb_remote=" in set_cookie
    # The redirect target must serve the real remote shell with that cookie.
    cookie = set_cookie.split(";")[0]
    name, value = cookie.split("=", 1)
    installed.set_cookie(domain="localhost", key=name, value=value)
    rv3 = installed.get("/admin/remote")
    assert rv3.status_code == 200
    assert b"remote.js" in rv3.data
    # And the cookie authenticates as the SAME device that installed.
    rv4 = installed.get("/admin/remote/protected_check")
    assert rv4.status_code == 200
    assert json.loads(rv4.data)["data"]["device_id"] == device_id


# ---------------------------------------------------------------------------
# (c) Garbage / revoked tokens: pair page, no cookie, no validity leak.
# ---------------------------------------------------------------------------

def test_garbage_token_falls_through_to_pair_page(client):
    rv = client.get("/admin/remote?device_token=not-a-real-token",
                    follow_redirects=False)
    assert rv.status_code == 200
    assert b"Pair this remote" in rv.data
    assert "mdb_remote" not in rv.headers.get("Set-Cookie", "")


def test_revoked_device_token_no_longer_redeems(app, client, tmp_path):
    device_id, _ = pair(client, tmp_path)
    _, manifest = fetch_manifest(client)
    token = token_from_start_url(manifest["start_url"])

    # Kiosk-side unpair: the revocation queue, exactly as the kiosk writes
    # it. Redemption must honor it even before the broadcaster's reap tick.
    (tmp_path / "pending_revocations.txt").write_text(device_id + "\n")

    installed = app.test_client()
    rv = installed.get(f"/admin/remote?device_token={token}",
                       follow_redirects=False)
    assert rv.status_code == 200
    assert b"Pair this remote" in rv.data
    assert "mdb_remote" not in rv.headers.get("Set-Cookie", "")
    # The record is gone — token death rode the revocation.
    paired = json.loads((tmp_path / "paired_remotes.json").read_text())
    assert paired["devices"] == []


# ---------------------------------------------------------------------------
# (d) The token value never lands in logs or at rest.
# ---------------------------------------------------------------------------

def test_token_absent_from_logs_and_rest(app, client, tmp_path, caplog):
    caplog.set_level(logging.DEBUG)
    _device_id, _ = pair(client, tmp_path)
    _, manifest = fetch_manifest(client)
    token = token_from_start_url(manifest["start_url"])

    installed = app.test_client()
    rv = installed.get(f"/admin/remote?device_token={token}",
                       follow_redirects=False)
    assert rv.status_code == 303
    # Root-route redemption (the installed Content Manager app path).
    installed_cm = app.test_client()
    rv_root = installed_cm.get(f"/?device_token={token}",
                               follow_redirects=False)
    assert rv_root.status_code == 303
    # A failed attempt must not echo it either.
    installed2 = app.test_client()
    installed2.get(f"/admin/remote?device_token={token[:-4]}XXXX",
                   follow_redirects=False)

    assert token not in caplog.text
    audit = (tmp_path / "pairing_audit.log")
    if audit.exists():
        audit_text = audit.read_text()
        assert token not in audit_text
        # Redemption IS audited (outcome only, code masked).
        assert "token_redeemed" in audit_text
        assert "token_rejected" in audit_text
    # Hashed/derived at rest: the bearer value must not sit in the store.
    assert token not in (tmp_path / "paired_remotes.json").read_text()


# ---------------------------------------------------------------------------
# Scope limitation: the token redeems ONLY on GET /admin/remote.
# ---------------------------------------------------------------------------

def test_token_does_not_authenticate_other_endpoints(app, client, tmp_path):
    pair(client, tmp_path)
    _, manifest = fetch_manifest(client)
    token = token_from_start_url(manifest["start_url"])

    bare = app.test_client()
    rv = bare.get(f"/admin/remote/protected_check?device_token={token}")
    assert rv.status_code == 401
    rv2 = bare.post(f"/admin/remote/_debug/press?btn=OK&phase=tap"
                    f"&device_token={token}")
    assert rv2.status_code == 401


# ---------------------------------------------------------------------------
# Lazy mint: devices paired before this feature get a token on their next
# authenticated manifest fetch.
# ---------------------------------------------------------------------------

def test_token_salt_lazily_minted_for_pre_token_devices(client, tmp_path):
    device_id, _ = pair(client, tmp_path)
    paired_path = tmp_path / "paired_remotes.json"
    data = json.loads(paired_path.read_text())
    del data["devices"][0]["token_salt"]  # simulate a pre-upgrade pairing
    paired_path.write_text(json.dumps(data))

    _, manifest = fetch_manifest(client)
    token = token_from_start_url(manifest["start_url"])
    assert len(token) >= 32
    data2 = json.loads(paired_path.read_text())
    assert data2["devices"][0].get("token_salt")


# ===========================================================================
# Root Content Manager app — the surface people ACTUALLY install from (the
# pairing flow lands on /?tab=remote, and field testing showed Share → Add
# to Home Screen happens there, not on /admin/remote). Same token, second
# manifest + redeem path.
# ===========================================================================

def test_root_manifest_unauthenticated_has_no_token(client):
    for path in ("/manifest.webmanifest", "/static/manifest.webmanifest"):
        rv, manifest = fetch_manifest(client, path)
        assert rv.mimetype == "application/manifest+json"
        assert rv.headers.get("Cache-Control") == "no-store"
        assert manifest["start_url"] == "/"
        assert manifest["scope"] == "/"
        assert manifest["display"] == "standalone"
        assert manifest["name"] == "Magic Dingus Box"
        assert len(manifest["icons"]) == 4


def test_old_static_manifest_url_is_shadowed_by_dynamic_route(client, tmp_path):
    """A phone with a cached index.html still references
    /static/manifest.webmanifest. That URL must serve the DYNAMIC manifest
    (exact rule outranks the /static/<path:filename> converter rule) so a
    stale tokenless manifest can never be fetched by accident."""
    pair(client, tmp_path)
    _, manifest = fetch_manifest(client, "/static/manifest.webmanifest")
    assert manifest["start_url"].startswith("/?device_token=")


def test_root_manifest_with_cookie_carries_same_token_as_remote(client, tmp_path):
    pair(client, tmp_path)
    _, root_m = fetch_manifest(client, "/manifest.webmanifest")
    root_token = token_from_start_url(root_m["start_url"], base="/")
    _, remote_m = fetch_manifest(client)
    remote_token = token_from_start_url(remote_m["start_url"])
    # One durable token per device, embedded by BOTH manifests.
    assert root_token == remote_token
    assert len(root_token) >= 32


def test_root_redeem_sets_cookie_and_preserves_other_params(app, client, tmp_path):
    device_id, _ = pair(client, tmp_path)
    _, manifest = fetch_manifest(client, "/manifest.webmanifest")
    token = token_from_start_url(manifest["start_url"], base="/")

    installed = app.test_client()
    rv = installed.get(f"/?device_token={token}&tab=remote",
                       follow_redirects=False)
    assert rv.status_code == 303
    assert rv.headers["Location"].endswith("/?tab=remote")
    assert "device_token" not in rv.headers["Location"]
    set_cookie = rv.headers.get("Set-Cookie", "")
    assert "mdb_remote=" in set_cookie

    # The cookie authenticates the same device — which is what pairs the
    # SPA's Remote tab (a same-origin iframe of /admin/remote, or a
    # same-origin navigation on phones: one jar per install, shared).
    cookie = set_cookie.split(";")[0]
    name, value = cookie.split("=", 1)
    installed.set_cookie(domain="localhost", key=name, value=value)
    rv2 = installed.get("/admin/remote")
    assert rv2.status_code == 200
    assert b"remote.js" in rv2.data
    rv3 = installed.get("/admin/remote/protected_check")
    assert rv3.status_code == 200
    assert json.loads(rv3.data)["data"]["device_id"] == device_id

    # No leftover params → bare redirect target, on either route alias.
    installed4 = app.test_client()
    rv4 = installed4.get(f"/admin?device_token={token}", follow_redirects=False)
    assert rv4.status_code == 303
    assert rv4.headers["Location"].endswith("/admin")


def test_root_garbage_token_serves_spa_without_cookie(client):
    rv = client.get("/?device_token=not-a-real-token", follow_redirects=False)
    assert rv.status_code == 200
    assert b"Content Manager" in rv.data  # the SPA, not an error page
    assert "mdb_remote" not in rv.headers.get("Set-Cookie", "")


def test_root_revoked_token_serves_spa_without_cookie(app, client, tmp_path):
    device_id, _ = pair(client, tmp_path)
    _, manifest = fetch_manifest(client, "/manifest.webmanifest")
    token = token_from_start_url(manifest["start_url"], base="/")
    (tmp_path / "pending_revocations.txt").write_text(device_id + "\n")

    installed = app.test_client()
    rv = installed.get(f"/?device_token={token}", follow_redirects=False)
    assert rv.status_code == 200
    assert b"Content Manager" in rv.data
    assert "mdb_remote" not in rv.headers.get("Set-Cookie", "")


# ===========================================================================
# Cross-origin steering (QR pairs on the IP origin; the .local origin has
# its own cookie jar). The steering toasts append ?device_token= to their
# links client-side, sourced from the dynamic manifest; server-side, the
# contract is: manifest link tags fetch WITH credentials, and the token
# alone pairs a foreign jar at both destinations.
# ===========================================================================

def test_manifest_link_tags_fetch_with_credentials(app, client, tmp_path):
    """Browsers fetch <link rel="manifest"> WITHOUT cookies — even
    same-origin — unless the tag carries crossorigin="use-credentials".
    Without it the install-time fetch never presents the pairing cookie,
    no manifest ever embeds a token, and the whole install-pairing scheme
    silently degrades. Guard every page that can be installed from."""
    assert b'crossorigin="use-credentials"' in client.get("/").data
    # Unpaired /admin/remote (the inline pair page)...
    assert b'crossorigin="use-credentials"' in client.get("/admin/remote").data
    # ...and the paired remote shell.
    pair(client, tmp_path)
    assert b'crossorigin="use-credentials"' in client.get("/admin/remote").data


def test_steering_token_pairs_a_foreign_origin_jar(app, client, tmp_path):
    """Alex's field case: paired on the IP origin, typed the .local
    address, got asked for the code again. The steering link now carries
    the manifest's token; a fresh cookie jar (stand-in for the .local
    origin) presenting only that token must come out paired at BOTH
    destinations, with the 303 stripping the token from the URL."""
    pair(client, tmp_path)
    _, manifest = fetch_manifest(client, "/manifest.webmanifest")
    token = token_from_start_url(manifest["start_url"], base="/")

    for dest, clean in ((f"/?device_token={token}", "/"),
                        (f"/admin/remote?device_token={token}", "/admin/remote")):
        other_origin = app.test_client()  # fresh jar = the .local origin
        rv = other_origin.get(dest, follow_redirects=False)
        assert rv.status_code == 303
        assert token not in rv.headers["Location"]
        assert rv.headers["Location"].endswith(clean)
        assert "mdb_remote=" in rv.headers.get("Set-Cookie", "")
