"""Apply the owner's browser time zone to the box.

The Content Manager page reports `Intl.DateTimeFormat().resolvedOptions()
.timeZone`; the route in admin_system.py validates it here, applies it with
`sudo -n timedatectl set-timezone`, rewrites services/.env's TZ= line (when
that file exists) and pokes the kiosk to `tzset()` — except that a UTC report
never moves a box off a named zone (keeps_named_zone: privacy modes report
UTC). Route-independent helpers (they import the .env helpers from
vpn_settings, nothing request-bound), so they are unit-tested on the Mac.
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path
from typing import Optional

try:
    from admin_vpn_config import _format_env_line
    from vpn_settings import atomic_replace, rewrite_env_text
except ImportError:  # pragma: no cover - package-relative in production
    from .admin_vpn_config import _format_env_line
    from .vpn_settings import atomic_replace, rewrite_env_text

ZONEINFO_DIR = Path("/usr/share/zoneinfo")
_ZONE_RE = re.compile(r"^[A-Za-z_]+(/[A-Za-z0-9_+-]+){0,2}$")
_MAX_ZONE_LEN = 64


def valid_zone(name, zoneinfo_dir: Path = ZONEINFO_DIR) -> bool:
    """True when `name` is a plausible zone name AND an existing regular file
    under `zoneinfo_dir` once resolved (so `..` or a symlink cannot escape).
    The name comes from a browser: never trust it further than this."""
    if not isinstance(name, str) or len(name) > _MAX_ZONE_LEN:
        return False
    # `$` also matches before a trailing newline; fullmatch does not.
    if not _ZONE_RE.fullmatch(name):
        return False
    try:
        base = Path(zoneinfo_dir).resolve()
        target = (base / name).resolve()
        if base not in target.parents:
            return False
        return target.is_file()
    except (OSError, ValueError):
        return False


# Browsers (Chrome) still report some legacy names; Debian Trixie moved those
# links into tzdata-legacy, so on a box without it the name is not a file.
LEGACY_ZONE_ALIASES = {
    "Asia/Calcutta": "Asia/Kolkata",
    "Europe/Kiev": "Europe/Kyiv",
    "Asia/Saigon": "Asia/Ho_Chi_Minh",
    "Asia/Katmandu": "Asia/Kathmandu",
    "Asia/Rangoon": "Asia/Yangon",
    "Atlantic/Faeroe": "Atlantic/Faroe",
    "America/Buenos_Aires": "America/Argentina/Buenos_Aires",
    "Asia/Dacca": "Asia/Dhaka",
    "Asia/Thimbu": "Asia/Thimphu",
    "Pacific/Ponape": "Pacific/Pohnpei",
    "Pacific/Truk": "Pacific/Chuuk",
}


def resolve_zone(name, zoneinfo_dir: Path = ZONEINFO_DIR) -> Optional[str]:
    """The zone to apply for a browser-reported `name`: the name itself when
    it is a valid zoneinfo file, else its canonical alias when that is, else
    None."""
    if valid_zone(name, zoneinfo_dir):
        return name
    alias = LEGACY_ZONE_ALIASES.get(name) if isinstance(name, str) else None
    if alias and valid_zone(alias, zoneinfo_dir):
        return alias
    return None


# Names for UTC itself (zero offset, no DST). Etc/GMT+5 and friends are real
# fixed offsets and are NOT in here.
UTC_ZONE_NAMES = frozenset({
    "UTC", "Etc/UTC", "GMT", "Etc/GMT", "UCT", "Etc/UCT", "Universal",
    "Etc/Universal", "Zulu", "Etc/Zulu", "GMT0", "Etc/GMT0", "GMT+0",
    "Etc/GMT+0", "GMT-0", "Etc/GMT-0", "Greenwich", "Etc/Greenwich",
})


def is_utc_zone(name) -> bool:
    """True when `name` is a name for UTC itself."""
    return isinstance(name, str) and name in UTC_ZONE_NAMES


def keeps_named_zone(reported: str, current: Optional[str]) -> bool:
    """True when a browser's UTC must not replace the box's zone: privacy
    modes (Firefox resistFingerprinting, Tor, Brave strict) report UTC
    whatever the owner's real zone is, so a UTC report says nothing about a
    box already on a named (non-UTC) zone. With the box's zone unknown, or
    already UTC, the report is applied as any other."""
    return is_utc_zone(reported) and bool(current) and not is_utc_zone(current)


def env_tz(env_path: Path) -> Optional[str]:
    """The `TZ=` value in the .env file, or None (absent file/line)."""
    try:
        text = Path(env_path).read_text(encoding="utf-8")
    except OSError:
        return None
    value = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        if k.strip() == "TZ":
            v = v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                v = v[1:-1]
            value = v  # a later duplicate wins in compose, so it wins here
    return value


def read_system_timezone() -> Optional[str]:
    """The box's current zone (`timedatectl show -p Timezone --value`), or
    None when it cannot be read."""
    try:
        result = subprocess.run(
            ["timedatectl", "show", "-p", "Timezone", "--value"],
            capture_output=True, text=True, timeout=5,
        )
        return result.stdout.strip() or None
    except Exception:  # noqa: BLE001
        return None


def set_system_timezone(zone: str) -> tuple[bool, str]:
    """`sudo -n timedatectl set-timezone <zone>` (argv, never a shell).
    Returns (ok, stderr-or-error text). Caller must have run valid_zone()."""
    try:
        res = subprocess.run(
            ["sudo", "-n", "timedatectl", "set-timezone", zone],
            capture_output=True, text=True, timeout=10,
        )
    except Exception as e:  # noqa: BLE001
        return False, str(e)
    return res.returncode == 0, (res.stderr or "").strip()


def rewrite_env_tz(env_path: Path, zone: str) -> bool:
    """Set `TZ=<zone>` in the .env file (replace the line or append one),
    atomically and keeping the file's mode. False when the file is absent."""
    env_path = Path(env_path)
    if not env_path.is_file():
        return False
    text = env_path.read_text(encoding="utf-8")
    atomic_replace(env_path, rewrite_env_text(text, _format_env_line("TZ", zone), key="TZ"))
    return True
