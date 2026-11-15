"""The CSRF-rejection envelope the Content Manager's retry depends on.

CSRF tokens live in the web server's memory for an hour and vanish on any
restart, so a long-open page eventually sends a dead token. manager.js
recognises that case — and only that case — by HTTP 403 plus
{"ok": false, "error": {"code": "CSRF_ERROR"}}, fetches a fresh token and
re-sends the request once (fetchWithCsrfRetry / xhrIsCsrfRejection).
Changing the status or the code silently brings back "Failed to save
playlist" after an hour, so it is pinned here, together with the real
client helpers run under node against the real server responses.
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

import admin_security  # owns the CSRF token store (patch it there)
from admin import create_app

MANAGER_JS = Path(__file__).resolve().parents[1] / "static" / "manager.js"
SAVE = "/admin/playlists/mix.yaml"
BODY = json.dumps({"title": "Mix", "items": []})


@pytest.fixture
def csrf_client(temp_data_dir, monkeypatch):
    monkeypatch.delenv("MAGIC_DISABLE_CSRF", raising=False)
    app = create_app(temp_data_dir, config={"TESTING": True})
    app.config["TESTING"] = True
    return app.test_client()


def _save(client, token=None):
    headers = {"X-CSRF-Token": token} if token else {}
    return client.post(SAVE, data=BODY, content_type="application/json",
                       headers=headers)


def _assert_csrf_envelope(rv):
    assert rv.status_code == 403
    assert rv.get_json() == {
        "ok": False,
        "error": {"code": "CSRF_ERROR",
                  "message": "Invalid or missing CSRF token"},
    }


def test_missing_token_is_a_csrf_rejection(csrf_client):
    _assert_csrf_envelope(_save(csrf_client))


def test_unknown_token_is_a_csrf_rejection(csrf_client):
    # What every open page holds after the web server restarts.
    _assert_csrf_envelope(_save(csrf_client, "not-a-token-this-process-minted"))


def test_expired_token_is_a_csrf_rejection(csrf_client, monkeypatch):
    token = csrf_client.get("/admin/csrf-token").get_json()["data"]["token"]
    monkeypatch.setitem(admin_security._csrf_tokens, token, 0.0)  # long past
    _assert_csrf_envelope(_save(csrf_client, token))


def test_a_fresh_token_is_accepted(csrf_client):
    rv = csrf_client.get("/admin/csrf-token")
    assert rv.status_code == 200
    token = rv.get_json()["data"]["token"]
    assert _save(csrf_client, token).status_code == 200


# ── the real client helpers, under node ─────────────────────────────────────

def _js_function(name: str) -> str:
    src = MANAGER_JS.read_text(encoding="utf-8")
    m = re.search(rf"^(async )?function {name}\(.*?^\}}", src, re.S | re.M)
    assert m, f"{name} not found in manager.js"
    return m.group(0)


def _node(script: str, stdin: str = "") -> str:
    node = shutil.which("node")
    if node is None:
        pytest.skip("node not installed")
    return subprocess.run([node, "-e", script], input=stdin,
                          capture_output=True, text=True, check=True).stdout


def test_client_recognises_exactly_the_csrf_rejection(csrf_client):
    rejected = _save(csrf_client)
    cross_site = csrf_client.post(SAVE, data=BODY,
                                  content_type="application/json",
                                  headers={"Sec-Fetch-Site": "cross-site"})
    assert cross_site.status_code == 403  # a 403 that must NOT be retried
    cases = [
        [rejected.status_code, rejected.get_json(), True],
        [cross_site.status_code, cross_site.get_json(), False],
        [400, {"ok": False, "error": {"code": "CSRF_ERROR"}}, False],
        [403, None, False],
    ]
    script = (_js_function("isCsrfRejection") + "\n"
              + _js_function("xhrIsCsrfRejection") + "\n"
              + "const cases = JSON.parse(require('fs').readFileSync(0,'utf8'));\n"
              + "process.stdout.write(JSON.stringify(cases.map(([s, b]) => ["
              + "isCsrfRejection(s, b),"
              + "xhrIsCsrfRejection({status: s, responseText: JSON.stringify(b)})])));")
    got = json.loads(_node(script, json.dumps(cases)))
    assert got == [[want, want] for _, _, want in cases]


def test_fetch_with_csrf_retry_refreshes_and_resends_once():
    # A stub server: the first token is dead, the refreshed one works.
    script = "\n".join([
        _js_function("isCsrfRejection"),
        _js_function("fetchWithCsrfRetry"),
        """
let csrfToken = 'dead';
const sent = [];
async function fetchCsrfToken() { csrfToken = 'fresh'; }
global.fetch = async (url, opts) => {
    sent.push(opts.headers['X-CSRF-Token']);
    const ok = opts.headers['X-CSRF-Token'] === 'fresh';
    const body = ok ? {ok: true, data: {}}
        : {ok: false, error: {code: 'CSRF_ERROR', message: 'x'}};
    return new Response(JSON.stringify(body), {status: ok ? 200 : 403});
};
(async () => {
    const r = await fetchWithCsrfRetry('/x', {method: 'POST', body: '{}',
        headers: {'Content-Type': 'application/json', 'X-CSRF-Token': csrfToken}});
    // Still dead after the refresh: exactly one re-send, then give up.
    csrfToken = 'dead';
    fetchCsrfToken = async () => { csrfToken = 'still-dead'; };
    const r2 = await fetchWithCsrfRetry('/x', {method: 'POST', body: '{}',
        headers: {'X-CSRF-Token': csrfToken}});
    process.stdout.write(JSON.stringify({status: r.status, status2: r2.status, sent}));
})();
""",
    ])
    out = json.loads(_node(script))
    assert out == {"status": 200, "status2": 403,
                   "sent": ["dead", "fresh", "dead", "still-dead"]}
