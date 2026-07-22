"""Tests for the WireGuard .conf parser behind /admin/media-browser/setup.

Regression: ProtonVPN configs list dual-stack interface addresses
(`Address = 10.2.0.2/32, 2a07:b944::2:2/128`). Gluetun's container runs
without IPv6 and hard-fails on the IPv6 entry ("interface address is
IPv6 but IPv6 is not supported"), crash-looping the whole services
stack — hit live on the first Pi 5 bench provisioning, 2026-07-22.
The parser must keep only IPv4 addresses.
"""
import pytest

from magic_dingus_box.web.admin import _parse_wireguard_config

PROTON_DUAL_STACK = """\
[Interface]
# Key for magicpi5
PrivateKey = 4BvbpNbcgIkewvT7dTHnR8n0PDbLV1Vd5Ph9DWjbanc=
Address = 10.2.0.2/32, 2a07:b944::2:2/128
DNS = 10.2.0.1

[Peer]
PublicKey = zZk9M0dTVdCgy1MEJyIWDqLxwCzXvHLQ2eyKUnKPfEA=
AllowedIPs = 0.0.0.0/0, ::/0
Endpoint = 169.150.196.67:51820
"""


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
    assert wg["WIREGUARD_PRIVATE_KEY"].endswith("banc=")
    assert wg["WIREGUARD_PUBLIC_KEY"].endswith("KPfEA=")
    assert wg["WIREGUARD_ENDPOINT_IP"] == "169.150.196.67"
