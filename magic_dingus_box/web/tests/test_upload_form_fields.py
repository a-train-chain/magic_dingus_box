"""Every video upload path sends the owner's Quality / Framing / Normalize.

/admin/smart-upload reads `resolution`, `fit_mode` and `normalize_audio`
from the form and falls back to its own defaults when they are absent. The
file-picker path (handleDirectUpload) always sent them; the drag-and-drop /
retry / playlist-editor path (uploadSingleFile) sent only the file, so the
owner's choices were silently ignored depending on HOW the file was added.

This runs the REAL manager_media.js under node with a fake DOM and a fake
XMLHttpRequest that records each multipart body, drives both paths, and
asserts they send the same three fields. Skipped where node is absent.
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
const [mediaPath, controlsJson] = process.argv.slice(1);
const controls = JSON.parse(controlsJson);  // null = controls absent

const sent = [];
class FakeFormData {
    constructor() { this.entries = []; }
    append(k, v) { this.entries.push([k, typeof v === 'string' ? v : '<file>']); }
}
class FakeXHR {
    constructor() { this.upload = { addEventListener() {} }; this.listeners = {}; }
    open(method, url) { this.url = url; }
    setRequestHeader() {}
    addEventListener(ev, fn) { this.listeners[ev] = fn; }
    send(body) {
        sent.push({ url: this.url, fields: Object.fromEntries(body.entries) });
        this.status = 200;
        this.responseText = JSON.stringify({ ok: true, data: { action: 'direct', output_filename: 'x.mp4' } });
        setImmediate(() => {
            if (this.onload) this.onload();
            if (this.listeners.load) this.listeners.load();
        });
    }
}

function element(id) {
    if (controls === null) return null;
    if (id === 'targetResolution') return { value: controls.resolution };
    if (id === 'framingMode') return { value: controls.fit_mode };
    if (id === 'normalizeAudio') return { checked: controls.normalize };
    return null;
}

const ctx = {
    console: { log() {}, warn() {}, error() {} },
    document: { getElementById: element, body: { contains: () => false } },
    FormData: FakeFormData,
    XMLHttpRequest: FakeXHR,
    setTimeout: (fn) => setImmediate(fn),
    clearTimeout() {},
    UPLOAD_CONFIG: {
        MAX_RETRIES: 1, RETRY_DELAY_MS: 0, TIMEOUT_MS: 1000,
        TRANSCODE: { DEFAULT_RESOLUTION: 'crt_hd', DEFAULT_FIT_MODE: 'crop' },
    },
    currentDevice: { url: 'http://box' },
    csrfToken: 'tok',
    spriteIcon: () => '',
    plural: (n, w) => `${n} ${w}`,
    acquireWakeLock: async () => {},
    releaseWakeLock: async () => {},
    loadAllContentFromDevice: async () => {},
    xhrIsCsrfRejection: () => false,
    fetchCsrfToken: async () => {},
};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(mediaPath, 'utf8'), ctx, { filename: 'manager_media.js' });

(async () => {
    const file = { name: 'clip.mov', size: 1 };
    await ctx.handleDirectUpload({ files: [file] });
    const progressBar = { update() {}, complete() {}, error() {}, setRetrying() {} };
    const r = await ctx.uploadSingleFile(file, progressBar, false);
    process.stdout.write(JSON.stringify({ sent, single: r }));
})().catch(e => { console.error = globalThis.console.error; process.stderr.write(String(e.stack)); process.exit(1); });
"""


def _run(controls):
    node = shutil.which("node")
    if node is None:
        pytest.skip("node not installed")
    res = subprocess.run(
        [node, "-e", HARNESS, str(STATIC / "manager_media.js"), json.dumps(controls)],
        capture_output=True, text=True, timeout=30)
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout)


@pytest.mark.parametrize("controls,expected", [
    # The owner's non-default choices reach the server on both paths.
    ({"resolution": "crt_fhd", "fit_mode": "fit", "normalize": True},
     {"resolution": "crt_fhd", "fit_mode": "fit", "normalize_audio": "true"}),
    ({"resolution": "crt", "fit_mode": "crop", "normalize": False},
     {"resolution": "crt", "fit_mode": "crop", "normalize_audio": "false"}),
    # Controls absent from the page: the file picker's defaults, on both.
    (None,
     {"resolution": "crt_hd", "fit_mode": "crop", "normalize_audio": "false"}),
])
def test_both_upload_paths_send_the_same_three_fields(controls, expected):
    out = _run(controls)
    assert out["single"] == {"success": True}
    picker, single = out["sent"]
    assert picker["url"] == single["url"] == "http://box/admin/smart-upload"
    want = {"file": "<file>", **expected}
    assert picker["fields"] == want, "file-picker path (handleDirectUpload)"
    assert single["fields"] == want, "drag/drop/retry path (uploadSingleFile)"
