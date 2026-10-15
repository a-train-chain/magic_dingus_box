#pragma once

// Reader for the services stack's dotenv file (services/.env), written by
// setup_services.sh and the web admin's WireGuard upload. The kiosk reads
// API keys from it (Radarr/Sonarr/Prowlarr fallback when systemd's
// EnvironmentFile= did not propagate them) and probes WIREGUARD_PRIVATE_KEY
// for the Media Browser's VPN-configured gate.
//
// ONE parser on purpose. Three hand-rolled copies used to disagree: the API
// key reader kept surrounding quotes (RADARR_API_KEY="abc" authenticated as
// the literal `"abc"`), while the two WireGuard probes stripped any run of
// quote characters from either end. The rule now, matching how docker
// compose reads the same file:
//
//   - a definition is a line that starts with exactly `KEY=`
//     (comments, indented lines and longer key names never match);
//   - the FIRST definition wins (as in every previous copy);
//   - surrounding whitespace (incl. a CRLF's \r) is trimmed, then ONE
//     matching pair of '...' or "..." is removed; inner text is verbatim;
//   - an unreadable/missing file or absent key reads as "".

#include <istream>
#include <string>

namespace utils {

inline constexpr const char* kServicesEnvPath =
    "/opt/magic_dingus_box/services/.env";

std::string env_value_from_stream(std::istream& in, const std::string& key);

std::string read_env_value(const std::string& path, const std::string& key);

}  // namespace utils
