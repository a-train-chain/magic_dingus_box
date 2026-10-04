#!/usr/bin/env python3
"""A local stand-in for the three GitHub surfaces update.sh talks to.

  api.github.com   GET /repos/<repo>/releases/latest
                   GET /repos/<repo>/releases/tags/v<X.Y.Z>
  github.com       GET /<repo>/releases/download/v<X.Y.Z>/<asset>
                   -> 302 to the asset CDN, as the real site does
  objects.githubusercontent.com (+ release-assets.githubusercontent.com)
                   GET <signed-looking path> -> the asset bytes

Releases come from RELEASES_DIR/v<X.Y.Z>/ (whatever asset files are there).
Behaviour is steered per request by CONTROL (JSON, re-read every request):
  {"latest": "1.10.0",        which release /releases/latest returns
   "minify": false,           compact JSON instead of GitHub's pretty form
   "asset_order": "release"}  "release" = the order release.yml uploads in
                              (source, checksum, binary); "reversed" puts
                              the arm64 binary FIRST (the upload-order trap
                              check_update's asset match was hardened for)
Every request is appended to REQUEST_LOG as one JSON line. Read-only: any
non-GET is answered 405, so nothing can ever be "published" to it.
"""
import hashlib
import json
import os
import re
import ssl
import sys
import time
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, urlsplit

RELEASES_DIR = Path(os.environ.get("FAKE_GH_RELEASES", "/w/release"))
CONTROL = Path(os.environ.get("FAKE_GH_CONTROL", "/run/fake-github/control.json"))
REQUEST_LOG = Path(os.environ.get("FAKE_GH_LOG", "/out/fake_github_requests.jsonl"))
REPO = os.environ.get("FAKE_GH_REPO", "a-train-chain/magic_dingus_box")
BODY_TEMPLATE = RELEASES_DIR / "body_template.md"
TEMPLATE_VERSION = os.environ.get("FAKE_GH_TEMPLATE_VERSION", "")
CDN = "objects.githubusercontent.com"


def control() -> dict:
    try:
        return json.loads(CONTROL.read_text())
    except (OSError, ValueError):
        return {}


def asset_id(name: str) -> int:
    return int(hashlib.sha1(name.encode()).hexdigest()[:8], 16)


def release_assets(ver: str, order: str):
    d = RELEASES_DIR / f"v{ver}"
    if not d.is_dir():
        return None
    files = {p.name: p for p in d.iterdir() if p.is_file()}
    # release.yml's `files:` order: source tarball, checksum, arm64 binary
    rank = lambda n: (0 if n.startswith("magic-dingus-box-") else
                      1 if n == "checksum.sha256" else 2, n)
    names = sorted(files, key=rank)
    if order == "reversed":
        names.reverse()
    return [(n, files[n]) for n in names]


def release_json(ver: str, order: str):
    assets = release_assets(ver, order)
    if assets is None:
        return None
    tag = f"v{ver}"
    body = BODY_TEMPLATE.read_text() if BODY_TEMPLATE.exists() else f"Release {tag}"
    if TEMPLATE_VERSION:
        body = body.replace(TEMPLATE_VERSION, ver)
    rid = asset_id(tag)
    published = datetime.fromtimestamp(
        max(p.stat().st_mtime for _, p in assets), tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    api = f"https://api.github.com/repos/{REPO}"
    return {
        "url": f"{api}/releases/{rid}",
        "assets_url": f"{api}/releases/{rid}/assets",
        "upload_url": f"https://uploads.github.com/repos/{REPO}/releases/{rid}/assets{{?name,label}}",
        "html_url": f"https://github.com/{REPO}/releases/tag/{tag}",
        "id": rid,
        "author": {"login": "github-actions[bot]", "id": 41898282, "type": "Bot"},
        "node_id": f"RE_kwDO{rid:08X}",
        "tag_name": tag,
        "target_commitish": "main",
        "name": tag,
        "draft": False,
        "immutable": False,
        "prerelease": False,
        "created_at": published,
        "updated_at": published,
        "published_at": published,
        "assets": [{
            "url": f"{api}/releases/assets/{asset_id(n)}",
            "id": asset_id(n),
            "node_id": f"RA_kwDO{asset_id(n):08X}",
            "name": n,
            "label": "",
            "uploader": {"login": "github-actions[bot]", "id": 41898282, "type": "Bot"},
            "content_type": "application/gzip" if n.endswith(".gz") else "application/octet-stream",
            "state": "uploaded",
            "size": p.stat().st_size,
            "digest": "sha256:" + hashlib.sha256(p.read_bytes()).hexdigest(),
            "download_count": 0,
            "created_at": published,
            "updated_at": published,
            "browser_download_url": f"https://github.com/{REPO}/releases/download/{tag}/{n}",
        } for n, p in assets],
        "tarball_url": f"{api}/tarball/{tag}",
        "zipball_url": f"{api}/zipball/{tag}",
        "body": body,
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "GitHub.com"
    sys_version = ""

    def log_message(self, fmt, *args):  # quiet stderr; we keep our own log
        pass

    def _log(self, status, nbytes=0, note=""):
        rec = {"t": round(time.time(), 3), "method": self.command,
               "host": (self.headers.get("Host") or "").split(":")[0],
               "path": self.path, "ua": self.headers.get("User-Agent", ""),
               "status": status, "bytes": nbytes}
        if note:
            rec["note"] = note
        with REQUEST_LOG.open("a") as f:
            f.write(json.dumps(rec) + "\n")

    def _json(self, status, obj, minify):
        data = (json.dumps(obj, separators=(",", ":")) if minify
                else json.dumps(obj, indent=2)).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        self._log(status, len(data))

    def _not_found(self, minify=False):
        self._json(404, {"message": "Not Found",
                         "documentation_url": "https://docs.github.com/rest",
                         "status": "404"}, minify)

    def do_GET(self):
        ctl = control()
        minify = bool(ctl.get("minify"))
        order = ctl.get("asset_order", "release")
        host = (self.headers.get("Host") or "").split(":")[0]
        path = urlsplit(self.path).path

        if host == "api.github.com":
            if path == f"/repos/{REPO}/releases/latest":
                rel = release_json(str(ctl.get("latest", "")), order)
                return self._json(200, rel, minify) if rel else self._not_found(minify)
            m = re.fullmatch(rf"/repos/{re.escape(REPO)}/releases/tags/v([0-9]+\.[0-9]+\.[0-9]+)", path)
            if m:
                rel = release_json(m.group(1), order)
                return self._json(200, rel, minify) if rel else self._not_found(minify)
            return self._not_found(minify)

        if host == "github.com":
            m = re.fullmatch(rf"/{re.escape(REPO)}/releases/download/v([0-9.]+)/([^/]+)", path)
            if m and (RELEASES_DIR / f"v{m.group(1)}" / m.group(2)).is_file():
                ver, name = m.groups()
                loc = (f"https://{CDN}/github-production-release-asset-2e65be/{asset_id(name)}/"
                       f"{uuid.uuid4()}?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Expires=300"
                       f"&response-content-disposition={quote('attachment; filename=' + name)}"
                       f"&response-content-type=application%2Foctet-stream"
                       f"&mdb_release={ver}&mdb_asset={quote(name)}")
                self.send_response(302)
                self.send_header("Location", loc)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return self._log(302, 0, note=f"-> {CDN}")
            self.send_response(404)
            self.send_header("Content-Length", "9")
            self.end_headers()
            self.wfile.write(b"Not Found")
            return self._log(404)

        if host in (CDN, "release-assets.githubusercontent.com"):
            q = dict(kv.split("=", 1) for kv in urlsplit(self.path).query.split("&") if "=" in kv)
            from urllib.parse import unquote
            ver, name = q.get("mdb_release", ""), unquote(q.get("mdb_asset", ""))
            p = RELEASES_DIR / f"v{ver}" / name
            if not (ver and name and "/" not in name and p.is_file()):
                self.send_response(403)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return self._log(403)
            size = p.stat().st_size
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("Content-Disposition", f"attachment; filename={name}")
            self.end_headers()
            with p.open("rb") as f:
                while chunk := f.read(1 << 20):
                    self.wfile.write(chunk)
            return self._log(200, size)

        self._not_found()

    def _refuse(self):
        self.send_response(405)
        self.send_header("Content-Length", "0")
        self.end_headers()
        self._log(405, note="read-only fake: write refused")

    do_POST = do_PUT = do_PATCH = do_DELETE = _refuse


def main():
    cert, key = sys.argv[1], sys.argv[2]
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    srv = ThreadingHTTPServer(("127.0.0.1", 443), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print("fake GitHub listening on 127.0.0.1:443", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
