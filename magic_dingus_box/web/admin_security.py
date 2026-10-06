"""Request guards for the Content Manager: CSRF tokens, the Host-header
allowlist (DNS-rebinding defence), the Sec-Fetch-Site cross-site refusal,
the optional MAGIC_ADMIN_TOKEN gate, and path containment (_is_within).

register() installs the before_request hooks and GET /admin/csrf-token and
hands `require_csrf` back to admin.create_app() through ctx. It runs before
any other register() so the hooks stay first in line, exactly as when they
were inline in create_app(). The CSRF token store is per-process state —
one more reason the server must stay at ONE gunicorn worker.
"""
from __future__ import annotations

import ipaddress
import os
import secrets
import socket
import threading
import time
from functools import wraps
from pathlib import Path

from flask import request

# Same dual import form as admin.py: flat when tests put web/ on
# sys.path, package-relative in production.
try:
    from admin_common import error_response, success_response
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import error_response, success_response


# CSRF Token Storage (in-memory with expiration)
# In production, consider using Redis or session storage
_csrf_tokens: dict[str, float] = {}
_CSRF_TOKEN_EXPIRY = 3600  # 1 hour
# Bounded, and locked. GET /admin/csrf-token mints a token per call, so the
# dict grew without limit for anything that looped it, and the cleanup
# iterated it while request threads inserted ("dictionary changed size during
# iteration" -> a 500 on a random request). A household holds a handful of
# live tokens; at the cap the earliest-expiring (= oldest) are dropped, and
# an operator whose token was evicted just reloads the page.
_CSRF_TOKEN_MAX = 2048
_csrf_lock = threading.Lock()


def _cleanup_expired_tokens():
    """Remove expired CSRF tokens. Caller holds _csrf_lock."""
    current_time = time.time()
    expired = [token for token, expiry in _csrf_tokens.items() if current_time > expiry]
    for token in expired:
        del _csrf_tokens[token]


def _generate_csrf_token() -> str:
    """Generate a new CSRF token."""
    token = secrets.token_urlsafe(32)
    with _csrf_lock:
        _cleanup_expired_tokens()
        overflow = len(_csrf_tokens) - (_CSRF_TOKEN_MAX - 1)
        if overflow > 0:
            for old in sorted(_csrf_tokens, key=_csrf_tokens.get)[:overflow]:
                del _csrf_tokens[old]
        _csrf_tokens[token] = time.time() + _CSRF_TOKEN_EXPIRY
    return token


def _validate_csrf_token(token: str | None) -> bool:
    """Validate a CSRF token.

    Tokens expire after 1 hour but are NOT single-use — the frontend
    fetches one token at app load and reuses it across all state-changing
    requests for the session. Per-request rotation would require frontend
    work to refetch before each request; for the LAN-only single-operator
    kiosk threat model the expiry-based scheme is adequate.
    """
    if not token:
        return False
    with _csrf_lock:
        _cleanup_expired_tokens()
        return token in _csrf_tokens


# ===== HOST HEADER ALLOWLIST (DNS-rebinding defence) =====
#
# The web admin deliberately has NO login (owner decision: a customer must
# never need a password or PIN). That makes it a DNS-rebinding target: a web
# page on the internet can point its own hostname at this box's LAN IP and
# then script same-origin requests at it from the victim's browser. The one
# thing such a request cannot fake is the Host header — it carries the
# ATTACKER's domain. So we accept only names a person on the LAN could
# legitimately type, and refuse everything else. Zero friction for real
# users; no credentials involved.
#
# Allowed:
#   * any IP literal (v4 / bracketed v6, optional port) — the pairing QR and
#     the typed address on the Connect screen are the LAN IP
#   * single-label names ("localhost", "magicpi-ab12", "dingus") — not
#     registrable on the public internet, so not rebindable
#   * *.local (mDNS: magicpi-XXXX.local), *.localhost, and the router-local
#     suffixes (.lan, .home, .home.arpa, .internal, .localdomain) — none are
#     publicly delegated
#   * dingus.box — the name advertised for the USB-C cable
#     (scripts/data/dnsmasq-usb0.conf, pairing_screen_renderer.cpp)
#   * this machine's own hostname / FQDN
#   * <this box's hostname>.<router suffix> for the suffixes home routers
#     append in their own DNS (_ROUTER_DNS_SUFFIXES: magicpi-ab12.fritz.box,
#     magicpi-ab12.attlocal.net, magicpi-ab12.router). The first label must
#     equal socket.gethostname() exactly (case-insensitive). These suffixes
#     sit under publicly registered domains (fritz.box is AVM's,
#     attlocal.net AT&T's), so unlike .lan they are not accepted for ANY
#     first label: the box's own unique name narrows them to this box. The
#     suffix list stays explicit on purpose. "Own hostname + any suffix"
#     would admit magicpi-ab12.<attacker's domain>: the hostname is visible
#     to anything on the LAN (mDNS) and its magicpi-XXXX pattern is
#     guessable, and the attacker controls the suffix, which is exactly
#     what this check exists to refuse.
#   * anything in MAGIC_ALLOWED_HOSTS (comma-separated; ".example.com" allows
#     a whole suffix) — an escape hatch that needs no release
_LOCAL_HOST_SUFFIXES = (
    ".local", ".localhost", ".lan", ".home", ".home.arpa", ".internal",
    ".localdomain",
)
_BUILTIN_ALLOWED_HOSTS = frozenset({"dingus.box"})
# Accepted only as <own hostname><suffix> (see above). Router DNS suffixes
# that are NOT already in _LOCAL_HOST_SUFFIXES (.lan, .home, .home.arpa,
# .localdomain, .internal and .local are accepted for any first label).
_ROUTER_DNS_SUFFIXES = (".fritz.box", ".attlocal.net", ".router")


def _split_host_header(host_header: str) -> str:
    """Return the lowercase host part of a Host header, port stripped."""
    host = (host_header or "").strip().lower()
    if host.startswith("["):
        end = host.find("]")
        return host[1:end] if end != -1 else host[1:]
    if host.count(":") == 1:
        host = host.rsplit(":", 1)[0]
    return host.rstrip(".")


def _host_is_allowed(host_header: str, own_names=(), extra=(),
                     own_label: str = "") -> bool:
    host = _split_host_header(host_header)
    if not host:
        # No Host header at all (HTTP/1.0 tooling). A browser — the only
        # thing DNS rebinding can drive — always sends one.
        return True
    try:
        ipaddress.ip_address(host.split("%", 1)[0])
        return True
    except ValueError:
        pass
    if "." not in host:
        return True
    if host in _BUILTIN_ALLOWED_HOSTS:
        return True
    if host.endswith(_LOCAL_HOST_SUFFIXES):
        return True
    if host in own_names:
        return True
    if own_label:
        first, _, suffix = host.partition(".")
        if first == own_label.lower() and ("." + suffix) in _ROUTER_DNS_SUFFIXES:
            return True
    for entry in extra:
        if entry.startswith("."):
            if host.endswith(entry) or host == entry[1:]:
                return True
        elif host == entry:
            return True
    return False


def _own_host_names() -> frozenset:
    names = set()
    for fn in (socket.gethostname, socket.getfqdn):
        try:
            n = (fn() or "").strip().lower().rstrip(".")
        except Exception:
            n = ""
        if n:
            names.add(n)
            names.add(n.split(".", 1)[0] + ".local")
    return frozenset(names)


def _own_host_label() -> str:
    """This box's hostname, first label only, lowercase ("" if unknown)."""
    try:
        return (socket.gethostname() or "").strip().lower().split(".", 1)[0]
    except Exception:
        return ""


def _extra_allowed_hosts() -> tuple:
    raw = os.getenv("MAGIC_ALLOWED_HOSTS", "")
    return tuple(h.strip().lower().rstrip(".")
                 for h in raw.split(",") if h.strip())


def _is_within(child, parent) -> bool:
    """True iff `child` is `parent` itself or a descendant of it.

    Uses the resolved-path parent relationship, NOT a string prefix.
    `str(x).startswith(str(y))` is a classic CWE-22 containment bypass:
    "/data/media_backup/secret" startswith "/data/media" is True even
    though media_backup is a SIBLING of media, not inside it — so a
    crafted <path:...> could reach files outside the intended tree the
    moment any such sibling directory exists. Path.is_relative_to()
    (Py3.9+) compares path components, so siblings never match."""
    try:
        child = Path(child).resolve()
        parent = Path(parent).resolve()
        return child == parent or parent in child.parents
    except Exception:
        return False


def register(app, ctx) -> None:
    """Register this module's routes on `app`. Called exactly once, from
    admin.create_app(); `ctx` carries the create_app() state these routes
    share with the rest of the Content Manager (see admin.py)."""

    # DNS-rebinding defence — see _host_is_allowed. Registered first so it
    # runs ahead of every other hook, including the phone-remote WebSocket
    # upgrade (flask-sock routes are ordinary Flask routes).
    _own_names = _own_host_names()
    _own_label = _own_host_label()
    _extra_hosts = _extra_allowed_hosts()

    @app.before_request
    def _check_host_header():  # type: ignore[no-redef]
        host = request.headers.get("Host", "")
        if _host_is_allowed(host, _own_names, _extra_hosts, _own_label):
            return None
        return error_response(
            "FORBIDDEN_HOST",
            "Open the Content Manager by the box's address (its IP, "
            "<name>.local, or http://dingus.box over USB).",
            status=403)

    # Cross-site request defence — the half the Host check cannot cover.
    # The allowlist must accept IP literals (the pairing QR and the Connect
    # screen are the LAN IP), so any website could still make its visitor's
    # browser hit http://<box-ip>:5000/...: spawn `update.sh check` and burn
    # the GitHub rate limit shared by the whole household, mint CSRF tokens,
    # or spend the pairing attempt budget with /?pair=000000. Browsers stamp
    # every request with Sec-Fetch-Site and a page cannot forge it, so refuse
    # 'cross-site'. Zero friction for real use:
    #   * header absent  -> allowed (curl, Retro Ripper, Safari < 16.4)
    #   * 'none'         -> allowed (camera-app QR scan, typed address,
    #                       home-screen app launch, bookmark)
    #   * same-origin / same-site -> allowed (the Content Manager itself)
    #   * a cross-site TOP-LEVEL navigation to a page (a link in a router's
    #     device list, a help article) still opens it — the user can see
    #     it; only embedding (iframe), fetch/XHR, forms and subresources are
    #     refused. One carrying a pairing code is bounced to the Connect
    #     page (admin_interface) so the code is spent only by a tap there.
    @app.before_request
    def _check_fetch_site():  # type: ignore[no-redef]
        if request.headers.get("Sec-Fetch-Site", "").lower() != "cross-site":
            return None
        if (request.method in ("GET", "HEAD")
                and request.headers.get("Sec-Fetch-Mode", "").lower() == "navigate"
                and request.headers.get("Sec-Fetch-Dest", "document").lower() == "document"):
            return None
        return error_response(
            "CROSS_SITE_REQUEST",
            "This request came from another website and was refused. Open "
            "the Content Manager directly by the box's address.",
            status=403)

    # Optional simple token auth for admin APIs (disabled by default)
    _admin_token = os.getenv("MAGIC_ADMIN_TOKEN")
    if _admin_token:
        @app.before_request
        def _require_token():  # type: ignore[no-redef]
            # Allow static assets without token
            if request.path.startswith("/static/"):
                return None
            if request.headers.get("X-Magic-Token") != _admin_token:
                return {"error": "unauthorized"}, 401

    # CSRF protection decorator for state-changing operations
    def require_csrf(f):
        """Decorator to require valid CSRF token for state-changing requests."""
        @wraps(f)
        def decorated_function(*args, **kwargs):
            # Skip CSRF check if CSRF is disabled (for development/testing)
            if os.getenv("MAGIC_DISABLE_CSRF"):
                return f(*args, **kwargs)

            token = request.headers.get("X-CSRF-Token")
            if not _validate_csrf_token(token):
                return error_response("CSRF_ERROR", "Invalid or missing CSRF token", status=403)
            return f(*args, **kwargs)
        return decorated_function

    # ===== CSRF TOKEN ENDPOINT =====

    @app.get("/admin/csrf-token")
    def get_csrf_token():  # type: ignore[no-redef]
        """Get a new CSRF token for state-changing requests."""
        token = _generate_csrf_token()
        return success_response(data={"token": token})

    # Published for the route modules registered after this one
    ctx.require_csrf = require_csrf
