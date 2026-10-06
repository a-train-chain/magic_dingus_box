"""VPN server country setting (Media Browser > Advanced).

The owner's tunnel was pinned to Netherlands servers with no way to move it
short of hand-editing services/.env. These pin the contract of
vpn_settings.py: a curated list only, ONLY the VPN_COUNTRIES line changes
(atomically, mode kept), ONLY gluetun is recreated (detached, single-flight
with OTA / setup), `custom` boxes are read-only, the usual Media Browser
gates + CSRF + cross-site refusal apply, and a Reconfigure keeps the chosen
country instead of resetting it to the Netherlands.
"""
from __future__ import annotations

import json
import os
import stat
import time

import pytest

import admin
import vpn_settings
from admin import _format_env_line, create_app

PROTON_ENV = (
    "# Media Browser services — managed by the Content Manager\n"
    "PUID=1000\n"
    "QBITTORRENT_ADMIN_PASSWORD=keep-me-secret\n"
    "WIREGUARD_PRIVATE_KEY=cGxhY2Vob2xkZXJwbGFjZWhvbGRlcnBsYWNlaG9sZGVyMDA=\n"
    "VPN_SERVICE_PROVIDER=protonvpn\n"
    "VPN_COUNTRIES=Netherlands\n"
    "VPN_PORT_FORWARDING=on\n"
    "TZ=America/New_York\n"
)

PROTON_CONF = """[Interface]
# Key for magic box
PrivateKey = cGxhY2Vob2xkZXJwbGFjZWhvbGRlcnBsYWNlaG9sZGVyMDA=
Address = 10.2.0.2/32
DNS = 10.2.0.1

[Peer]
PublicKey = cHViYmxpY3BsYWNlaG9sZGVycHViYmxpY3BsYWNlaG8wMDA=
AllowedIPs = 0.0.0.0/0
Endpoint = 203.0.113.10:51820
"""


@pytest.fixture
def box(tmp_path, monkeypatch):
    root = tmp_path / "opt"
    data_dir = root / "cpp" / "data"
    for sub in ("playlists", "media", "roms", "upload_temp"):
        (data_dir / sub).mkdir(parents=True)
    scripts = root / "cpp" / "scripts"
    scripts.mkdir(parents=True)
    recreate = scripts / "recreate_gluetun.sh"
    recreate.write_text("#!/bin/bash\necho '[vpn-country] Reconnecting the VPN…'\n"
                        "sleep \"${VPN_SLOW:-0}\"\n"
                        "echo '[vpn-country] VPN connected — exit country: Sweden.'\n")
    setup = scripts / "setup_services.sh"
    setup.write_text("#!/bin/bash\necho done\n")
    setup.chmod(0o755)
    (root / "services").mkdir()
    env = root / "services" / ".env"
    env.write_text(PROTON_ENV)
    env.chmod(0o600)
    (root / "VERSION").write_text("1.0.7\n")

    settings = tmp_path / "settings.json"
    settings.write_text(json.dumps({"playback": {"media_browser_unlocked": True}}))
    monkeypatch.setattr(admin, "MEDIA_BROWSER_SETTINGS_PATH", str(settings))
    monkeypatch.setattr(admin, "_require_nopasswd_sudo", lambda: None)
    monkeypatch.setenv("MAGIC_DISABLE_CSRF", "1")
    launches = []
    real_launch = admin.DetachedJobs.launch

    def launch(self, kind, argv, **kw):
        launches.append((kind, list(argv), kw.get("as_root")))
        # Popen-mode root jobs would need `sudo -n`; privilege is not what
        # is under test.
        return real_launch(self, kind, argv, **{**kw, "as_root": False})
    monkeypatch.setattr(admin.DetachedJobs, "launch", launch)

    def make_app():
        app = create_app(data_dir, config={"TESTING": True})
        app.config["TESTING"] = True
        return app

    return {"make_app": make_app, "env": env, "settings": settings,
            "launches": launches, "recreate": recreate}


def _wait_job(client, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        job = client.get("/admin/media-browser/vpn-country").get_json()["data"]["job"]
        if job and job["status"] != "running":
            return job
        time.sleep(0.05)
    pytest.fail("vpn-country job never finished")


# ------------------------------------------------------------------ pure


def test_every_country_is_a_safe_env_value():
    for c in vpn_settings.COUNTRIES:
        line = _format_env_line("VPN_COUNTRIES", c)
        assert line in (f"VPN_COUNTRIES={c}", f'VPN_COUNTRIES="{c}"')
    assert vpn_settings.COUNTRIES[0] == vpn_settings.DEFAULT_COUNTRY == "Netherlands"
    assert "United States" in vpn_settings.COUNTRIES


def test_rewrite_replaces_only_the_country_line():
    out = vpn_settings.rewrite_env_text(PROTON_ENV, 'VPN_COUNTRIES="United States"')
    assert out == PROTON_ENV.replace("VPN_COUNTRIES=Netherlands",
                                     'VPN_COUNTRIES="United States"')


def test_rewrite_collapses_duplicates_and_keeps_comments():
    text = "# VPN_COUNTRIES=Commented\nVPN_COUNTRIES=A\nX=1\nVPN_COUNTRIES=B\n"
    assert vpn_settings.rewrite_env_text(text, "VPN_COUNTRIES=Sweden") == \
        "# VPN_COUNTRIES=Commented\nVPN_COUNTRIES=Sweden\nX=1\n"


def test_rewrite_appends_when_absent_even_without_trailing_newline():
    assert vpn_settings.rewrite_env_text("A=1", "VPN_COUNTRIES=Sweden") == \
        "A=1\nVPN_COUNTRIES=Sweden\n"


def test_custom_provider_is_read_only():
    st = vpn_settings.country_state({"VPN_SERVICE_PROVIDER": "custom", "VPN_COUNTRIES": ""})
    assert st["editable"] is False and "Reconfigure" in st["reason"]
    # Absent provider == compose's default, protonvpn.
    assert vpn_settings.country_state({})["editable"] is True


# ---------------------------------------------------------------- routes


def test_get_reports_current_country_and_choices(box):
    client = box["make_app"]().test_client()
    r = client.get("/admin/media-browser/vpn-country")
    assert r.status_code == 200
    d = r.get_json()["data"]
    assert d["country"] == "Netherlands"
    assert d["editable"] is True
    assert d["choices"] == list(vpn_settings.COUNTRIES)
    assert d["job"] is None


def test_change_rewrites_one_line_and_recreates_only_gluetun(box):
    client = box["make_app"]().test_client()
    r = client.post("/admin/media-browser/vpn-country", json={"country": "United States"})
    assert r.status_code == 200, r.get_json()
    d = r.get_json()["data"]
    assert d["changed"] is True and d["job_id"]

    assert box["env"].read_text() == PROTON_ENV.replace(
        "VPN_COUNTRIES=Netherlands", 'VPN_COUNTRIES="United States"')
    assert stat.S_IMODE(os.stat(box["env"]).st_mode) == 0o600
    assert box["launches"] == [("vpn-country", ["/bin/bash", str(box["recreate"])], True)]

    job = _wait_job(client)
    assert job["status"] == "success"
    assert any("exit country: Sweden" in l for l in job["log"])
    assert client.get("/admin/media-browser/vpn-country").get_json()["data"]["country"] \
        == "United States"


def test_same_country_is_a_no_op(box):
    client = box["make_app"]().test_client()
    r = client.post("/admin/media-browser/vpn-country", json={"country": "Netherlands"})
    assert r.status_code == 200
    assert r.get_json()["data"]["changed"] is False
    assert box["launches"] == []
    assert box["env"].read_text() == PROTON_ENV


@pytest.mark.parametrize("bad", ["Narnia", "", "netherlands", "Netherlands;id",
                                 "Afghanistan", None, ["Sweden"]])
def test_only_listed_countries_are_accepted(box, bad):
    client = box["make_app"]().test_client()
    r = client.post("/admin/media-browser/vpn-country", json={"country": bad})
    assert r.status_code == 400
    assert box["env"].read_text() == PROTON_ENV
    assert box["launches"] == []


def test_custom_provider_box_refuses(box):
    box["env"].write_text(PROTON_ENV.replace("protonvpn", "custom")
                          .replace("VPN_COUNTRIES=Netherlands", "VPN_COUNTRIES="))
    before = box["env"].read_text()
    client = box["make_app"]().test_client()
    d = client.get("/admin/media-browser/vpn-country").get_json()["data"]
    assert d["editable"] is False
    r = client.post("/admin/media-browser/vpn-country", json={"country": "Sweden"})
    assert r.status_code == 409
    assert box["env"].read_text() == before
    assert box["launches"] == []


def test_refused_while_other_maintenance_runs(box, monkeypatch):
    monkeypatch.setattr(admin.DetachedJobs, "active",
                        lambda self, kinds: ("f" * 32, "mb-setup"))
    client = box["make_app"]().test_client()
    r = client.post("/admin/media-browser/vpn-country", json={"country": "Sweden"})
    assert r.status_code == 409
    assert box["env"].read_text() == PROTON_ENV


def test_a_running_change_blocks_media_browser_setup(box, monkeypatch):
    monkeypatch.setenv("VPN_SLOW", "2")
    client = box["make_app"]().test_client()
    assert client.post("/admin/media-browser/vpn-country",
                       json={"country": "Sweden"}).status_code == 200
    r = client.post("/admin/media-browser/setup",
                    data={"config_text": PROTON_CONF, "provider": "protonvpn"})
    assert r.status_code == 409
    assert "VPN country" in r.get_json()["error"]["message"]
    _wait_job(client)


def test_failed_launch_restores_the_env(box, monkeypatch):
    def boom(self, kind, argv, **kw):
        raise OSError("no systemd-run")
    monkeypatch.setattr(admin.DetachedJobs, "launch", boom)
    client = box["make_app"]().test_client()
    r = client.post("/admin/media-browser/vpn-country", json={"country": "Sweden"})
    assert r.status_code == 500
    assert box["env"].read_text() == PROTON_ENV


def test_gated_like_every_media_browser_route(box):
    client = box["make_app"]().test_client()
    box["settings"].write_text(json.dumps({"playback": {"media_browser_unlocked": False}}))
    assert client.get("/admin/media-browser/vpn-country").status_code == 403
    assert client.post("/admin/media-browser/vpn-country",
                       json={"country": "Sweden"}).status_code == 403
    box["settings"].write_text(json.dumps({"playback": {"media_browser_unlocked": True}}))
    box["env"].write_text(PROTON_ENV.replace(
        "WIREGUARD_PRIVATE_KEY=cGxhY2Vob2xkZXJwbGFjZWhvbGRlcnBsYWNlaG9sZGVyMDA=",
        "WIREGUARD_PRIVATE_KEY="))
    r = client.post("/admin/media-browser/vpn-country", json={"country": "Sweden"})
    assert r.status_code == 403
    assert r.get_json()["error"]["code"] == "vpn_not_configured"


def test_csrf_and_cross_site_are_refused(box, monkeypatch):
    monkeypatch.delenv("MAGIC_DISABLE_CSRF")
    client = box["make_app"]().test_client()
    r = client.post("/admin/media-browser/vpn-country", json={"country": "Sweden"})
    assert r.status_code == 403
    assert r.get_json()["error"]["code"] == "CSRF_ERROR"
    token = client.get("/admin/csrf-token").get_json()["data"]["token"]
    r = client.post("/admin/media-browser/vpn-country", json={"country": "Sweden"},
                    headers={"X-CSRF-Token": token, "Sec-Fetch-Site": "cross-site"})
    assert r.status_code == 403
    assert r.get_json()["error"]["code"] == "CROSS_SITE_REQUEST"
    assert box["env"].read_text() == PROTON_ENV
    r = client.post("/admin/media-browser/vpn-country", json={"country": "Sweden"},
                    headers={"X-CSRF-Token": token, "Sec-Fetch-Site": "same-origin"})
    assert r.status_code == 200, r.get_json()
    _wait_job(client)


def test_reconfigure_keeps_the_chosen_country(box):
    box["env"].write_text(PROTON_ENV.replace("VPN_COUNTRIES=Netherlands",
                                             'VPN_COUNTRIES="United States"'))
    client = box["make_app"]().test_client()
    r = client.post("/admin/media-browser/setup",
                    data={"config_text": PROTON_CONF, "provider": "protonvpn"})
    assert r.status_code == 200, r.get_json()
    assert 'VPN_COUNTRIES="United States"' in box["env"].read_text()


def test_first_setup_still_defaults_to_netherlands(box):
    box["env"].write_text("WIREGUARD_PRIVATE_KEY=\n")
    client = box["make_app"]().test_client()
    r = client.post("/admin/media-browser/setup",
                    data={"config_text": PROTON_CONF, "provider": "protonvpn"})
    assert r.status_code == 200, r.get_json()
    assert "VPN_COUNTRIES=Netherlands" in box["env"].read_text()
