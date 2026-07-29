"""Tests for the WireGuard .conf parser + VPN provider selection behind
/admin/media-browser/setup.

Regression 1 — dual-stack addresses. ProtonVPN configs list dual-stack
interface addresses (`Address = 10.2.0.2/32, 2a07:b944::2:2/128`). Gluetun's
container runs without IPv6 and hard-fails on the IPv6 entry ("interface
address is IPv6 but IPv6 is not supported"), crash-looping the whole services
stack — hit live on the first Pi 5 bench provisioning, 2026-07-22. The parser
must keep only IPv4 addresses.

Regression 2 — bring-your-own-VPN. The setup flow used to hardcode
`VPN_SERVICE_PROVIDER=protonvpn` + `VPN_PORT_FORWARDING=on` and pin
`WIREGUARD_ENDPOINT_IP` for every customer. Each of those is a FATAL gluetun
startup error for the wrong provider, and gluetun failing to start takes the
whole stack with it (radarr/prowlarr/qbittorrent/byparr are gated on
`depends_on: service_healthy`).

The fatal conditions asserted below were verified empirically against the
exact image the box runs (`qmcgaw/gluetun:v3` == v3.41.1) by launching
throwaway containers and reading gluetun's own settings-validation output:

  * VPN_PORT_FORWARDING=on for an unsupported provider:
      "port forwarding cannot be enabled: value is not one of the possible
       choices: mullvad must be one of perfect privacy, private internet
       access, privatevpn or protonvpn"
  * non-empty SERVER_COUNTRIES with provider `custom`:
      "for VPN service provider custom: the country specified is not valid:
       one or more values is set but there is no possible value available"
  * `custom` without an endpoint port:
      "server selection: Wireguard server selection settings: endpoint port
       is not set"
  * a hostname in WIREGUARD_ENDPOINT_IP:
      "ParseAddr(...): unexpected character ... note this MUST be an IP
       address"

All keys in this file are SYNTHETIC — fixed placeholder strings, never issued
by any provider. Do not paste a real config in here: a live ProtonVPN key was
committed to this file once (commit 5fad794) and had to be revoked.
"""
import pytest

from magic_dingus_box.web.admin import (
    _detect_vpn_brand,
    _parse_wireguard_config,
    _parse_wireguard_endpoint,
    _vpn_provider_env,
    _vpn_supports_port_forwarding,
)

# Synthetic key material — base64 of readable placeholder text, so it is
# obvious at a glance that these are not real keys.
SYNTH_PRIV = "cGxhY2Vob2xkZXJfcHJpdmF0ZV9rZXlfMzJieXRlc18="
SYNTH_PUB = "cGxhY2Vob2xkZXJfcHVibGljX2tleV8zMmJ5dGVzX18="

PROTON_DUAL_STACK = f"""\
[Interface]
# Key for magicpi5
PrivateKey = {SYNTH_PRIV}
Address = 10.2.0.2/32, 2a07:b944::2:2/128
DNS = 10.2.0.1

[Peer]
PublicKey = {SYNTH_PUB}
AllowedIPs = 0.0.0.0/0, ::/0
Endpoint = 169.150.196.67:51820
"""

MULLVAD = f"""\
[Interface]
PrivateKey = {SYNTH_PRIV}
Address = 10.64.222.11/32
DNS = 10.64.0.1

[Peer]
PublicKey = {SYNTH_PUB}
AllowedIPs = 0.0.0.0/0,::/0
Endpoint = 185.65.135.170:51820
"""

IVPN = f"""\
[Interface]
PrivateKey = {SYNTH_PRIV}
Address = 172.27.181.44/32
DNS = 172.16.0.1

[Peer]
PublicKey = {SYNTH_PUB}
AllowedIPs = 0.0.0.0/0
Endpoint = nl.wg.ivpn.net:58237
"""

AIRVPN = f"""\
[Interface]
Address = 10.128.91.7/32
PrivateKey = {SYNTH_PRIV}
DNS = 10.128.0.1

[Peer]
PublicKey = {SYNTH_PUB}
AllowedIPs = 0.0.0.0/0
Endpoint = nl3.vpn.airdns.org:1637
"""

WINDSCRIBE = f"""\
[Interface]
PrivateKey = {SYNTH_PRIV}
Address = 100.75.19.203/32
DNS = 10.255.255.1

[Peer]
PublicKey = {SYNTH_PUB}
AllowedIPs = 0.0.0.0/0
Endpoint = nl-002.whiskergalaxy.com:443
"""

# A self-hosted / corporate tunnel: no brand markers at all.
SELF_HOSTED = f"""\
[Interface]
PrivateKey = {SYNTH_PRIV}
Address = 192.168.77.4/24
DNS = 192.168.77.1

[Peer]
PublicKey = {SYNTH_PUB}
AllowedIPs = 0.0.0.0/0
Endpoint = 203.0.113.9:51820
"""


# --------------------------------------------------------------------------
# Regression 1 — IPv4-only addresses
# --------------------------------------------------------------------------

def test_dual_stack_address_keeps_ipv4_only():
    wg = _parse_wireguard_config(PROTON_DUAL_STACK)
    assert wg["WIREGUARD_ADDRESSES"] == "10.2.0.2/32"


def test_ipv4_only_address_passes_through():
    conf = PROTON_DUAL_STACK.replace(
        "Address = 10.2.0.2/32, 2a07:b944::2:2/128", "Address = 10.2.0.2/32")
    wg = _parse_wireguard_config(conf)
    assert wg["WIREGUARD_ADDRESSES"] == "10.2.0.2/32"


def test_ipv6_only_address_is_a_clear_error():
    conf = PROTON_DUAL_STACK.replace(
        "Address = 10.2.0.2/32, 2a07:b944::2:2/128",
        "Address = 2a07:b944::2:2/128")
    with pytest.raises(ValueError, match="IPv4"):
        _parse_wireguard_config(conf)


def test_other_fields_unaffected():
    wg = _parse_wireguard_config(PROTON_DUAL_STACK)
    assert wg["WIREGUARD_PRIVATE_KEY"] == SYNTH_PRIV
    assert wg["WIREGUARD_PUBLIC_KEY"] == SYNTH_PUB
    assert wg["WIREGUARD_ENDPOINT_IP"] == "169.150.196.67"


def test_parser_emits_only_gluetun_env_keys():
    """Everything the parser returns is written verbatim into services/.env."""
    wg = _parse_wireguard_config(PROTON_DUAL_STACK)
    assert all(k.startswith("WIREGUARD_") for k in wg), wg.keys()


# --------------------------------------------------------------------------
# Endpoint port — gluetun's `custom` provider refuses to start without it
# --------------------------------------------------------------------------

def test_endpoint_port_is_captured():
    wg = _parse_wireguard_config(PROTON_DUAL_STACK)
    assert wg["WIREGUARD_ENDPOINT_PORT"] == "51820"


@pytest.mark.parametrize("conf,expected_port", [
    (IVPN, "58237"),
    (AIRVPN, "1637"),
    (WINDSCRIBE, "443"),
])
def test_non_default_endpoint_ports_survive(conf, expected_port):
    assert _parse_wireguard_config(conf)["WIREGUARD_ENDPOINT_PORT"] == expected_port


def test_endpoint_without_port_falls_back_to_wireguard_default():
    host, port = _parse_wireguard_endpoint("198.51.100.7")
    assert (host, port) == ("198.51.100.7", 51820)


def test_bracketed_ipv6_endpoint_splits_host_and_port():
    host, port = _parse_wireguard_endpoint("[2001:db8::1]:51820")
    assert (host, port) == ("2001:db8::1", 51820)


def test_unbracketed_ipv6_endpoint_does_not_invent_a_port():
    """Splitting on the last colon would turn the final hextet into a port."""
    host, port = _parse_wireguard_endpoint("2001:db8::1")
    assert (host, port) == ("2001:db8::1", 51820)


def test_hostname_endpoint_keeps_the_hostname():
    """Resolution happens later; the parser must not mangle the name."""
    wg = _parse_wireguard_config(IVPN)
    assert wg["WIREGUARD_ENDPOINT_IP"] == "nl.wg.ivpn.net"


# --------------------------------------------------------------------------
# Provider detection
# --------------------------------------------------------------------------

@pytest.mark.parametrize("conf,expected", [
    (PROTON_DUAL_STACK, "protonvpn"),
    (MULLVAD, "mullvad"),
    (IVPN, "ivpn"),
    (AIRVPN, "airvpn"),
    (WINDSCRIBE, "windscribe"),
])
def test_detects_known_providers(conf, expected):
    assert _detect_vpn_brand(conf) == expected


def test_unknown_config_detects_as_nothing():
    """A self-hosted tunnel must not be mistaken for a commercial provider."""
    assert _detect_vpn_brand(SELF_HOSTED) == ""


def test_proton_detected_without_the_key_comment():
    """Address + DNS alone are enough; the comment is only a third signal."""
    conf = PROTON_DUAL_STACK.replace("# Key for magicpi5\n", "")
    assert _detect_vpn_brand(conf) == "protonvpn"


def test_proton_not_detected_from_a_single_weak_signal():
    conf = SELF_HOSTED.replace("Address = 192.168.77.4/24",
                               "Address = 10.2.0.9/32")
    assert _detect_vpn_brand(conf) == ""


# --------------------------------------------------------------------------
# Port forwarding — the list gluetun v3.41.1 actually accepts
# --------------------------------------------------------------------------

@pytest.mark.parametrize("provider,supported", [
    ("protonvpn", True),
    ("private internet access", True),
    ("privatevpn", True),
    ("perfect privacy", True),
    ("mullvad", False),
    ("ivpn", False),
    ("airvpn", False),
    ("windscribe", False),
    ("nordvpn", False),
    ("surfshark", False),
    ("custom", False),
])
def test_port_forwarding_support_matches_gluetun(provider, supported):
    assert _vpn_supports_port_forwarding(provider) is supported


# --------------------------------------------------------------------------
# Env generation — the fatal-condition guards
# --------------------------------------------------------------------------

def test_protonvpn_gets_native_mode_with_port_forwarding():
    wg = _parse_wireguard_config(PROTON_DUAL_STACK)
    env = _vpn_provider_env("protonvpn", wg, country="Netherlands")
    assert env["VPN_SERVICE_PROVIDER"] == "protonvpn"
    assert env["VPN_PORT_FORWARDING"] == "on"
    assert env["VPN_COUNTRIES"] == "Netherlands"


def test_protonvpn_does_not_pin_the_endpoint():
    """A pin overrides gluetun's port-forwarding-only server filter.

    Observed live: the box stayed stuck on one server across reconnects and
    could not move to a working one. The keys must be present-but-blank so a
    re-provision CLEARS a pin written by an earlier version.
    """
    wg = _parse_wireguard_config(PROTON_DUAL_STACK)
    env = _vpn_provider_env("protonvpn", wg, country="Netherlands")
    assert env["WIREGUARD_ENDPOINT_IP"] == ""
    assert env["WIREGUARD_ENDPOINT_PORT"] == ""


@pytest.mark.parametrize("conf", [MULLVAD, IVPN, AIRVPN, WINDSCRIBE, SELF_HOSTED])
def test_non_proton_configs_run_as_custom_with_a_pinned_endpoint(conf):
    """`custom` has no server list, so the endpoint is all gluetun has."""
    wg = _parse_wireguard_config(conf)
    env = _vpn_provider_env("custom", wg)
    assert env["VPN_SERVICE_PROVIDER"] == "custom"
    assert env["WIREGUARD_ENDPOINT_IP"] == wg["WIREGUARD_ENDPOINT_IP"]
    assert env["WIREGUARD_ENDPOINT_PORT"] == wg["WIREGUARD_ENDPOINT_PORT"]


def test_custom_never_requests_port_forwarding():
    """gluetun: 'custom must be one of perfect privacy, ... or protonvpn'."""
    wg = _parse_wireguard_config(SELF_HOSTED)
    assert _vpn_provider_env("custom", wg)["VPN_PORT_FORWARDING"] == "off"


def test_custom_blanks_the_country():
    """A non-empty SERVER_COUNTRIES with provider `custom` is fatal."""
    wg = _parse_wireguard_config(SELF_HOSTED)
    env = _vpn_provider_env("custom", wg, country="Netherlands")
    assert env["VPN_COUNTRIES"] == ""


def test_custom_always_carries_an_endpoint_port():
    """'endpoint port is not set' is a fatal gluetun startup error."""
    wg = _parse_wireguard_config(SELF_HOSTED)
    env = _vpn_provider_env("custom", wg)
    assert env["WIREGUARD_ENDPOINT_PORT"].isdigit()


def test_unknown_provider_string_is_coerced_to_custom():
    """Never hand gluetun a provider name it does not know."""
    wg = _parse_wireguard_config(SELF_HOSTED)
    env = _vpn_provider_env("some-vpn-that-does-not-exist", wg)
    assert env["VPN_SERVICE_PROVIDER"] == "custom"
    assert env["VPN_PORT_FORWARDING"] == "off"


@pytest.mark.parametrize("provider", [
    "mullvad", "ivpn", "airvpn", "windscribe", "nordvpn", "surfshark",
])
def test_native_non_proton_providers_are_safe_by_construction(provider):
    """Operator override path: no port forwarding, no country filter.

    gluetun's windscribe WireGuard servers carry no country field at all, so
    any country here would trip the same fatal validation as `custom`.
    """
    wg = _parse_wireguard_config(MULLVAD)
    env = _vpn_provider_env(provider, wg, country="Netherlands")
    assert env["VPN_SERVICE_PROVIDER"] == provider
    assert env["VPN_PORT_FORWARDING"] == "off"
    assert env["VPN_COUNTRIES"] == ""
    assert env["WIREGUARD_ENDPOINT_IP"] == ""


def test_every_provider_env_sets_wireguard_type():
    wg = _parse_wireguard_config(MULLVAD)
    for provider in ("protonvpn", "mullvad", "custom", "nonsense"):
        assert _vpn_provider_env(provider, wg)["VPN_TYPE"] == "wireguard"
