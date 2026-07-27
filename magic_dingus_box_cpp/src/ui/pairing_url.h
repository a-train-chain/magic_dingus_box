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

    std::string url = "http://" + host + ":5000/?";
    if (!code.empty()) {
        url += "pair=" + code + "&";
    }
    url += "tab=remote";
    return url;
}

// The same address in a form worth printing under the QR, so a scan that
// fails (bad lighting, cracked screen, a phone camera that won't open
// http:// links) is still recoverable by typing it in.
inline std::string pairing_display_host(const std::string& lan_ip,
                                        const std::string& hostname) {
    if (!lan_ip.empty()) return lan_ip + ":5000";
    if (!hostname.empty()) {
        std::string h = hostname;
        if (h.size() < 6 || h.compare(h.size() - 6, 6, ".local") != 0) {
            h += ".local";
        }
        return h + ":5000";
    }
    return "";
}

} // namespace ui
