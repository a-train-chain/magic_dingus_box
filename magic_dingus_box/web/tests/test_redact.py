"""The secret scrubber behind the diagnostics bundle and the health card.

Every line below is shaped like something that has (or plausibly could)
land in one of this box's logs. Each must lose its secret and, where the
rest of the line is a diagnosis, keep the rest.
"""
import pytest

from redact import REDACTED, Redactor, env_secret_values, redact_line, redact_text

QBIT_PW = "Zq8vRk2mWp4xYt7nLs3c"          # setup_services.sh shape
WG_KEY = "yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk="
ARR_KEY = "0123456789abcdef0123456789abcdef"
FLASK = "f" * 8 + "0123456789abcdef" * 3 + "a" * 8


@pytest.mark.parametrize("line,secret", [
    (f"WIREGUARD_PRIVATE_KEY={WG_KEY}", WG_KEY),
    (f"export QBITTORRENT_ADMIN_PASSWORD='{QBIT_PW}'", QBIT_PW),
    (f"MDB_QBIT_PASS={QBIT_PW}", QBIT_PW),
    (f"SONARR_API_KEY = {ARR_KEY}", ARR_KEY),
    (f"curl -H 'X-Api-Key: {ARR_KEY}' http://localhost:7878/api/v3/queue", ARR_KEY),
    (f"GET http://localhost:9696/api/v1/search?query=dune&apikey={ARR_KEY}&type=search", ARR_KEY),
    (f"https://api.themoviedb.org/3/movie/550?api_key={ARR_KEY}&language=en", ARR_KEY),
    ('qbit: POST /api/v2/auth/login username=admin&password=hunter2hunter2', "hunter2hunter2"),
    (f'{{"username": "admin", "password": "{QBIT_PW}"}}', QBIT_PW),
    (f'"apiKey": "{ARR_KEY}",', ARR_KEY),
    ("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJhdWQiOiJ4eHgiLCJzdWIiOiJ5In0.abcDEF123456789xyz",
     "eyJhbGciOiJIUzI1NiJ9"),
    ("tmdb token eyJhbGciOiJIUzI1NiJ9.eyJhdWQiOiJ4eHgiLCJzdWIiOiJ5In0.abcDEF123456789xyz loaded",
     "eyJhbGciOiJIUzI1NiJ9"),
    (f"Cookie: mdb_remote=dev1.1700000000.{FLASK}", FLASK),
    (f"PrivateKey = {WG_KEY}", WG_KEY),
    ("psk: correct-horse-battery", "correct-horse-battery"),
    ("802-11-wireless-security.psk:MyHomeWifi2024", "MyHomeWifi2024"),
    ("nmcli dev wifi connect HomeNet password hunter2", "hunter2"),
    (f"loaded secret {FLASK}", FLASK),
    (f"[radarr] request failed for key {ARR_KEY}", ARR_KEY),
])
def test_secret_is_removed(line, secret):
    out = redact_line(line)
    assert secret not in out, out


def test_value_redaction_keeps_the_diagnosis():
    out = redact_line(f"GET /api/v3/queue?apikey={ARR_KEY} -> 503 Service Unavailable")
    assert ARR_KEY not in out
    assert "503 Service Unavailable" in out
    assert REDACTED in out


def test_known_values_catch_shapeless_secrets():
    # A bare 20-char random password with nothing secret-looking around it.
    line = f"qbit-port-sync: re-login as admin/{QBIT_PW} after 403"
    assert QBIT_PW in redact_line(line, strict=False)
    r = Redactor([QBIT_PW])
    assert QBIT_PW not in r.line(line, strict=False)
    assert "after 403" in r.line(line, strict=False)


@pytest.mark.parametrize("line", [
    "Platform: Raspberry Pi 5 Model B Rev 1.0",
    "  [PASS] board: Raspberry Pi 5 Model B Rev 1.0",
    "Selected mode 1920x1080@60Hz (preferred)",
    "Loading /home/magic/.config/retroarch/cores/mupen64plus_next_libretro.so",
    "Finished magic-dingus-sync-qbit-password.service.",
    "magic-dingus-box-cpp.service: Main process exited, code=killed, status=9/KILL",
    "/dev/mmcblk0p2   29G   12G   16G  43% /",
    "Mem:  1849  912  211  25  726  937",
])
def test_ordinary_lines_survive(line):
    assert redact_line(line) == line


def test_strict_drops_unhandled_mentions_but_lenient_keeps_them():
    line = "[PASS] qBit login with .env password succeeds"
    assert redact_line(line, strict=False) == line
    assert redact_line(line, strict=True) == "[line redacted: it mentioned a password]"


def test_text_preserves_line_structure():
    text = f"a\nWIREGUARD_PRIVATE_KEY={WG_KEY}\r\nb\n"
    out = redact_text(text)
    assert out.splitlines() == ["a", f"WIREGUARD_PRIVATE_KEY={REDACTED}", "b"]
    assert out.endswith("\n")


def test_env_secret_values():
    env = "\n".join([
        "# comment",
        f"WIREGUARD_PRIVATE_KEY={WG_KEY}",
        f"QBITTORRENT_ADMIN_PASSWORD={QBIT_PW}",
        f'SONARR_API_KEY="{ARR_KEY}"',
        "TZ=America/New_York",
        "STORAGE_ROOT=/mnt/ssd",
        "PUID=1000",
        "SERVER_COUNTRIES=United States",
        "SOME_OPAQUE_THING=AbCdEf0123456789XyZ",
    ])
    vals = env_secret_values(env)
    assert WG_KEY in vals and QBIT_PW in vals and ARR_KEY in vals
    assert "AbCdEf0123456789XyZ" in vals
    for plain in ("America/New_York", "/mnt/ssd", "1000", "United States"):
        assert plain not in vals


def test_known_values_ignore_plain_words():
    r = Redactor(["enabled", "1000000", "abc"])
    assert r.line("feature enabled at 1000000", strict=False) == "feature enabled at 1000000"
