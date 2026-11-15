"""The Content Manager reports the browser's time zone once per device load.

Runs the REAL manager_devices.js loadAllContentFromDevice under node with
stubs; asserts it POSTs {zone} to /admin/system/timezone after the CSRF
fetch, and that a failing POST never breaks the load. Skipped without node.
"""
from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

STATIC = Path(__file__).resolve().parents[1] / "static"

HARNESS = r"""
const vm = require('vm');
const fs = require('fs');
const [path, mode] = process.argv.slice(1);
const order = [];
const posts = [];
const ctx = {
    console: { log() {}, warn() {}, error() {} },
    Intl: { DateTimeFormat: () => ({ resolvedOptions: () => ({ timeZone: 'America/Los_Angeles' }) }) },
    currentDevice: { url: 'http://box' },
    csrfToken: 'tok',
    fetchCsrfToken: async () => { order.push('csrf'); },
    getCsrfHeaders: () => ({ 'Content-Type': 'application/json', 'X-CSRF-Token': 'tok' }),
    fetchWithCsrfRetry: (u, o) => ctx.fetch(u, o),
    fetch: async (url, opts) => {
        order.push('post');
        posts.push({ url, method: opts.method, body: JSON.parse(opts.body), headers: opts.headers });
        if (mode === 'fail') throw new Error('network');
        return { ok: true, json: async () => ({}) };
    },
    loadVideos: async () => { order.push('videos'); },
    loadROMs: async () => {}, loadExistingPlaylists: async () => {},
    renderVideoPlaylistAvailable() {}, renderGamePlaylistAvailable() {},
    renderLibraryPanel() {}, refreshHealthInfo() {}, loadVersionInfo() {},
};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path, 'utf8'), ctx, { filename: 'manager_devices.js' });
(async () => {
    await ctx.loadAllContentFromDevice();
    await new Promise(r => setImmediate(r));
    process.stdout.write(JSON.stringify({ order, posts }));
})().catch(e => { process.stderr.write(String(e.stack)); process.exit(1); });
"""


def _run(mode):
    node = shutil.which("node")
    if node is None:
        pytest.skip("node not installed")
    res = subprocess.run([node, "-e", HARNESS, str(STATIC / "manager_devices.js"), mode],
                         capture_output=True, text=True, timeout=30)
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout)


def test_posts_zone_after_csrf():
    out = _run("ok")
    assert len(out["posts"]) == 1
    p = out["posts"][0]
    assert p["url"] == "http://box/admin/system/timezone"
    assert p["method"] == "POST"
    assert p["body"] == {"zone": "America/Los_Angeles"}
    assert out["order"].index("csrf") < out["order"].index("post")


def test_failure_never_breaks_page_load():
    out = _run("fail")
    assert "videos" in out["order"]
