#include "media_browser/mb_services.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "app/app_state.h"
#include "media_browser/health/vpn_health_monitor.h"
#include "media_browser/prowlarr/prowlarr_client.h"
#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/radarr/radarr_mock.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/sonarr/sonarr_mock.h"
#include "media_browser/tmdb_client.h"
#include "utils/services_env.h"

namespace media_browser {

MbServiceClients::MbServiceClients() = default;
MbServiceClients::MbServiceClients(MbServiceClients&&) noexcept = default;
MbServiceClients& MbServiceClients::operator=(MbServiceClients&&) noexcept = default;
MbServiceClients::~MbServiceClients() = default;

std::string resolve_service_api_key(const EnvLookup& env,
                                    const char* kiosk_var,
                                    const char* service_var,
                                    const std::string& services_env_path) {
    if (const char* k = env(kiosk_var); k && *k) return k;
    if (const char* k2 = env(service_var); k2 && *k2) return k2;
    return utils::read_env_value(services_env_path, service_var);
}

std::string resolve_tv_prefix(const EnvLookup& env, const char* tv_var,
                              const char* parent_var,
                              const std::string& compiled_default) {
    if (const char* p = env(tv_var); p && *p) {
        return SonarrClient::normalize_prefix(p);
    }
    if (const char* p = env(parent_var); p && *p) {
        return SonarrClient::normalize_prefix(
            SonarrClient::normalize_prefix(p) + "tv");
    }
    return compiled_default;
}

std::string resolve_tmdb_api_key(const EnvLookup& env) {
    std::string tmdb_key;
    if (const char* k = env("MDB_TMDB_API_KEY"); k && *k) {
        tmdb_key = k;
    } else if (const char* home = env("HOME"); home) {
        std::ifstream kf(std::string(home) + "/.config/magic_dingus_box/tmdb_api_key");
        if (kf) std::getline(kf, tmdb_key);
        while (!tmdb_key.empty() &&
               (tmdb_key.back() == '\n' || tmdb_key.back() == '\r' ||
                tmdb_key.back() == ' ')) {
            tmdb_key.pop_back();
        }
    }
    return tmdb_key;
}

MbServiceClients make_service_clients(const EnvLookup& env,
                                      const std::string& services_env_path) {
    MbServiceClients out;

    // Radarr client is the real HTTP client when we can find an API key:
    //   1. MDB_RADARR_API_KEY env var (preferred — explicit kiosk config)
    //   2. RADARR_API_KEY env var (systemd EnvironmentFile of services/.env)
    //   3. Parse /opt/magic_dingus_box/services/.env directly (fallback
    //      for when systemd env propagation isn't set up)
    // Otherwise we fall back to RadarrMockClient for dev machines.
    // (Step 3 for every client below goes through utils::read_env_value —
    // see utils/services_env.h for the quoting rules.)
    const std::string radarr_key = resolve_service_api_key(
        env, "MDB_RADARR_API_KEY", "RADARR_API_KEY", services_env_path);

    if (!radarr_key.empty()) {
        RadarrClient::Config radarr_cfg;
        if (const char* base = env("MDB_RADARR_BASE_URL"); base && *base) {
            radarr_cfg.base_url = base;
        }
        radarr_cfg.api_key = radarr_key;
        // Path-translation overrides: redirect /library/* between the Radarr
        // container's view and the host's actual mount point. Default in
        // radarr_cfg works for the standard /mnt/ssd setup; the env vars
        // exist so STORAGE_ROOT can change without a kiosk recompile. The
        // download/incomplete tree is not exposed via Radarr's API to the
        // kiosk, so it doesn't need translation here. normalize_prefix
        // ensures both prefixes end with '/' to avoid /library2/foo
        // falsely matching /library.
        if (const char* p = env("MDB_CONTAINER_LIBRARY_PREFIX"); p && *p) {
            radarr_cfg.container_library_prefix =
                RadarrClient::normalize_prefix(p);
            std::cout << "[media_browser] container_library_prefix override: "
                      << radarr_cfg.container_library_prefix << std::endl;
        }
        if (const char* p = env("MDB_HOST_LIBRARY_PREFIX"); p && *p) {
            radarr_cfg.host_library_prefix =
                RadarrClient::normalize_prefix(p);
            std::cout << "[media_browser] host_library_prefix override: "
                      << radarr_cfg.host_library_prefix << std::endl;
        }
        std::string base_url_for_log = radarr_cfg.base_url;
        out.radarr = std::make_unique<RadarrClient>(std::move(radarr_cfg));
        std::cout << "[media_browser] Using real RadarrClient (base_url="
                  << base_url_for_log << ")" << std::endl;
    } else {
        out.radarr = std::make_unique<RadarrMockClient>();
        std::cout << "[media_browser] No Radarr API key found — using RadarrMockClient" << std::endl;
    }

    // Sonarr client (Phase 2b). Same three-stage key chain as Radarr above:
    //   1. MDB_SONARR_API_KEY env var (explicit kiosk config)
    //   2. SONARR_API_KEY env var (systemd EnvironmentFile of services/.env)
    //   3. Parse /opt/magic_dingus_box/services/.env directly
    // setup_services.sh writes SONARR_API_KEY into that .env after Sonarr's
    // first container start; a box provisioned before the Sonarr stack landed
    // simply has no line and falls through to the mock.
    const std::string sonarr_key = resolve_service_api_key(
        env, "MDB_SONARR_API_KEY", "SONARR_API_KEY", services_env_path);

    if (!sonarr_key.empty()) {
        SonarrClient::Config sonarr_cfg;
        if (const char* base = env("MDB_SONARR_BASE_URL"); base && *base) {
            sonarr_cfg.base_url = base;
        }
        sonarr_cfg.api_key = sonarr_key;
        // TV path prefixes, resolved in three tiers.
        //
        // The TV subtree is /data/library/tv ↔ /mnt/ssd/library/tv — one level
        // below the movie library root — so it cannot simply reuse the Radarr
        // vars (every TV path would translate one directory too high). But it
        // must not ignore them either: MDB_HOST_LIBRARY_PREFIX exists so
        // STORAGE_ROOT can move without a recompile, and a box where the
        // operator points movies at /mnt/nvme/library/ while Sonarr keeps a
        // compiled-in /mnt/ssd/library/tv/ would hand GStreamer an
        // unresolvable container path — with nothing but a spdlog::warn to say
        // so, and none of the legacy-alternate fallbacks the Radarr resolver
        // has. Nothing in provisioning writes MDB_*_TV_PREFIX, so deriving
        // from the parent is what actually fires in the field.
        //
        // Order: explicit TV var → parent movie var + "tv" → compiled default.
        sonarr_cfg.container_library_prefix =
            resolve_tv_prefix(env, "MDB_CONTAINER_TV_PREFIX",
                              "MDB_CONTAINER_LIBRARY_PREFIX",
                              sonarr_cfg.container_library_prefix);
        sonarr_cfg.host_library_prefix =
            resolve_tv_prefix(env, "MDB_HOST_TV_PREFIX", "MDB_HOST_LIBRARY_PREFIX",
                              sonarr_cfg.host_library_prefix);
        std::cout << "[media_browser] sonarr tv prefixes: "
                  << sonarr_cfg.container_library_prefix << " -> "
                  << sonarr_cfg.host_library_prefix << std::endl;
        std::string sonarr_url_for_log = sonarr_cfg.base_url;
        out.sonarr = std::make_unique<SonarrClient>(std::move(sonarr_cfg));
        std::cout << "[media_browser] Using real SonarrClient (base_url="
                  << sonarr_url_for_log << ")" << std::endl;
    } else {
        out.sonarr = std::make_unique<SonarrMockClient>();
        std::cout << "[media_browser] No Sonarr API key found — using SonarrMockClient"
                  << std::endl;
    }
    // sonarr_configured = !sonarr_key.empty() is exactly the
    // fallback-to-SonarrMockClient condition above.
    out.sonarr_configured = !sonarr_key.empty();
    // Consumed by BrowseScreen (Phase 2c-1): the TV library feeds the
    // in-library hide and the For You seed sample in TV mode.

    // TMDB client — Phase A: Discover endpoints for Browse categories.
    // Radarr still handles library/add/queue; TMDB only drives discovery.
    const std::string tmdb_key = resolve_tmdb_api_key(env);
    if (tmdb_key.empty()) {
        std::cout << "[media_browser] WARN: No TMDB API key (MDB_TMDB_API_KEY or "
                     "~/.config/magic_dingus_box/tmdb_api_key). Browse categories "
                     "will be empty until a key is configured." << std::endl;
    } else {
        std::cout << "[media_browser] TMDB API key loaded (len=" << tmdb_key.size() << ")"
                  << std::endl;
    }
    out.tmdb = std::make_unique<TmdbClient>(tmdb_key);

    // Optional Prowlarr client for the AVAILABILITY readout on Detail.
    // Same key lookup chain as Radarr above:
    //   1. MDB_PROWLARR_API_KEY env var
    //   2. PROWLARR_API_KEY env var (systemd EnvironmentFile)
    //   3. Parse /opt/magic_dingus_box/services/.env directly
    // Falls back to nullptr (readout suppressed) if no key is found.
    {
        const std::string prowlarr_key = resolve_service_api_key(
            env, "MDB_PROWLARR_API_KEY", "PROWLARR_API_KEY", services_env_path);

        if (!prowlarr_key.empty()) {
            ProwlarrClient::Config pcfg;
            pcfg.api_key = prowlarr_key;
            if (const char* base = env("MDB_PROWLARR_BASE_URL");
                base && *base) {
                pcfg.base_url = base;
            }
            out.prowlarr = std::make_unique<ProwlarrClient>(std::move(pcfg));
            std::cout << "[media_browser] Prowlarr client enabled "
                      << "(base_url=" << "http://localhost:9696" << ", "
                      << "key_len=" << prowlarr_key.size() << ")"
                      << std::endl;
        } else {
            std::cout << "[media_browser] Prowlarr client disabled "
                      << "(no PROWLARR_API_KEY found; AVAILABILITY readout "
                      << "on Detail will be suppressed)" << std::endl;
        }
    }

    // qBittorrent client — used by QueueScreen to overlay live
    // download progress over Radarr's stale-cached queue snapshot.
    // qBit always runs on localhost:8080 in our docker-compose setup;
    // credentials come from MDB_QBIT_USER / MDB_QBIT_PASS env vars,
    // or fall back to the docker-compose default (admin/adminadmin).
    out.qbit = std::make_unique<QbittorrentClient>(
        [&env]() {
            QbittorrentClient::Config cfg;
            if (const char* u = env("MDB_QBIT_USER"); u && *u) {
                cfg.username = u;
            }
            if (const char* p = env("MDB_QBIT_PASS"); p && *p) {
                cfg.password = p;
            }
            if (const char* url = env("MDB_QBIT_BASE_URL");
                url && *url) {
                cfg.base_url = url;
            }
            return cfg;
        }());
    std::cout << "[media_browser] qBittorrent client enabled "
              << "(base_url=http://localhost:8080)" << std::endl;

    return out;
}

MbServiceClients make_service_clients() {
    return make_service_clients(
        [](const char* name) -> const char* { return std::getenv(name); },
        utils::kServicesEnvPath);
}

void bootstrap_qbit_alt_limits(QbittorrentClient& qbit,
                               const std::string& services_env_path) {
    // Trickle-limit bootstrap (movie playback contention guard, Pi 5).
    // Two one-shot, best-effort calls — qBit may well be down this early
    // in boot (the Docker stack races kiosk startup), so a failure is
    // informational only and must never block the kiosk coming up.
    //
    // (a) Converge the alternative-limit rates: 2 MiB/s down (leaves the
    //     swarm progressing through a 2h movie without contending with
    //     GStreamer's reads), 8 KiB/s up — effectively OFF. Seeding is
    //     the expensive direction during playback: serving strangers'
    //     piece requests is random reads over the whole library, and
    //     with no free RAM for page cache it measured 8x amplified
    //     (122 GB read to upload 18 GB, 2026-08-11) on the same SSD the
    //     movie streams from. Downloads the user is waiting on are
    //     cheap sequential writes; those stay at 2 MiB/s.
    //     Written every boot so shipped boxes converge on retuned rates
    //     via OTA without anyone touching the qBit WebUI.
    // (b) CRASH RECOVERY: unconditionally clear the alt-limits cap. Only
    //     PlaybackScreen sets it (Pi 5 movie playback), and its leave()
    //     clears it — but a kiosk crash/power-cut mid-movie would leave
    //     every future download silently capped at trickle speed with
    //     nothing in any UI to explain why. The wrapper is idempotent
    //     (read-then-toggle), so the ordinary clean boot is a no-op read.
    // Gated on the provisioning marker like GameQuietMode, so
    // unprovisioned Pis and dev machines do exactly nothing (no qBit
    // failure lines on every boot). Provisioned boxes still clear
    // unconditionally — board-agnostic, since a leftover cap is
    // qBit-side state that can travel with a cloned image or SSD.
    if (std::filesystem::exists(services_env_path)) {
        // 2 MiB/s down / 8 KiB/s up, IN BYTES (see the client header:
        // qBit 5's preference field is bytes/s despite docs claiming
        // KiB — the KiB assumption strangled downloads to 1.5 KB/s).
        // Upload deliberately near-zero, not zero: 0 means UNLIMITED to
        // qBit, and a token allowance keeps peer connections from
        // erroring out mid-handshake.
        if (!qbit.configure_alt_speed_limits(
                /*dl_bytes_s=*/2 * 1024 * 1024,
                /*up_bytes_s=*/8 * 1024)) {
            std::cout << "[media_browser] qbit alt-limit rate config failed "
                         "(best-effort; qBit may not be up yet)" << std::endl;
        }
        if (!qbit.set_alt_speed_limits_enabled(false)) {
            std::cout << "[media_browser] qbit alt-limit crash-recovery "
                         "clear failed (best-effort; qBit may not be up yet)"
                      << std::endl;
        }
    }
}

std::unique_ptr<VpnHealthMonitor> start_vpn_health_monitor(app::AppState& state) {
    // Layer 3 monitor — only meaningful when Layers 1+2 already pass.
    // Otherwise the Settings menu won't expose MB anyway, so save the
    // background polling work.
    std::unique_ptr<VpnHealthMonitor> vpn_health_monitor;
    if (state.media_browser_unlocked && state.media_browser_vpn_configured) {
        vpn_health_monitor = std::make_unique<VpnHealthMonitor>(state);
        vpn_health_monitor->start();

        // Startup safety: call playback_services_pause.sh unpause to bring
        // any stopped MB containers back up. This is the recovery path for
        // "PlaybackScreen::leave() didn't run cleanly" cases — e.g. kiosk
        // crashed mid-playback or was SIGABRT'd by systemd watchdog. With
        // the docker-stop pause behavior, missed leave() = stranded
        // containers (Docker's restart=unless-stopped doesn't auto-start
        // manually-stopped containers). Without this safety: Movies entry
        // silently vanishes on next boot. unpause is idempotent — no-op
        // for already-running containers, starts stopped ones. Backgrounded
        // (& at end) so the ~1-2 s docker start doesn't block the kiosk
        // entering its main loop.
        std::system(
            "/usr/local/bin/playback_services_pause.sh unpause "
            ">/dev/null 2>&1 &");
    }
    return vpn_health_monitor;
}

}  // namespace media_browser
