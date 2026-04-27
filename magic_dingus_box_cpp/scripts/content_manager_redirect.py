#!/usr/bin/env python3
"""
Magic Dingus Box - port 80 → :5000 redirect server.

Why this exists: the Content Manager (Flask) listens on :5000. Operators
who plug a laptop into the Pi's USB-C port (or join the Pi's Wi-Fi)
expect to type a short address like `magicpi.local` or `10.55.0.1` in
their browser and land on the Content Manager — not get an
"unable to connect" because they forgot the port number. This tiny
HTTP server fills that gap by listening on port 80 and 302-redirecting
ANY request to the same host:port 5000.

It's intentionally minimal — no static files, no logic, no logging
beyond stdout. Runs as a systemd unit.

Behavior examples:
  GET  http://magicpi.local/         → 302 → http://magicpi.local:5000/
  GET  http://10.55.0.1/             → 302 → http://10.55.0.1:5000/
  GET  http://10.0.0.76/admin/...    → 302 → http://10.0.0.76:5000/admin/...
  GET  http://magicpi.local/foo?bar  → 302 → http://magicpi.local:5000/foo?bar

Captive-portal interception is a separate feature; this server doesn't
do it (and probing requests like /hotspot-detect.html still get
redirected to :5000, which is fine — the worst that happens is macOS
shows a "Sign in to network" popup, which the user can dismiss or
click-through to the Content Manager).
"""

import http.server
import socketserver
import sys
from urllib.parse import urlsplit, urlunsplit

PORT = 80
TARGET_PORT = 5000


class RedirectHandler(http.server.BaseHTTPRequestHandler):
    """302-redirect every request to the same host on TARGET_PORT."""

    def do_GET(self):
        self._redirect()

    def do_HEAD(self):
        self._redirect()

    def do_POST(self):
        self._redirect()

    def _redirect(self):
        # The Host header tells us what name/IP the client used to reach us.
        # We rewrite ONLY the port — preserve the original hostname so that
        # `magicpi.local` stays as `magicpi.local` (works on the host's
        # network), and so that 10.x.x.x stays as 10.x.x.x.
        host_header = self.headers.get("Host", "").strip()
        # Strip any existing :port from the host header.
        if ":" in host_header and not host_header.startswith("["):
            host_only = host_header.rsplit(":", 1)[0]
        elif host_header.startswith("[") and "]:" in host_header:
            host_only = host_header.split("]:", 1)[0] + "]"
        else:
            host_only = host_header

        # Default fallback so a malformed Host header doesn't crash us.
        if not host_only:
            host_only = "magicpi.local"

        # Preserve the path + query so deep-links keep working.
        parts = urlsplit(self.path)
        target_path = parts.path or "/"
        target_query = ("?" + parts.query) if parts.query else ""
        target_frag = ("#" + parts.fragment) if parts.fragment else ""
        location = f"http://{host_only}:{TARGET_PORT}{target_path}{target_query}{target_frag}"

        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", "0")
        self.end_headers()

    # Quiet the default access log (single line per redirect is fine,
    # default format is too noisy for journalctl).
    def log_message(self, fmt, *args):
        sys.stdout.write(f"[redirect] {self.address_string()} → "
                         f"{self.command} {self.path} → 302\n")
        sys.stdout.flush()


def main():
    # Bind to 0.0.0.0 so we accept on every interface — usb0, wlan0, lo.
    # Port 80 requires root; the systemd unit grants CAP_NET_BIND_SERVICE
    # so we don't have to run as full root.
    with socketserver.ThreadingTCPServer(("", PORT), RedirectHandler) as httpd:
        # Allow rapid restart without TIME_WAIT errors during dev.
        httpd.allow_reuse_address = True
        sys.stdout.write(f"[redirect] listening on :{PORT} → forwarding to :{TARGET_PORT}\n")
        sys.stdout.flush()
        httpd.serve_forever()


if __name__ == "__main__":
    main()
