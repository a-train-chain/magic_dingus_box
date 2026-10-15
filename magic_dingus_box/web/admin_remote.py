"""Page-serving and Phone Remote routes: the SPA (/ and /admin, which also
complete ?pair= and ?device_token= pairing), /connect, the dynamic web-app
manifests, /admin/remote (+ name, protected_check, the debug press and the
WebSocket), /api/host-info and /static/*.
"""
from __future__ import annotations

import json
import re
from pathlib import Path
from urllib.parse import urlencode

from flask import (
    jsonify, redirect, render_template_string, request, send_file,
    send_from_directory,
)

# Same dual import form as admin.py: flat when tests put web/ on
# sys.path, package-relative in production.
try:
    from admin_common import error_response, success_response
    from remote import (
        auth as remote_auth, devices as remote_devices, ws_handler,
    )
    from remote.uinput_writer import UinputWriter
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import error_response, success_response
    from .remote import (
        auth as remote_auth, devices as remote_devices, ws_handler,
    )
    from .remote.uinput_writer import UinputWriter


NICKNAME_PROMPT_HTML = """
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, maximum-scale=1, user-scalable=no">
<!-- Matches the faceplate's BOTTOM edge (#131013), not --bg. iOS paints the
     region outside the web view with theme-color, and on a standalone launch
     that region is real: the layout viewport comes up ~59px short of the
     screen. The strip cannot be drawn into (only the root BACKGROUND
     propagates past the viewport, not content or borders), so the only way to
     hide the seam is to make iOS paint it the same colour the faceplate ends
     on. The bottom radial gradient darkens #1F191F to #131013 there. -->
<meta name="theme-color" content="#131013">
<title>Name your remote</title>
<style>
  * { box-sizing: border-box; }
  html, body {
    margin: 0; padding: 0; min-height: 100vh;
    background: #1F191F; color: #F2E4D9;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
    display: flex; align-items: center; justify-content: center;
  }
  .card {
    width: min(360px, 90%); padding: 32px 24px;
    background: #2A232A; border-radius: 16px;
    text-align: center;
  }
  h1 { margin: 0 0 8px; font-size: 22px; font-weight: 600; }
  p.sub { margin: 0 0 24px; font-size: 13px; color: #968B85; }
  input {
    width: 100%; padding: 14px 12px;
    background: #1F191F; color: #F2E4D9;
    border: 1px solid #968B85; border-radius: 10px;
    font-size: 16px; text-align: center; margin-bottom: 16px;
  }
  button {
    width: 100%; padding: 14px;
    background: #F5BF42; color: #1F191F;
    border: none; border-radius: 10px;
    font-size: 16px; font-weight: 700;
    cursor: pointer;
  }
  button:active { filter: brightness(0.9); }
</style>
</head>
<body>
<form class="card" method="post">
  <h1>&#10003; Paired</h1>
  <p class="sub">What should we call this remote?</p>
  <input name="nickname" placeholder="{{ placeholder }}" autofocus
         autocomplete="off" autocapitalize="words" maxlength="40">
  <button type="submit">Continue</button>
</form>
</body></html>
"""


CONNECT_PAGE_HTML = """
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#131013">
<title>Connect a Device</title>
<style>
  /* Standalone page — matches the dark styling of the inline pair page
     (/admin/remote) rather than style.css, and carries its own copy of the
     mobile overflow hygiene rules: this page never loads style.css, so a
     long hostname or query string must not push the layout wide here
     either. */
  * { box-sizing: border-box; min-width: 0; }
  html, body {
    margin: 0; padding: 0; min-height: 100vh;
    background: #1F191F; color: #F2E4D9;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
    display: flex; align-items: center; justify-content: center;
    overflow-wrap: anywhere; word-break: break-word;
  }
  .card {
    width: min(400px, 92%); padding: 32px 24px;
    background: #2A232A; border-radius: 16px;
    text-align: center; margin: 24px 0;
  }
  h1 { margin: 0 0 8px; font-size: 22px; font-weight: 600; }
  p.sub { margin: 0 0 26px; font-size: 14px; color: #968B85; line-height: 1.5; }
  a.big {
    display: block; width: 100%; padding: 18px 16px;
    border-radius: 12px; text-decoration: none;
    font-size: 17px; font-weight: 600; line-height: 1.3;
  }
  a.big span { display: block; margin-top: 4px; font-size: 13px; font-weight: 400; }
  a.big:active { filter: brightness(0.9); }
  .remote { background: #EA3A27; color: #FFF; margin-bottom: 14px; }
  .remote span { color: rgba(255, 255, 255, 0.75); }
  .manage { background: #1F191F; color: #F2E4D9; border: 2px solid #4A414A; }
  .manage span { color: #968B85; }
  p.hint { margin: 22px 0 0; font-size: 13px; color: #968B85; line-height: 1.5; }
</style>
</head>
<body>
<div class="card">
  <h1>Connect a Device</h1>
  <p class="sub">This is your Magic Dingus Box. What would you like to do?</p>
  <a class="big remote" href="{{ remote_href }}">Use this phone as a remote
    <span>D-pad control, and type with your phone&rsquo;s keyboard</span></a>
  <a class="big manage" href="/">Manage movies &amp; playlists
    <span>Upload videos, build playlists, movie setup</span></a>
  {% if has_code %}
  <p class="hint">Choosing the remote pairs this phone automatically &mdash;
     no code to type.</p>
  {% else %}
  <p class="hint">Pairing a remote needs the 6-digit code from
     Settings &rarr; Connect a Device on the kiosk.</p>
  {% endif %}
  <p class="hint">On a laptop with a USB-C cable? Any address works &mdash;
     try <strong>http://dingus.box</strong></p>
</div>
</body></html>
"""


def register(app, ctx) -> None:
    """Register this module's routes on `app`. Called exactly once, from
    admin.create_app(); `ctx` carries the create_app() state these routes
    share with the rest of the Content Manager (see admin.py)."""
    # create_app() state shared across route modules
    sock = ctx.sock

    # ============= Phone Remote — debug endpoint =============
    # Curl-driven smoke test: POST /admin/remote/_debug/press?btn=OK&phase=tap
    # Auth is intentionally absent here — Phase C adds the real /admin/remote/ws
    # which is HMAC-cookie gated. This endpoint stays available for diagnostics.
    @app.route("/admin/remote/_debug/press", methods=["POST"])
    def remote_debug_press():
        from flask import abort
        cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
        if remote_auth.verify_cookie(cookie) is None:
            abort(401)
        btn = request.args.get("btn", "")
        phase = request.args.get("phase", "tap")
        writer = app.config.get("UINPUT_WRITER")
        if writer is None:
            try:
                writer = UinputWriter()  # opens real /dev/uinput
                app.config["UINPUT_WRITER"] = writer
            except Exception as e:
                return error_response("uinput_unavailable", str(e), status=503)
        try:
            writer.press(btn, phase=phase)
        except ValueError as e:
            return error_response("bad_button", str(e))
        return success_response({"sent": btn})

    # Phone Remote — WebSocket endpoint (auth via mdb_remote cookie).
    if sock is not None:
        @sock.route("/admin/remote/ws")
        def remote_ws(ws):
            writer = app.config.get("UINPUT_WRITER")
            if writer is None:
                try:
                    writer = UinputWriter()
                    app.config["UINPUT_WRITER"] = writer
                except Exception:
                    # Best-effort: send an error and close. The phone will retry.
                    try:
                        ws.send(json.dumps({"t": "error",
                                            "code": "uinput_unavailable"}))
                        ws.close()
                    except Exception:
                        pass
                    return
            ws_handler.handle_connection(
                ws,
                uinput_writer=writer,
                text_input_writer=app.config["TEXT_INPUT_WRITER"],
                data_dir=Path(app.config["DATA_DIR"]),
                verify_cookie=remote_auth.verify_cookie,
            )
    else:
        import warnings
        warnings.warn(
            "flask-sock not installed; /admin/remote/ws WebSocket endpoint is unavailable.",
            RuntimeWarning,
            stacklevel=3,  # create_app()'s caller (raised from register())
        )

    # ===== SERVE WEB INTERFACE =====

    @app.get("/api/host-info")
    def host_info():  # type: ignore[no-redef]
        """The box's own addresses, for the install-coaching toast.

        An installed home-screen app is pinned to the origin it was
        installed from, permanently. If the phone arrived via the pairing
        QR it is standing on a raw DHCP IP, and an icon made there breaks
        at the next lease change. The toast uses this to offer the stable
        <hostname>.local address instead.
        """
        import socket
        try:
            host = socket.gethostname()
        except Exception:
            host = ""
        mdns = (host + ".local") if host and not host.endswith(".local") else host
        return jsonify({"hostname": host, "mdns": mdns})

    @app.get("/connect")
    def connect_landing():  # type: ignore[no-redef]
        """Unified "Connect a Device" landing page — the kiosk QR target.

        The kiosk's Settings menu used to point two different QR codes at
        two different URLs (the Content Manager root, and the phone-remote
        pairing flow) and customers could not tell which one they wanted.
        Both kiosk QR codes now land HERE, and the choice is made in words.

        Accepts an optional ?code=NNNNNN from the pairing screen's QR. With
        a well-formed code the remote button submits it through the EXISTING
        pairing flow (GET /?pair=CODE&tab=remote — handle_pair_param lives
        on the root route; there is no /pair route). Without one, the button
        goes to /admin/remote, which already shows the in-app 6-digit form
        when unpaired.

        This page deliberately adds NO new pairing mechanics: iOS
        home-screen apps have a separate cookie jar from Safari and depend
        on the in-app 6-digit form, so the /admin/remote form, the
        handle_pair_param flow, and the HMAC cookie are all untouched — this
        is only a signpost in front of them. A malformed code is treated as
        absent rather than rejected: the customer still gets a working page
        and the in-app form as the fallback path.
        """
        code = (request.args.get("code") or "").strip()
        if not re.fullmatch(r"[0-9]{6}", code):
            code = ""
        remote_href = f"/?pair={code}&tab=remote" if code else "/admin/remote"
        return render_template_string(
            CONNECT_PAGE_HTML, remote_href=remote_href, has_code=bool(code))

    @app.get("/")
    @app.get("/admin")
    def admin_interface():  # type: ignore[no-redef]
        """Serve the web interface.

        If ?pair=<code> is present, delegate to the phone-remote pairing flow
        before serving the static SPA so that the kiosk QR-code link is handled
        transparently.

        If ?device_token= is present (an installed Content Manager app's
        start_url, planted there by the dynamic root manifest), redeem it
        into the mdb_remote cookie so the SPA's Remote tab — a same-origin
        iframe of /admin/remote, or a same-origin navigation on phones —
        opens already paired. Redeeming on EVERY launch (even with a live
        cookie) is deliberate: it rolls the cookie's issue time forward and
        heals a jar that iOS evicted. The 303 re-serves the same path with
        device_token stripped but every other query param preserved. An
        invalid or revoked token just proceeds to the SPA unauthenticated —
        no error, no signal about token validity.
        """
        pair_code = request.args.get("pair")
        if pair_code:
            if request.headers.get("Sec-Fetch-Site", "").lower() == "cross-site":
                # A pairing code arriving by a link on ANOTHER website is
                # never the kiosk's QR (a camera scan is Sec-Fetch-Site:
                # none) — it is how a hostile page would spend the attempt
                # budget. Show the Connect page instead: its button submits
                # the same code same-origin, so a genuine link costs one tap.
                code = pair_code.strip()
                target = (f"/connect?code={code}"
                          if re.fullmatch(r"[0-9]{6}", code) else "/connect")
                return redirect(target, code=303)
            return remote_auth.handle_pair_param(pair_code)
        submitted_token = request.args.get("device_token")
        if submitted_token:
            redeemed = remote_auth.redeem_device_token(submitted_token)
            if redeemed is not None:
                remaining = [(k, v)
                             for k, vals in request.args.lists()
                             for v in vals if k != "device_token"]
                qs = urlencode(remaining)
                resp = redirect(request.path + (f"?{qs}" if qs else ""),
                                code=303)
                remote_auth.issue_cookie(resp, redeemed)
                return resp
        static_dir = Path(__file__).parent / "static"
        return send_file(static_dir / "index.html")

    # ------------------------------------------------------------------
    # Dynamic web-app manifests (iOS home-screen install pairing).
    #
    # iOS gives an installed home-screen app a SEPARATE cookie jar from
    # Safari, so "Add to Home Screen" on a paired page used to produce an
    # app that opened UNPAIRED (the pairing cookie stayed in Safari's
    # jar). The bridge: iOS fetches the manifest at install time FROM THE
    # PAIRED SAFARI SESSION, so a request presenting a valid mdb_remote
    # cookie gets a start_url carrying that device's durable install
    # token — the token rides inside the icon, and the installed app
    # trades it for its own cookie on first launch (the redeem branches
    # in admin_interface and remote_page). An unauthenticated fetch gets
    # the same manifest with a bare start_url, which degrades to the
    # 6-digit pair form.
    #
    # TWO manifests because there are two install surfaces: the pairing
    # flow lands people on the root Content Manager SPA — where field
    # testing showed installs actually happen — and /admin/remote is the
    # standalone remote. Each embeds the SAME per-device token; only the
    # start_url/scope/identity differ.
    #
    # no-store is mandatory on both: a cached token-bearing manifest
    # served to the wrong requester would be a credential leak.
    # ------------------------------------------------------------------

    _MANIFEST_ICONS = [
        {"src": "/static/icons/icon-192.png", "sizes": "192x192",
         "type": "image/png", "purpose": "any"},
        {"src": "/static/icons/icon-512.png", "sizes": "512x512",
         "type": "image/png", "purpose": "any"},
        {"src": "/static/icons/icon-maskable-192.png", "sizes": "192x192",
         "type": "image/png", "purpose": "maskable"},
        {"src": "/static/icons/icon-maskable-512.png", "sizes": "512x512",
         "type": "image/png", "purpose": "maskable"},
    ]

    def _tokened_start_url(base: str) -> str:
        """base, plus this requester's install token when (and only when)
        the request presents a valid pairing cookie."""
        cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
        device_id = remote_auth.verify_cookie(cookie)
        if device_id is not None:
            token = remote_auth.device_token_for(device_id)
            if token:
                return f"{base}?device_token={token}"
        return base

    def _manifest_response(payload: dict):
        resp = app.response_class(
            json.dumps(payload, indent=2),
            mimetype="application/manifest+json")
        resp.headers["Cache-Control"] = "no-store"
        return resp

    # The /static/manifest.webmanifest alias is deliberate, not legacy
    # convenience: the exact rule outranks the /static/<path:filename>
    # converter rule in werkzeug's ordering, so it SHADOWS the old static
    # file's URL. Any cached page still referencing the old address gets
    # this dynamic manifest — a stale tokenless manifest cannot be served
    # by accident. The static file itself is deleted from the repo.
    @app.get("/manifest.webmanifest")
    @app.get("/static/manifest.webmanifest")
    def root_manifest():  # type: ignore[no-redef]
        """Dynamic manifest for the root Content Manager app (see the
        install-pairing comment block above). Identity preserved from the
        old static manifest — only start_url became dynamic. No forced
        tab= in start_url: someone installing the CM for management should
        land on the default tab; the token pairs the Remote tab silently."""
        return _manifest_response({
            "name": "Magic Dingus Box",
            "short_name": "Magic Dingus Box",
            "description": "Content Manager and phone remote for your "
                           "Magic Dingus Box.",
            "id": "/",
            "start_url": _tokened_start_url("/"),
            "scope": "/",
            "display": "standalone",
            "orientation": "any",
            "background_color": "#1F191F",
            "theme_color": "#131013",
            "icons": _MANIFEST_ICONS,
        })

    @app.get("/admin/remote/manifest.webmanifest")
    def remote_manifest():  # type: ignore[no-redef]
        """Dynamic manifest for the standalone phone remote (see the
        install-pairing comment block above)."""
        return _manifest_response({
            "name": "Dingus Remote",
            "short_name": "Dingus Remote",
            "description": "Phone remote for your Magic Dingus Box.",
            "id": "/admin/remote",
            "start_url": _tokened_start_url("/admin/remote"),
            "scope": "/admin/remote",
            "display": "standalone",
            "background_color": "#1F191F",
            "theme_color": "#131013",
            "icons": _MANIFEST_ICONS,
        })

    @app.route("/admin/remote", methods=["GET"])
    def remote_page():  # type: ignore[no-redef]
        cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
        device_id = remote_auth.verify_cookie(cookie)
        if device_id is None:
            # Installed-app first launch: no cookie in THIS jar yet, but the
            # icon's start_url may carry the durable install token the
            # manifest embedded at install time. Trade it for a cookie and
            # 303 to the clean URL (keeps the token out of the visible
            # URL/history). This GET is the ONLY place the token redeems.
            # An invalid/revoked token deliberately falls through to the
            # ordinary pair form — no signal about token validity.
            submitted_token = request.args.get("device_token", "")
            if submitted_token:
                redeemed = remote_auth.redeem_device_token(submitted_token)
                if redeemed is not None:
                    resp = redirect("/admin/remote", code=303)
                    remote_auth.issue_cookie(resp, redeemed)
                    return resp
            return render_template_string("""
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, maximum-scale=1, user-scalable=no">
<!-- Faceplate BOTTOM-edge colour (#131013), not --bg. iOS paints the region
     outside the web view with theme-color, and on a standalone launch that
     region is real: the layout viewport comes up ~59px short of the screen.
     It cannot be drawn into (only the root BACKGROUND propagates past the
     viewport; content and borders are clipped), so matching the colour the
     faceplate ends on is the only way to hide the seam. -->
<meta name="theme-color" content="#131013">
<!-- Same installable-app tags as the paired remote shell — this page is
     served at the SAME URL (/admin/remote), so it must reference the same
     remote-scoped manifest or install behavior would depend on pairing
     state. Unauthenticated manifest fetches carry no token, so an install
     made from here opens on this pair form — the correct degradation.
     Without these tags, adding to the home screen from THIS page (a real
     possibility, since it is where an expired pairing lands you) produces
     a generic Safari bookmark with a screenshot icon instead of the app. -->
<link rel="manifest" href="/admin/remote/manifest.webmanifest" crossorigin="use-credentials">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Dingus Remote">
<link rel="apple-touch-icon" href="/static/icons/icon-180.png">
<link rel="icon" type="image/png" sizes="32x32" href="/static/icons/icon-32.png">
<title>Remote not paired</title>
<style>
  html, body { margin: 0; padding: 0; background: #1F191F; color: #F2E4D9;
               font-family: -apple-system, system-ui, sans-serif;
               min-height: 100vh; display: flex; align-items: center; justify-content: center; }
  .card { width: min(360px, 90%); padding: 32px 24px; background: #2A232A;
          border-radius: 16px; text-align: center; }
  h1 { margin: 0 0 12px; font-size: 22px; }
  p { color: #968B85; line-height: 1.5; }
  form { margin: 22px 0 6px; }
  input[name=pair] {
    width: 100%; box-sizing: border-box; padding: 16px; font-size: 30px;
    letter-spacing: 10px; text-align: center; border-radius: 12px;
    border: 2px solid #4A414A; background: #1F191F; color: #F2E4D9;
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
  input[name=pair]:focus { outline: none; border-color: #EA3A27; }
  button {
    width: 100%; margin-top: 14px; padding: 16px; font-size: 17px;
    font-weight: 600; border: 0; border-radius: 12px;
    background: #EA3A27; color: #FFF; }
  .home { display: inline-block; margin-top: 20px; color: #968B85;
          font-size: 15px; text-decoration: none; }
  .hint { font-size: 13px; margin-top: 4px; }
</style>
</head>
<body>
<div class="card">
  <h1>Pair this remote</h1>
  <p>On the kiosk, open <strong>Settings &rarr; Connect a Device</strong> and enter the 6-digit code shown there.</p>
  <!-- A form, not just instructions. Scanning the QR opens Safari, which
       on iOS has a DIFFERENT cookie jar from an installed home-screen app
       — so a QR scan can never authenticate this app, and telling the
       user to scan it left them permanently stuck with no way out (there
       is no address bar in standalone mode). Submitting here issues the
       request from THIS jar, so the cookie lands where it is needed.
       GET to "/" because that is where handle_pair_param lives; there is
       no /pair route. -->
  <form action="/" method="get" autocomplete="off">
    <input name="pair" inputmode="numeric" pattern="[0-9]{6}" maxlength="6"
           placeholder="000000" aria-label="6-digit pairing code" autofocus>
    <input type="hidden" name="tab" value="remote">
    <button type="submit">Pair</button>
  </form>
  <p class="hint">The code changes every couple of minutes.</p>
  <a class="home" href="/">&larr; Content Manager</a>
</div>
</body></html>
""")
        # Cookie OK — serve the static remote shell
        return send_from_directory("static/remote", "remote.html")

    @app.route("/admin/remote/name", methods=["GET", "POST"])
    def remote_name():  # type: ignore[no-redef]
        """Nickname-prompt page shown immediately after a successful pair."""
        cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
        device_id = remote_auth.verify_cookie(cookie)
        if device_id is None:
            return redirect("/", code=303)

        paired_path = Path(app.config["DATA_DIR"]) / "paired_remotes.json"

        if request.method == "POST":
            nickname = (request.form.get("nickname") or "").strip()[:40] or "Phone"
            # Through devices.py, under its lock: an atomic rename alone kept
            # the file whole but not CURRENT — this read-modify-write could
            # write back a copy read before a concurrent pairing (or the
            # StatusBroadcaster's revocation reap) committed, undoing it.
            remote_devices.rename_device(paired_path, device_id, nickname)
            target = request.args.get("tab", "remote")
            return redirect(f"/?tab={target}", code=303)

        # GET — render the form. User-Agent hint becomes the placeholder.
        ua = request.headers.get("User-Agent", "")
        placeholder = "iPad" if "iPad" in ua else "iPhone" if "iPhone" in ua else "Phone"

        return render_template_string(NICKNAME_PROMPT_HTML, placeholder=placeholder)

    @app.route("/admin/remote/protected_check")
    def remote_protected_check():  # type: ignore[no-redef]
        """Debug endpoint: verify the mdb_remote cookie and return device_id."""
        cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
        device_id = remote_auth.verify_cookie(cookie)
        if device_id is None:
            return error_response("unpaired", "Not paired", status=401)
        return success_response({"device_id": device_id})

    @app.route("/static/<path:filename>")
    def serve_static(filename):  # type: ignore[no-redef]
        """Serve static assets.

        Use send_from_directory (not send_file with `static_dir / filename`)
        so Flask's safe_join enforces containment — without it, a request
        like /static/../../config/settings.json escapes the static dir and
        discloses arbitrary process-readable files.
        """
        static_dir = Path(__file__).parent / "static"
        return send_from_directory(static_dir, filename)
