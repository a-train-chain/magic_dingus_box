#pragma once

// Media Browser service-client setup — the part of main() that used to
// build the Radarr / Sonarr / TMDB / Prowlarr / qBittorrent clients, start
// the Layer 3 VPN health monitor and run qBit's boot-time alt-limit
// bootstrap. Moved out of main.cpp verbatim (same lookups, same order,
// same log lines); main() still OWNS the result and lends references to
// MediaBrowserHost (media_browser/mb_host.h) and the quiet-mode executors.
//
// Renderer-free and GL-free, so it is compiled into BOTH the kiosk and
// test_media_browser_unit: the key/prefix resolution and the mock-vs-real
// choice are unit-tested on the Mac through an injected environment
// lookup (tests/media_browser/test_mb_services.cpp). Constructing a client
// does no I/O — the first network call happens on first use.

#include <functional>
#include <memory>
#include <string>

namespace app { struct AppState; }

namespace media_browser {

class RadarrClient;
class SonarrClient;
class TmdbClient;
class ProwlarrClient;
class QbittorrentClient;
class VpnHealthMonitor;

// getenv-shaped lookup: nullptr (or "") means unset. Production passes
// std::getenv; tests pass a map.
using EnvLookup = std::function<const char*(const char*)>;

// The three-stage API-key chain shared by Radarr, Sonarr and Prowlarr:
//   1. `kiosk_var`   (MDB_*_API_KEY — explicit kiosk config)
//   2. `service_var` (*_API_KEY — systemd EnvironmentFile of services/.env)
//   3. `service_var` parsed out of `services_env_path` directly (fallback
//      for when systemd env propagation isn't set up), via
//      utils::read_env_value — see utils/services_env.h for quoting rules.
// The file is read only when both variables are unset or empty. "" when
// no stage has a key.
std::string resolve_service_api_key(const EnvLookup& env,
                                    const char* kiosk_var,
                                    const char* service_var,
                                    const std::string& services_env_path);

// One Sonarr TV path prefix, resolved in three tiers: explicit TV var ->
// parent movie var + "tv" -> compiled default. Unset/empty vars are
// skipped. The TV subtree sits one level below the movie library root, so
// reusing the movie var as-is would translate every TV path one directory
// too high; ignoring it would break a box whose STORAGE_ROOT moved. See
// the comment at the call site in make_service_clients().
std::string resolve_tv_prefix(const EnvLookup& env, const char* tv_var,
                              const char* parent_var,
                              const std::string& compiled_default);

// TMDB key: MDB_TMDB_API_KEY, else the first line of
// $HOME/.config/magic_dingus_box/tmdb_api_key with trailing '\n', '\r'
// and ' ' stripped. "" when neither exists.
std::string resolve_tmdb_api_key(const EnvLookup& env);

// Every Media Browser service client, owned. Members are never null except
// `prowlarr` (no key: the Detail AVAILABILITY readout is suppressed).
struct MbServiceClients {
    std::unique_ptr<RadarrClient> radarr;   // RadarrMockClient without a key
    std::unique_ptr<SonarrClient> sonarr;   // SonarrMockClient without a key
    // True exactly when `sonarr` is the REAL client (a key was found).
    // The screens must not present the mock's fixtures as real TV on a box
    // that never had Sonarr set up.
    bool sonarr_configured = false;
    std::unique_ptr<TmdbClient> tmdb;
    std::unique_ptr<ProwlarrClient> prowlarr;  // nullable
    std::unique_ptr<QbittorrentClient> qbit;

    MbServiceClients();
    MbServiceClients(MbServiceClients&&) noexcept;
    MbServiceClients& operator=(MbServiceClients&&) noexcept;
    ~MbServiceClients();
};

// Build the clients in main()'s historical order — Radarr, Sonarr, TMDB,
// Prowlarr, qBittorrent — logging the same "[media_browser] ..." lines to
// stdout. Does no network I/O.
MbServiceClients make_service_clients(
    const EnvLookup& env, const std::string& services_env_path);

// Production overload: std::getenv + utils::kServicesEnvPath.
MbServiceClients make_service_clients();

// Trickle-limit bootstrap (movie playback contention guard): converge the
// alt-limit RATES and clear a crash-stranded alt-limit cap. Best-effort
// and gated on `services_env_path` existing (unprovisioned Pis and dev
// machines do nothing). Does network I/O against qBit when provisioned.
void bootstrap_qbit_alt_limits(QbittorrentClient& qbit,
                               const std::string& services_env_path);

// Layer 3 monitor, started only when Layers 1+2 already pass (unlocked AND
// VPN configured); otherwise nullptr. When started it also kicks the
// backgrounded `playback_services_pause.sh unpause` startup safety net.
// The returned monitor's destructor stops its worker thread.
std::unique_ptr<VpnHealthMonitor> start_vpn_health_monitor(app::AppState& state);

}  // namespace media_browser
