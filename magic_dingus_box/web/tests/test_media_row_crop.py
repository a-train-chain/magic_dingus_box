"""The video library row's Crop checkbox, from the REAL manager_media.js run
under node with a fake DOM. Skipped where node is absent."""
from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

STATIC = Path(__file__).resolve().parents[1] / "static"

HARNESS = r"""
const vm = require('vm');
const fs = require('fs');
const [mediaPath, videosJson, escapeJsSrc] = process.argv.slice(1);
const videos = JSON.parse(videosJson);

const container = { innerHTML: '' };
const calls = [];
const ctx = {
    console: { log() {}, warn() {}, error() {} },
    document: {
        getElementById: (id) => id === 'videoList' ? container : null,
        querySelectorAll: () => [],
        body: { contains: () => false },
    },
    AppState: { media: { videos: [] }, device: { current: true, url: 'http://box' } },
    currentDevice: { url: 'http://box' },
    fetch: async (url) => ({ json: async () => ({
        data: String(url).endsWith('/admin/media') ? videos : [] }) }),
    escapeHtml: (s) => String(s).replace(/[&<>"']/g,
        c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])),
    normalizeVideoPath: (p) => p,
    formatFileSize: (n) => `${n} B`,
    getCsrfHeaders: () => ({ 'X-CSRF-Token': 'tok' }),
    apiRequest: async (url, opts) => { calls.push({ url, opts }); return { crop: false }; },
    renderVideoPlaylistAvailable() {},
    spriteIcon: () => '',
    alert() {},
};
vm.createContext(ctx);
vm.runInContext(escapeJsSrc, ctx, { filename: 'manager.js (escapeJs)' });
vm.runInContext(fs.readFileSync(mediaPath, 'utf8'), ctx, { filename: 'manager_media.js' });

(async () => {
    await ctx.loadVideos();
    const html = container.innerHTML;
    let toggled = null;
    if (typeof ctx.setVideoCrop === 'function') {
        await ctx.setVideoCrop('data/media/a.mp4', false);
        toggled = calls;
    }
    process.stdout.write(JSON.stringify({ html, calls: toggled }));
})().catch(e => { process.stderr.write(String(e.stack)); process.exit(1); });
"""


def _real_escape_js() -> str:
    """escapeJs exactly as manager.js defines it (same extraction as
    test_escape_js.py), so these tests cannot drift from the real escaper."""
    src = (STATIC / "manager.js").read_text(encoding="utf-8")
    m = re.search(r"^function escapeJs\(str\) \{.*?^\}", src, re.S | re.M)
    assert m, "escapeJs not found in manager.js"
    return m.group(0)


def _run(videos):
    node = shutil.which("node")
    if node is None:
        pytest.skip("node not installed")
    res = subprocess.run(
        [node, "-e", HARNESS, str(STATIC / "manager_media.js"), json.dumps(videos),
         _real_escape_js()],
        capture_output=True, text=True, timeout=30)
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout)


def _v(name, aspect, crop=True):
    return {"filename": name, "path": f"data/media/{name}", "size": 1,
            "display_aspect": aspect, "crop": crop}


def test_crop_checkbox_only_for_a_shape_that_differs_from_4_3():
    out = _run([_v("wide.mp4", 1.7778), _v("square43.mp4", 1.3333),
                _v("unknown.mp4", None)])
    rows = out["html"].split("<tr>")[2:]  # [0] header block, [1] its th row
    assert len(rows) == 3
    assert "video-crop" in rows[0]
    assert '<input type="checkbox" checked' in rows[0]
    assert "video-crop" not in rows[1]
    assert "video-crop" not in rows[2]


def test_unticked_when_the_video_is_fitted():
    out = _run([_v("wide.mp4", 1.7778, crop=False)])
    assert "video-crop" in out["html"]
    assert "<input type=\"checkbox\" checked" not in out["html"]


def test_filename_in_the_handler_is_escaped():
    out = _run([_v("it's \"x\".mp4", 1.7778)])
    handler = out["html"].split("video-crop")[1].split("<input")[1].split(">")[0]
    # The real escapeJs: quotes become \\x27 / \\x22, inert as HTML.
    assert "data/media/it\\x27s \\x22x\\x22.mp4" in handler
    assert "it's" not in handler and '"x"' not in handler


@pytest.mark.parametrize("aspect, shown", [
    (1.3333, False),   # 4:3
    (1.3599, False),   # just inside +2 % (1.36)
    (1.3068, False),   # just inside -2 % (1.30667)
    (1.3605, True),    # just outside +2 %
    (1.3066, True),    # just outside -2 %
])
def test_four_by_three_is_within_two_percent_relative(aspect, shown):
    out = _run([_v("a.mp4", aspect)])
    assert ("video-crop" in out["html"]) is shown


def test_toggle_calls_apiRequest_with_path_and_crop():
    out = _run([_v("a.mp4", 1.7778)])
    assert len(out["calls"]) == 1
    call = out["calls"][0]
    assert call["url"] == "http://box/admin/media/picture"
    assert call["opts"]["method"] == "POST"
    assert json.loads(call["opts"]["body"]) == {
        "path": "data/media/a.mp4", "crop": False}
