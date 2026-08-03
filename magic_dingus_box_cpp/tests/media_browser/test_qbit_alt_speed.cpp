// Unit tests for QbittorrentClient's alternative-speed-limits ("trickle")
// wrapper — the Pi 5 movie-playback contention guard.
//
// qBit 5.x has no explicit-set endpoint for the alt-limits mode (verified
// live on 5.0.3): GET /transfer/speedLimitsMode returns plain-text "0"/"1"
// and POST /transfer/toggleSpeedLimitsMode flips it. The wrapper must
// read-then-toggle-only-on-mismatch (idempotent — it is called
// unconditionally with `false` at every kiosk startup as crash recovery),
// and must treat an empty/malformed read as failure rather than blind-
// toggling a state it cannot see.
//
// Driven entirely through the protected virtual http_get/http_post seam
// ("Virtual seam for tests" in qbittorrent_client.h) — no sockets, no
// login (the wrapper deliberately relies on http_*'s own 403 re-login
// path instead of calling login_locked(), precisely so these tests can
// exist).

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "media_browser/qbittorrent/qbittorrent_client.h"

namespace mb = media_browser;

namespace {

// Simulates qBit's transfer/speedLimitsMode + toggle + setPreferences
// surface. `alt_on` is the simulated server-side mode; overridable knobs
// simulate the failure classes the wrapper must survive.
class FakeQbit : public mb::QbittorrentClient {
public:
    FakeQbit() : mb::QbittorrentClient(Config{}) {}

    bool alt_on = false;              // simulated speedLimitsMode state
    std::string mode_override;        // when set, GET returns this verbatim
    bool fail_mode_get = false;       // GET fails like a transport error
    bool toggle_is_noop = false;      // POST toggle "succeeds" but state sticks
    bool fail_toggle_post = false;    // POST toggle fails like a 4xx

    int mode_gets = 0;
    int toggles = 0;
    std::vector<std::string> post_bodies;  // "<path>|<body>" per http_post

protected:
    std::string http_get(const std::string& path) override {
        if (path == "/api/v2/transfer/speedLimitsMode") {
            ++mode_gets;
            if (fail_mode_get) {
                set_error("curl: Connection refused");  // real-path shape
                return {};
            }
            if (!mode_override.empty()) return mode_override;
            return alt_on ? "1" : "0";
        }
        set_error("unexpected GET " + path);
        return {};
    }

    std::string http_post(const std::string& path,
                          const std::string& body) override {
        post_bodies.push_back(path + "|" + body);
        if (path == "/api/v2/transfer/toggleSpeedLimitsMode") {
            ++toggles;
            if (fail_toggle_post) {
                set_error("qbit HTTP 409");
                return {};
            }
            if (!toggle_is_noop) alt_on = !alt_on;
            return {};  // real qBit: 200 with empty body
        }
        if (path == "/api/v2/app/setPreferences") {
            return {};  // 200, empty body
        }
        set_error("unexpected POST " + path);
        return {};
    }
};

}  // namespace

TEST_CASE("alt limits: enabling from OFF toggles exactly once and verifies",
          "[qbit][alt-speed]") {
    FakeQbit q;
    q.alt_on = false;
    REQUIRE(q.set_alt_speed_limits_enabled(true));
    REQUIRE(q.toggles == 1);
    REQUIRE(q.alt_on);
    REQUIRE(q.mode_gets == 2);  // read + post-toggle verify
    REQUIRE(q.last_error().empty());
}

TEST_CASE("alt limits: enabling when already ON is a no-op (idempotent)",
          "[qbit][alt-speed]") {
    FakeQbit q;
    q.alt_on = true;
    REQUIRE(q.set_alt_speed_limits_enabled(true));
    REQUIRE(q.toggles == 0);  // the whole point of read-then-toggle
    REQUIRE(q.alt_on);
}

TEST_CASE("alt limits: disabling from ON toggles exactly once",
          "[qbit][alt-speed]") {
    FakeQbit q;
    q.alt_on = true;
    REQUIRE(q.set_alt_speed_limits_enabled(false));
    REQUIRE(q.toggles == 1);
    REQUIRE_FALSE(q.alt_on);
}

TEST_CASE("alt limits: startup crash-recovery 'off' on a clean box is a no-op",
          "[qbit][alt-speed]") {
    // The kiosk calls set_alt_speed_limits_enabled(false) unconditionally
    // at every startup so a crash mid-movie can never leave downloads
    // silently capped. On the ordinary (uncapped) boot this must not
    // touch the toggle endpoint at all.
    FakeQbit q;
    q.alt_on = false;
    REQUIRE(q.set_alt_speed_limits_enabled(false));
    REQUIRE(q.toggles == 0);
}

TEST_CASE("alt limits: empty mode response fails without toggling",
          "[qbit][alt-speed]") {
    // qBit down / auth dead: the wrapper cannot know the current state,
    // and a blind toggle could flip the cap the WRONG way. Must fail.
    FakeQbit q;
    q.fail_mode_get = true;
    REQUIRE_FALSE(q.set_alt_speed_limits_enabled(true));
    REQUIRE(q.toggles == 0);
    REQUIRE_FALSE(q.last_error().empty());
}

TEST_CASE("alt limits: malformed mode response fails without toggling",
          "[qbit][alt-speed]") {
    FakeQbit q;
    q.mode_override = "<html>502 Bad Gateway</html>";
    REQUIRE_FALSE(q.set_alt_speed_limits_enabled(false));
    REQUIRE(q.toggles == 0);
}

TEST_CASE("alt limits: trailing newline on the mode response is tolerated",
          "[qbit][alt-speed]") {
    FakeQbit q;
    q.alt_on = false;
    q.mode_override = "0\r\n";
    // Read says OFF; requesting OFF must be a clean no-op, not a
    // malformed-response failure.
    REQUIRE(q.set_alt_speed_limits_enabled(false));
    REQUIRE(q.toggles == 0);
}

TEST_CASE("alt limits: toggle POST failure is reported as failure",
          "[qbit][alt-speed]") {
    FakeQbit q;
    q.alt_on = false;
    q.fail_toggle_post = true;
    REQUIRE_FALSE(q.set_alt_speed_limits_enabled(true));
    REQUIRE_FALSE(q.alt_on);  // server state untouched
}

TEST_CASE("alt limits: success means the FINAL state matches — a toggle "
          "that didn't take is a failure",
          "[qbit][alt-speed]") {
    FakeQbit q;
    q.alt_on = false;
    q.toggle_is_noop = true;  // POST 200s but the mode never changes
    REQUIRE_FALSE(q.set_alt_speed_limits_enabled(true));
    REQUIRE(q.toggles == 1);
    REQUIRE(q.mode_gets == 2);  // it actually verified
    REQUIRE_FALSE(q.last_error().empty());
}

TEST_CASE("configure_alt_speed_limits posts BYTES/s rates as json= preferences",
          "[qbit][alt-speed]") {
    FakeQbit q;
    REQUIRE(q.configure_alt_speed_limits(2097152, 1048576));
    REQUIRE(q.post_bodies.size() == 1);
    // alt_dl_limit / alt_up_limit are KiB/s in qBit's preferences JSON —
    // 2 MiB/s down / 1 MiB/s up IN BYTES — the field is bytes/s on
    // qBit 5.0.3 (live-verified; the KiB docs are wrong).
    REQUIRE(q.post_bodies[0] ==
            "/api/v2/app/setPreferences|"
            "json={\"alt_dl_limit\":2097152,\"alt_up_limit\":1048576}");
}
