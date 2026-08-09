#pragma once

#include <string>

namespace ui {

// Build the URL encoded into the Phone Remote pairing QR code.
//
// HISTORY: this was hardcoded as "http://magicpi.local:5000/...". That
// host exists only on a box literally named "magicpi" — and first_boot.sh
// assigns every cloned Pi a hostname of the form "magicpi-XXXX", so the QR
// pointed at a nonexistent host on every shipped unit. The phone silently
// failed to resolve it, no request ever reached the server, and the
// pairing audit log stayed empty with nothing to diagnose from.
//
// Prefer the LAN IP over the .local name. mDNS is the fragile link here:
// Android's support has been inconsistent across versions and plenty of
// consumer routers block multicast, whereas a literal IP just works for
// any phone on the same subnet. A pairing code lives ~2 minutes, far
// shorter than any DHCP lease, so the address cannot go stale inside the
// window that matters. The hostname remains the fallback for the case
// where no IPv4 address is known yet.
//
// Returns an empty string when neither is available — the caller must
// then render no QR at all rather than one pointing at "http://.local".
//
// TARGET: the /connect landing page ("Connect a Device"), NOT the pairing
// flow directly. The web side offers the choice — "use this phone as a
// remote" (which forwards the code into the existing /?pair= flow) or
// "manage movies & playlists" — so ONE QR serves both customer intents.
// The code rides along as ?code=NNNNNN; /connect validates it and builds
// the pair link itself. Kept as a single ":5000/connect?code=" literal on
// purpose: the release pipeline greps `strings` of the binary for
// "/connect?code=" to prove the retarget actually shipped.
inline std::string build_pairing_url(const std::string& lan_ip,
                                     const std::string& hostname,
                                     const std::string& code) {
    std::string host;
    if (!lan_ip.empty()) {
        host = lan_ip;
    } else if (!hostname.empty()) {
        host = hostname;
        // Don't produce "magicpi5.local.local" if the caller already
        // handed us a fully-qualified mDNS name.
        if (host.size() < 6 || host.compare(host.size() - 6, 6, ".local") != 0) {
            host += ".local";
        }
    } else {
        return "";
    }

    if (!code.empty()) {
        return "http://" + host + ":5000/connect?code=" + code;
    }
    // No code (shouldn't happen while the pairing screen is open, but the
    // page is still useful without one — /connect then routes the remote
    // button to the in-app 6-digit form).
    return "http://" + host + ":5000/connect";
}

// The typed-fallback address printed under the QR on the "Connect a
// Device" screen, so a failed scan (bad lighting, cracked screen, a phone
// camera that won't open http:// links) is still recoverable by typing.
//
// Deliberately the OPPOSITE preference from build_pairing_url: the QR
// prefers the raw IP (phone cameras just follow it; mDNS is the fragile
// link), but a human TYPING an address is better served by the stable,
// bookmarkable <hostname>.local form — it's what the Owner's Guide and
// the Content Manager teach, and it survives DHCP lease changes. The IP
// rides along in parentheses as the rescue for networks where mDNS is
// blocked, which is exactly the failure mode that forced the QR to
// prefer the IP in the first place.
inline std::string connect_typed_address(const std::string& lan_ip,
                                         const std::string& hostname) {
    std::string mdns;
    if (!hostname.empty()) {
        mdns = hostname;
        if (mdns.size() < 6 || mdns.compare(mdns.size() - 6, 6, ".local") != 0) {
            mdns += ".local";
        }
    }
    if (!mdns.empty() && !lan_ip.empty()) {
        return "http://" + mdns + ":5000  (or " + lan_ip + ":5000)";
    }
    if (!mdns.empty())   return "http://" + mdns + ":5000";
    if (!lan_ip.empty()) return "http://" + lan_ip + ":5000";
    return "";
}

} // namespace ui
