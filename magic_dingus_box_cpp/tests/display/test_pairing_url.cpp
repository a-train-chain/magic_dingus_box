// Unit tests for the phone-remote pairing URL.
//
// The QR previously hardcoded "http://magicpi.local:5000/...". That host
// does not exist on any box whose hostname isn't literally "magicpi" —
// and first_boot.sh names every cloned Pi "magicpi-XXXX", so the QR
// pointed at a nonexistent host on EVERY shipped unit. The phone silently
// failed to resolve it and no pairing request ever reached the server.

#include <catch2/catch_test_macros.hpp>

#include "ui/pairing_url.h"

using ui::build_pairing_url;

TEST_CASE("prefers the LAN IP — no mDNS resolution required") {
    // A raw IP works even where .local is broken: Android's mDNS support
    // is inconsistent and plenty of consumer routers block multicast.
    // The code is valid for ~2 minutes, far shorter than any DHCP lease,
    // so an IP is stable enough for the scan.
    REQUIRE(build_pairing_url("10.0.0.227", "magicpi5", "587244")
            == "http://10.0.0.227:5000/?pair=587244&tab=remote");
}

TEST_CASE("falls back to the REAL hostname when no IP is known") {
    // Must use the actual hostname, never a hardcoded one.
    REQUIRE(build_pairing_url("", "magicpi5", "111222")
            == "http://magicpi5.local:5000/?pair=111222&tab=remote");
}

TEST_CASE("cloned Pis get their generated hostname, not 'magicpi'") {
    // first_boot.sh assigns magicpi-XXXX. This is the case the old
    // hardcoded URL broke on every fielded box.
    REQUIRE(build_pairing_url("", "magicpi-a3f9", "333444")
            == "http://magicpi-a3f9.local:5000/?pair=333444&tab=remote");
}

TEST_CASE("a hostname already carrying .local is not double-suffixed") {
    REQUIRE(build_pairing_url("", "magicpi5.local", "555666")
            == "http://magicpi5.local:5000/?pair=555666&tab=remote");
}

TEST_CASE("with neither IP nor hostname the URL is empty, not malformed") {
    // Better to render no QR than one pointing at "http://.local:5000".
    REQUIRE(build_pairing_url("", "", "777888").empty());
}

TEST_CASE("an empty code still yields a reachable base URL") {
    // The operator can still open the page and type the code by hand.
    REQUIRE(build_pairing_url("10.0.0.227", "magicpi5", "")
            == "http://10.0.0.227:5000/?tab=remote");
}

TEST_CASE("human-readable address for on-screen manual entry") {
    // Shown under the QR so a failed scan is recoverable.
    REQUIRE(ui::pairing_display_host("10.0.0.227", "magicpi5") == "10.0.0.227:5000");
    REQUIRE(ui::pairing_display_host("", "magicpi5") == "magicpi5.local:5000");
    REQUIRE(ui::pairing_display_host("", "").empty());
}
