"""services/.env is sourced by bash as root (verify_services.sh,
import_library_movies.sh) and several of its values come from the LAN (the
Media Browser setup form's `country`, the uploaded WireGuard .conf). A value
written raw could therefore run commands as root, and even a legitimate
"United States" made bash try to execute `States`. These tests pin the
serializer that stands between request data and that file, and prove the
written lines are inert when bash actually sources them.
"""
import shutil
import subprocess

import pytest

from magic_dingus_box.web.admin import _format_env_line, _unquote_env_value


@pytest.mark.parametrize("key,value,line", [
    ("WIREGUARD_PRIVATE_KEY", "cGxhY2Vob2xkZXI+/==", "WIREGUARD_PRIVATE_KEY=cGxhY2Vob2xkZXI+/=="),
    ("WIREGUARD_ADDRESSES", "10.2.0.2/32", "WIREGUARD_ADDRESSES=10.2.0.2/32"),
    ("TZ", "America/Argentina/Buenos_Aires", "TZ=America/Argentina/Buenos_Aires"),
    ("VPN_COUNTRIES", "", "VPN_COUNTRIES="),
    ("VPN_COUNTRIES", "United States", 'VPN_COUNTRIES="United States"'),
    ("WIREGUARD_ADDRESSES", "10.2.0.2/32, 10.3.0.2/32", 'WIREGUARD_ADDRESSES="10.2.0.2/32, 10.3.0.2/32"'),
])
def test_legitimate_values_serialize(key, value, line):
    assert _format_env_line(key, value) == line


@pytest.mark.parametrize("value", [
    "Netherlands;touch /tmp/pwned",
    "Netherlands$(id)",
    "Netherlands`id`",
    "Netherlands\nEVIL=1",
    'Nether"lands',
    "Nether'lands",
    "a|b", "a&b", "a>b", "a\\b",
])
def test_shell_metacharacters_are_refused(value):
    with pytest.raises(ValueError):
        _format_env_line("VPN_COUNTRIES", value)


def test_bad_keys_are_refused():
    with pytest.raises(ValueError):
        _format_env_line("A;B", "x")


def test_quoting_round_trips():
    line = _format_env_line("VPN_COUNTRIES", "United States")
    assert _unquote_env_value(line.partition("=")[2]) == "United States"
    assert _unquote_env_value("plain") == "plain"


@pytest.mark.skipif(shutil.which("bash") is None, reason="needs bash")
def test_written_file_is_inert_when_sourced_by_bash(tmp_path):
    env = tmp_path / ".env"
    env.write_text("\n".join([
        _format_env_line("VPN_COUNTRIES", "United States"),
        _format_env_line("WIREGUARD_ADDRESSES", "10.2.0.2/32, 10.3.0.2/32"),
        _format_env_line("TZ", "Europe/Amsterdam"),
    ]) + "\n")
    out = subprocess.run(
        ["bash", "-c", 'set -eu; set -a; . "$1"; set +a; '
                       'printf "%s|%s|%s" "$VPN_COUNTRIES" "$WIREGUARD_ADDRESSES" "$TZ"',
         "_", str(env)],
        capture_output=True, text=True, check=True)
    assert out.stdout == "United States|10.2.0.2/32, 10.3.0.2/32|Europe/Amsterdam"
    assert out.stderr == ""
