"""WireGuard / VPN-provider / services/.env helpers for the Media Browser
setup: .conf parsing, provider detection, the Gluetun env each provider
needs, and the .env reader/formatter whose charset rules keep LAN-supplied
values inert in every consumer of that file. Pure functions, no routes.
"""
from __future__ import annotations

import ipaddress
import re
import socket
from pathlib import Path


# --- VPN provider support -------------------------------------------------
#
# Everything below was verified empirically against the exact image this box
# runs (`qmcgaw/gluetun:v3` == v3.41.1) by launching throwaway containers and
# reading gluetun's own settings-validation errors. Do not "simplify" these
# rules from memory — each one corresponds to a FATAL gluetun startup error,
# and gluetun failing to start takes the whole media stack with it (radarr /
# prowlarr / qbittorrent / byparr are all `depends_on: service_healthy`).

# Default WireGuard listen port, used when a .conf's Endpoint omits one.
_WG_DEFAULT_ENDPOINT_PORT = 51820

# Gluetun's `custom` provider runs ANY standard WireGuard config. It needs
# exactly what a .conf already contains, so it is our universal fallback.
_VPN_PROVIDER_CUSTOM = "custom"

# Providers gluetun v3.41.1 accepts for VPN_PORT_FORWARDING=on. Verbatim from
# its own error text:
#   "port forwarding cannot be enabled: value is not one of the possible
#    choices: mullvad must be one of perfect privacy, private internet
#    access, privatevpn or protonvpn"
# Setting VPN_PORT_FORWARDING=on for anything else is a HARD startup failure,
# not a warning — including for `custom`.
_VPN_PORT_FORWARDING_PROVIDERS = frozenset({
    "perfect privacy",
    "private internet access",
    "privatevpn",
    "protonvpn",
})

# Of the four above, only protonvpn actually has WireGuard servers in
# gluetun's embedded server list (checked against /gluetun/servers.json:
# perfect privacy 0, private internet access 0, privatevpn 0, protonvpn 800).
# So over WireGuard — which is all this box supports — ProtonVPN is the only
# provider that can ever forward a port.
_VPN_WIREGUARD_NATIVE_PROVIDERS = frozenset({
    "airvpn", "fastestvpn", "ivpn", "mullvad",
    "nordvpn", "protonvpn", "surfshark", "windscribe",
})

# Providers we are willing to select NATIVELY on our own (i.e. from detection
# alone, with no operator confirmation). Native mode hands server choice to
# gluetun, which is only worth the extra failure surface where it unlocks
# something we need — and the only thing it unlocks here is port forwarding.
# Everything else detects to a friendly label but still RUNS as `custom`.
_VPN_AUTO_NATIVE_PROVIDERS = frozenset({"protonvpn"})


def _parse_wireguard_endpoint(endpoint: str) -> tuple[str, int]:
    """Split a WireGuard `Endpoint` into (host, port).

    Host may be an IPv4 address, an IPv6 address or a DNS name — callers that
    hand it to gluetun must resolve names first (see
    _resolve_wireguard_endpoint_ip). Port falls back to the WireGuard default
    when the endpoint omits it.
    """
    endpoint = endpoint.strip()
    port = _WG_DEFAULT_ENDPOINT_PORT
    if endpoint.startswith("["):
        # Bracketed IPv6: "[2001:db8::1]:51820"
        host, _, rest = endpoint[1:].partition("]")
        if rest.startswith(":") and rest[1:].isdigit():
            port = int(rest[1:])
    else:
        head, sep, tail = endpoint.rpartition(":")
        # "host:port" — but an unbracketed IPv6 literal is also full of
        # colons, and splitting one on its last colon would silently invent a
        # port from the final hextet. Only treat the tail as a port when what
        # is left is a single colon-free host.
        if sep and tail.isdigit() and head and ":" not in head:
            host, port = head, int(tail)
        else:
            host = endpoint
    return host.strip(), port


def _wireguard_sections(text: str) -> tuple[dict, dict]:
    """Split a WireGuard .conf into its ([Interface], [Peer]) key/value maps."""
    section = None
    interface: dict = {}
    peer: dict = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip()
        if section == "interface":
            interface[key] = value
        elif section == "peer":
            peer[key] = value
    return interface, peer


def _parse_wireguard_config(text: str) -> dict:
    """Parse a WireGuard .conf file into the vars Gluetun needs.

    Returns a dict with keys:
      WIREGUARD_PRIVATE_KEY, WIREGUARD_ADDRESSES,
      WIREGUARD_PUBLIC_KEY,  WIREGUARD_ENDPOINT_IP,
      WIREGUARD_ENDPOINT_PORT
    Raises ValueError on missing/malformed fields.

    Every key returned here is written straight to services/.env, so nothing
    that is not a real gluetun variable belongs in this dict.

    WIREGUARD_ENDPOINT_PORT is not optional trivia: gluetun's `custom`
    provider refuses to start without it ("server selection: Wireguard server
    selection settings: endpoint port is not set"). The port used to be
    discarded here, which is fine only for native providers that carry their
    own server list.
    """
    interface, peer = _wireguard_sections(text)

    missing = []
    if "PrivateKey" not in interface:
        missing.append("[Interface] PrivateKey")
    if "Address" not in interface:
        missing.append("[Interface] Address")
    if "PublicKey" not in peer:
        missing.append("[Peer] PublicKey")
    if "Endpoint" not in peer:
        missing.append("[Peer] Endpoint")
    if missing:
        raise ValueError(f"Missing required fields: {', '.join(missing)}")

    endpoint_ip, endpoint_port = _parse_wireguard_endpoint(peer["Endpoint"])
    if not endpoint_ip:
        raise ValueError("Could not parse host from [Peer] Endpoint")

    # ProtonVPN configs list dual-stack addresses
    # ("10.2.0.2/32, 2a07:b944::2:2/128"). Gluetun's container has no
    # IPv6 and hard-fails on the IPv6 entry, crash-looping the stack —
    # hit live on the first Pi 5 provisioning (2026-07-22). Keep IPv4 only.
    addresses = [a.strip() for a in interface["Address"].split(",")]
    ipv4_addresses = [a for a in addresses if a and ":" not in a]
    if not ipv4_addresses:
        raise ValueError(
            "[Interface] Address has no IPv4 entry (IPv6-only configs are "
            "not supported by the Gluetun container)")

    return {
        "WIREGUARD_PRIVATE_KEY": interface["PrivateKey"],
        "WIREGUARD_ADDRESSES": ", ".join(ipv4_addresses),
        "WIREGUARD_PUBLIC_KEY": peer["PublicKey"],
        "WIREGUARD_ENDPOINT_IP": endpoint_ip,
        "WIREGUARD_ENDPOINT_PORT": str(endpoint_port),
    }


def _detect_vpn_brand(text: str, wg: dict | None = None) -> str:
    """Best-effort brand ID for an uploaded WireGuard config.

    Returns a gluetun provider string, or "" when nothing matches. This is a
    LABEL — it does not by itself decide how the tunnel is run; see
    _vpn_provider_env. Detection is deliberately conservative: an unknown
    config falls through to "" and therefore to gluetun's `custom` provider,
    which runs any standard WireGuard file.

    Signature sources, and how far each is actually trusted:
      * Endpoint hostname suffixes are taken from gluetun's own embedded
        server list (/gluetun/servers.json in the running image), so they are
        exact for the providers gluetun knows. They only help when the .conf
        carries a hostname — ProtonVPN and Mullvad emit a bare IP instead.
      * ProtonVPN's 10.2.0.2 interface address + 10.2.0.1 DNS gateway is
        corroborated three ways: a real Proton config; this repo's own
        operational notes (the FIREWALL_OUTBOUND_SUBNETS comment in
        docker-compose.yml documents the 10.2.0.1 gateway as the reason
        10.0.0.0/8 must not be listed); and gluetun itself, which hardcodes
        10.2.0.2 as its ProtonVPN default address.
      * The remaining subnet rules are only applied where the range is
        distinctive enough that a self-hosted tunnel is unlikely to collide
        with it. IVPN (172.16.0.0/12) is deliberately NOT matched on subnet:
        that is the most commonly self-chosen private range there is, and
        IVPN configs carry a hostname endpoint anyway.

    A wrong label here is cosmetic, never functional: every brand except
    protonvpn still RUNS as `custom` (see _VPN_AUTO_NATIVE_PROVIDERS), and
    the operator can override the choice in the setup panel.
    """
    interface, peer = _wireguard_sections(text)
    if wg and wg.get("WIREGUARD_ENDPOINT_IP"):
        host = wg["WIREGUARD_ENDPOINT_IP"].strip().lower()
    else:
        host, _ = _parse_wireguard_endpoint(peer.get("Endpoint", ""))
        host = host.lower()
    address = (interface.get("Address") or "").strip().lower()
    dns = (interface.get("DNS") or "").strip().lower()

    def _in(value: str, cidr: str) -> bool:
        """True iff `value` (an Address or DNS entry) sits inside `cidr`."""
        first = value.split(",")[0].strip().split("/")[0].strip()
        if not first:
            return False
        try:
            return ipaddress.ip_address(first) in ipaddress.ip_network(cidr)
        except ValueError:
            return False

    # 1. Endpoint hostname — the strongest signal when present.
    host_suffixes = (
        (".protonvpn.net", "protonvpn"),
        (".mullvad.net", "mullvad"),
        (".wg.ivpn.net", "ivpn"),
        (".ivpn.net", "ivpn"),
        (".vpn.airdns.org", "airvpn"),
        (".airvpn.org", "airvpn"),
        (".nordvpn.com", "nordvpn"),
        (".prod.surfshark.com", "surfshark"),
        (".surfshark.com", "surfshark"),
        (".whiskergalaxy.com", "windscribe"),
        (".windscribe.com", "windscribe"),
        (".jumptoserver.com", "fastestvpn"),
    )
    for suffix, brand in host_suffixes:
        if host.endswith(suffix):
            return brand

    # 2. ProtonVPN — two of three: the 10.2.0.x address, the 10.2.0.1 DNS
    #    gateway, or one of the "# Key for" / "# NetShield" / "# NAT-PMP" /
    #    "# VPN Accelerator" headers its dashboard writes. These co-occur;
    #    they are not alternatives, so any one of them counts once.
    proton_hits = 0
    if _in(address, "10.2.0.0/24"):
        proton_hits += 1
    if _in(dns, "10.2.0.1/32"):
        proton_hits += 1
    if re.search(r"^\s*#\s*(Key for\s+\S|NetShield|NAT-PMP|VPN Accelerator)",
                 text, re.MULTILINE | re.IGNORECASE):
        proton_hits += 1
    if proton_hits >= 2:
        return "protonvpn"

    # 3. Surfshark writes a byte-identical `Address = 10.14.0.2/16` for every
    #    user and every server — the /16 on a single-peer client config is
    #    itself unusual enough to be a signature.
    if address.startswith("10.14.0.2/16"):
        return "surfshark"

    # 4. Mullvad: 10.64.0.0/10 address AND the 10.64.0.1 DNS gateway. The
    #    address is NOT fixed at 10.64.0.x — real ones include 10.69.209.105
    #    and 10.71.237.120 — so the whole /10 has to be checked.
    if _in(address, "10.64.0.0/10") and _in(dns, "10.64.0.1/32"):
        return "mullvad"

    # 5. AirVPN: 10.128.0.0/9 address with the 10.128.0.1 DNS gateway.
    if _in(address, "10.128.0.0/9") and _in(dns, "10.128.0.1/32"):
        return "airvpn"

    # 6. Windscribe: CGNAT-range address (100.64.0.0/10) with a 10.255.255.x
    #    resolver. Its hostname rule above covers the usual case.
    if _in(address, "100.64.0.0/10") and _in(dns, "10.255.255.0/24"):
        return "windscribe"

    return ""


# services/.env is consumed by bash `.`/`source` running as ROOT
# (verify_services.sh, import_library_movies.sh), by docker compose, by
# systemd EnvironmentFile= and by `grep | cut` readers — and several of its
# values arrive from the LAN (the setup form's `country`, the uploaded
# WireGuard .conf's keys and addresses). Written raw, `country=X;cmd` ran
# `cmd` as root on the next smoke test, and a legitimate "United States"
# made bash try to execute `States`. So every value is checked against a
# charset that is inert in all of those readers. A value with spaces is
# double-quoted (all four readers strip the quotes; none of the grep|cut
# readers ever reads a spaced value); anything else outside the set is
# refused rather than escaped, because no single escaping is correct for
# all four parsers.
_ENV_KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_ENV_BARE_VALUE_RE = re.compile(r"^[A-Za-z0-9_./:+=,@%-]*$")
_ENV_QUOTED_VALUE_RE = re.compile(r"^[A-Za-z0-9_./:+=,@% -]*$")


def _format_env_line(key: str, value) -> str:
    """Serialize one KEY=VALUE line for services/.env, or raise ValueError."""
    value = "" if value is None else str(value)
    if not _ENV_KEY_RE.match(key):
        raise ValueError(f"invalid .env key: {key!r}")
    if _ENV_BARE_VALUE_RE.match(value):
        return f"{key}={value}"
    if _ENV_QUOTED_VALUE_RE.match(value):
        return f'{key}="{value}"'
    raise ValueError(f"unsafe characters in .env value for {key}")


def _unquote_env_value(value: str) -> str:
    """Inverse of _format_env_line's quoting for a raw .env value."""
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


class EnvFileReadError(Exception):
    """services/.env exists but could not be read (permissions, I/O, encoding)."""


def _read_env_file(path: Path) -> dict:
    """Parse a KEY=VALUE .env file into a dict.

    Returns {} ONLY when the file does not exist. Any other failure raises
    EnvFileReadError. It used to return {} on every exception, and that was
    destructive: a root-owned 0600 .env raised PermissionError, the setup
    route took the {} as "empty .env", merged in only the WireGuard keys and
    wrote that back — erasing QBITTORRENT_ADMIN_PASSWORD and the API keys.
    setup_services.sh then generated a fresh qBit password qBittorrent did
    not have, and the kiosk, port-sync and password-sync all lost qBit auth.
    Callers that WRITE must treat the exception as "abort, write nothing".
    """
    try:
        text = Path(path).read_text()
    except FileNotFoundError:
        return {}
    except (OSError, UnicodeDecodeError) as e:
        raise EnvFileReadError(f"Could not read {path}: {e}") from e
    result = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        result[key.strip()] = _unquote_env_value(value.strip())
    return result


def _vpn_provider_env(provider: str, wg: dict, country: str = "") -> dict:
    """Return the VPN_*/WIREGUARD_ENDPOINT_* env block for a chosen provider.

    `provider` is a gluetun VPN_SERVICE_PROVIDER value; anything not natively
    supported over WireGuard is coerced to `custom`. The returned dict is
    applied AUTHORITATIVELY (it overwrites whatever the .env had), because
    these keys have to stay mutually consistent with the uploaded config —
    a stale value from a previous provider is exactly what breaks the tunnel.

    Three rules here are load-bearing, each verified against gluetun v3.41.1
    by reading its startup validation:

    1. VPN_PORT_FORWARDING=on is only legal for the four providers in
       _VPN_PORT_FORWARDING_PROVIDERS. For anything else — `custom` included
       — gluetun exits with "port forwarding cannot be enabled". Because the
       rest of the stack gates on `depends_on: service_healthy`, that is a
       whole-stack outage, not a degraded tunnel.

    2. A non-empty SERVER_COUNTRIES with provider `custom` is likewise fatal:
       "for VPN service provider custom: the country specified is not valid:
       one or more values is set but there is no possible value available".
       Custom has no server list to filter, so the country must be blank.

    3. The endpoint pin is only written for `custom`. For a native provider
       over WireGuard, gluetun filters its server list FIRST and then scans
       the filtered pool for the pinned IP (internal/provider/utils/pick.go)
       — so the pin does not skip the port-forwarding-only filter, it
       collapses the pool to exactly one server. That is why this box stayed
       on a single server across every reconnect and could not move to a
       working one; and if the pinned server ever drops out of the filtered
       set, gluetun fails outright with "target IP address not found: in %d
       filtered connections". Native providers get the pin explicitly
       BLANKED so re-provisioning clears one written by an earlier version.
       (Native WireGuard providers also reject an endpoint PORT outright —
       it must be unset for protonvpn/nordvpn/surfshark/fastestvpn, and is
       restricted to a fixed allow-list for airvpn/ivpn/windscribe — so
       blanking both keys is the only safe native shape.)
    """
    if provider not in _VPN_WIREGUARD_NATIVE_PROVIDERS:
        provider = _VPN_PROVIDER_CUSTOM

    env = {
        "VPN_SERVICE_PROVIDER": provider,
        "VPN_TYPE": "wireguard",
        "VPN_PORT_FORWARDING":
            "on" if provider in _VPN_PORT_FORWARDING_PROVIDERS else "off",
    }

    if provider == _VPN_PROVIDER_CUSTOM:
        env["VPN_COUNTRIES"] = ""            # rule 2
        env["WIREGUARD_ENDPOINT_IP"] = wg.get("WIREGUARD_ENDPOINT_IP", "")
        env["WIREGUARD_ENDPOINT_PORT"] = wg.get(
            "WIREGUARD_ENDPOINT_PORT", str(_WG_DEFAULT_ENDPOINT_PORT))
    else:
        # Country filtering is only offered for ProtonVPN. Other native
        # providers are left unfiltered on purpose: gluetun's windscribe
        # WireGuard servers carry no country field at all, so a country here
        # would trip the same "no possible value available" fatal as rule 2.
        env["VPN_COUNTRIES"] = country if provider == "protonvpn" else ""
        env["WIREGUARD_ENDPOINT_IP"] = ""    # rule 3
        env["WIREGUARD_ENDPOINT_PORT"] = ""

    return env


def _vpn_supports_port_forwarding(provider: str) -> bool:
    """True iff gluetun can lease a forwarded port for this provider."""
    return provider in _VPN_PORT_FORWARDING_PROVIDERS


# Display names for the setup panel's provider dropdown. Keys are gluetun's
# own VPN_SERVICE_PROVIDER strings and must stay exact.
_VPN_PROVIDER_LABELS = {
    _VPN_PROVIDER_CUSTOM: "Other / self-hosted (works with any WireGuard config)",
    "protonvpn": "ProtonVPN",
    "airvpn": "AirVPN",
    "fastestvpn": "FastestVPN",
    "ivpn": "IVPN",
    "mullvad": "Mullvad",
    "nordvpn": "NordVPN",
    "surfshark": "Surfshark",
    "windscribe": "Windscribe",
}


def _vpn_provider_choices() -> list[dict]:
    """Provider options for the setup panel, custom first.

    `custom` leads because it is the right answer for almost everyone: it
    runs any standard WireGuard config, so it is the option that cannot be
    wrong. The native entries below it only change anything for operators who
    want gluetun picking servers for them.
    """
    ordered = [_VPN_PROVIDER_CUSTOM] + sorted(_VPN_WIREGUARD_NATIVE_PROVIDERS)
    return [
        {
            "value": p,
            "label": _VPN_PROVIDER_LABELS.get(p, p),
            "port_forwarding": _vpn_supports_port_forwarding(p),
        }
        for p in ordered
    ]


def _resolve_wireguard_endpoint_ip(host: str) -> str:
    """Resolve a WireGuard endpoint host to a literal IPv4 address.

    Gluetun rejects a hostname outright — "environment variable
    WIREGUARD_ENDPOINT_IP: ParseAddr(...): unexpected character ... note this
    MUST be an IP address" — and several providers ship configs whose
    Endpoint is a DNS name, so resolving here is what makes those configs
    usable at all. Returns the input unchanged when it is already an IP.
    Raises ValueError when a name cannot be resolved.
    """
    host = (host or "").strip()
    if not host:
        raise ValueError("empty WireGuard endpoint host")
    try:
        ipaddress.ip_address(host)
        return host
    except ValueError:
        pass
    try:
        return socket.gethostbyname(host)
    except Exception as exc:
        raise ValueError(
            f"Could not resolve WireGuard endpoint host {host!r} to an IP "
            f"address ({exc}). Gluetun requires a literal IP.") from exc
