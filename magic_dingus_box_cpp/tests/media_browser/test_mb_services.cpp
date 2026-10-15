// tests/media_browser/test_mb_services.cpp
//
// media_browser/mb_services.h — the service-client setup moved out of
// main.cpp. main.cpp is in no test target, so before the move none of this
// (key chains, the mock fallbacks, the Sonarr TV prefix derivation) had
// any coverage at all.

#include <catch2/catch_test_macros.hpp>

#include "media_browser/mb_services.h"
#include "media_browser/prowlarr/prowlarr_client.h"
#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/radarr/radarr_mock.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/sonarr/sonarr_mock.h"
#include "media_browser/tmdb_client.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <unistd.h>

using media_browser::EnvLookup;

namespace {

namespace fs = std::filesystem;

// A fake process environment. Missing names read as unset (nullptr).
struct FakeEnv {
    std::map<std::string, std::string> vars;
    EnvLookup lookup() const {
        return [this](const char* name) -> const char* {
            auto it = vars.find(name);
            return it == vars.end() ? nullptr : it->second.c_str();
        };
    }
};

// A scratch directory removed on scope exit.
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("mdb_mb_services_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string write(const std::string& rel, const std::string& body) const {
        const fs::path p = path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << body;
        return p.string();
    }
};

const std::string kNoEnvFile = "/nonexistent/mdb-test/services/.env";

}  // namespace

TEST_CASE("resolve_service_api_key: kiosk var beats service var beats .env",
          "[mb_services]") {
    TempDir tmp;
    const std::string env_file = tmp.write("services/.env",
                                           "RADARR_API_KEY=\"from-file\"\n");
    FakeEnv env;

    // Stage 3: only the file has it (quotes stripped by read_env_value).
    CHECK(media_browser::resolve_service_api_key(
              env.lookup(), "MDB_RADARR_API_KEY", "RADARR_API_KEY", env_file) ==
          "from-file");

    // Stage 2 wins over the file.
    env.vars["RADARR_API_KEY"] = "from-systemd";
    CHECK(media_browser::resolve_service_api_key(
              env.lookup(), "MDB_RADARR_API_KEY", "RADARR_API_KEY", env_file) ==
          "from-systemd");

    // Stage 1 wins over both.
    env.vars["MDB_RADARR_API_KEY"] = "from-kiosk";
    CHECK(media_browser::resolve_service_api_key(
              env.lookup(), "MDB_RADARR_API_KEY", "RADARR_API_KEY", env_file) ==
          "from-kiosk");
}

TEST_CASE("resolve_service_api_key: empty variables are treated as unset",
          "[mb_services]") {
    TempDir tmp;
    const std::string env_file = tmp.write("services/.env",
                                           "SONARR_API_KEY=abc\n");
    FakeEnv env;
    env.vars["MDB_SONARR_API_KEY"] = "";
    env.vars["SONARR_API_KEY"] = "";
    CHECK(media_browser::resolve_service_api_key(
              env.lookup(), "MDB_SONARR_API_KEY", "SONARR_API_KEY", env_file) ==
          "abc");
    // No stage has a key.
    CHECK(media_browser::resolve_service_api_key(
              env.lookup(), "MDB_SONARR_API_KEY", "SONARR_API_KEY", kNoEnvFile)
              .empty());
}

TEST_CASE("resolve_tv_prefix: explicit TV var, then parent + tv, then default",
          "[mb_services]") {
    FakeEnv env;
    const std::string def = "/mnt/ssd/library/tv/";

    CHECK(media_browser::resolve_tv_prefix(env.lookup(), "MDB_HOST_TV_PREFIX",
                                           "MDB_HOST_LIBRARY_PREFIX", def) == def);

    // The field case: only the movie var moved (STORAGE_ROOT change). The
    // TV subtree follows it one level down, with or without a trailing '/'.
    env.vars["MDB_HOST_LIBRARY_PREFIX"] = "/mnt/nvme/library";
    CHECK(media_browser::resolve_tv_prefix(env.lookup(), "MDB_HOST_TV_PREFIX",
                                           "MDB_HOST_LIBRARY_PREFIX", def) ==
          "/mnt/nvme/library/tv/");
    env.vars["MDB_HOST_LIBRARY_PREFIX"] = "/mnt/nvme/library/";
    CHECK(media_browser::resolve_tv_prefix(env.lookup(), "MDB_HOST_TV_PREFIX",
                                           "MDB_HOST_LIBRARY_PREFIX", def) ==
          "/mnt/nvme/library/tv/");

    // An explicit TV var beats the derived one, normalized.
    env.vars["MDB_HOST_TV_PREFIX"] = "/srv/shows";
    CHECK(media_browser::resolve_tv_prefix(env.lookup(), "MDB_HOST_TV_PREFIX",
                                           "MDB_HOST_LIBRARY_PREFIX", def) ==
          "/srv/shows/");

    // Empty == unset at both tiers.
    env.vars["MDB_HOST_TV_PREFIX"] = "";
    env.vars["MDB_HOST_LIBRARY_PREFIX"] = "";
    CHECK(media_browser::resolve_tv_prefix(env.lookup(), "MDB_HOST_TV_PREFIX",
                                           "MDB_HOST_LIBRARY_PREFIX", def) == def);
}

TEST_CASE("resolve_tmdb_api_key: env var, else first line of the HOME key file",
          "[mb_services]") {
    TempDir tmp;
    FakeEnv env;
    env.vars["HOME"] = tmp.path.string();

    // No env var, no file.
    CHECK(media_browser::resolve_tmdb_api_key(env.lookup()).empty());

    // File: first line only, trailing CR / spaces trimmed.
    tmp.write(".config/magic_dingus_box/tmdb_api_key", "k3y-value \r\nsecond\n");
    CHECK(media_browser::resolve_tmdb_api_key(env.lookup()) == "k3y-value");

    // The env var wins over the file.
    env.vars["MDB_TMDB_API_KEY"] = "from-env";
    CHECK(media_browser::resolve_tmdb_api_key(env.lookup()) == "from-env");

    // No HOME and no env var: nothing.
    FakeEnv bare;
    CHECK(media_browser::resolve_tmdb_api_key(bare.lookup()).empty());
}

TEST_CASE("make_service_clients: no keys anywhere -> mocks, no Prowlarr",
          "[mb_services]") {
    TempDir tmp;
    FakeEnv env;
    env.vars["HOME"] = tmp.path.string();  // no TMDB key file there

    auto c = media_browser::make_service_clients(env.lookup(), kNoEnvFile);
    REQUIRE(c.radarr);
    REQUIRE(c.sonarr);
    REQUIRE(c.tmdb);
    REQUIRE(c.qbit);
    CHECK(dynamic_cast<media_browser::RadarrMockClient*>(c.radarr.get()) != nullptr);
    CHECK(dynamic_cast<media_browser::SonarrMockClient*>(c.sonarr.get()) != nullptr);
    // The screens key their "is TV real" decision off this flag.
    CHECK_FALSE(c.sonarr_configured);
    CHECK(c.prowlarr == nullptr);
}

TEST_CASE("make_service_clients: keys from the services .env -> real clients",
          "[mb_services]") {
    TempDir tmp;
    const std::string env_file = tmp.write(
        "services/.env",
        "RADARR_API_KEY=r\nSONARR_API_KEY=s\nPROWLARR_API_KEY=p\n");
    FakeEnv env;
    env.vars["HOME"] = tmp.path.string();

    auto c = media_browser::make_service_clients(env.lookup(), env_file);
    REQUIRE(c.radarr);
    REQUIRE(c.sonarr);
    CHECK(dynamic_cast<media_browser::RadarrMockClient*>(c.radarr.get()) == nullptr);
    CHECK(dynamic_cast<media_browser::SonarrMockClient*>(c.sonarr.get()) == nullptr);
    CHECK(c.sonarr_configured);
    CHECK(c.prowlarr != nullptr);
    CHECK(c.qbit != nullptr);
    CHECK(c.tmdb != nullptr);
}

TEST_CASE("make_service_clients: Sonarr derives its TV prefixes from the movie vars",
          "[mb_services]") {
    TempDir tmp;
    FakeEnv env;
    env.vars["HOME"] = tmp.path.string();
    env.vars["MDB_SONARR_API_KEY"] = "s";
    env.vars["MDB_CONTAINER_LIBRARY_PREFIX"] = "/data/lib";
    env.vars["MDB_HOST_LIBRARY_PREFIX"] = "/mnt/nvme/library";

    auto c = media_browser::make_service_clients(env.lookup(), kNoEnvFile);
    REQUIRE(c.sonarr_configured);
    // Container /data/lib/tv/... -> host /mnt/nvme/library/tv/...
    CHECK(c.sonarr->resolve_host_path("/data/lib/tv/Show/S01E01.mkv") ==
          "/mnt/nvme/library/tv/Show/S01E01.mkv");
    // Radarr stays mocked (no Radarr key) while Sonarr is real.
    CHECK(dynamic_cast<media_browser::RadarrMockClient*>(c.radarr.get()) != nullptr);
}
