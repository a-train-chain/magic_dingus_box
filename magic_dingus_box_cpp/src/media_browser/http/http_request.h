#pragma once

// The one libcurl transport behind every Media Browser HTTP call (Radarr,
// Sonarr, Prowlarr, qBittorrent, TMDB, the artwork fetcher, the VPN health
// ping, the MB Settings probes). It owns the curl handle setup that used to
// be copy-pasted per client and had drifted (Radarr ran without NOSIGNAL).
//
// Deliberately thin: no retries, no JSON, no logging, no error mapping.
// Each client keeps its own retry policy, parsing and error strings, and
// decides what a status code means. CURLOPT_NOSIGNAL is ALWAYS set: these
// calls run on worker threads, and without it libcurl's resolver may use
// SIGALRM for DNS timeouts, which is unsafe in a multi-threaded process.

#include <curl/curl.h>

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace media_browser::http {

enum class Method { Get, Head, Post, Put, Delete };

struct Request {
    Method method = Method::Get;
    std::string url;
    // Full header lines, e.g. "X-Api-Key: abc". Sent in this order.
    std::vector<std::string> headers;
    // Request body (POSTFIELDS). Allowed on any verb except Get/Head —
    // Sonarr's bulk delete sends one on DELETE.
    std::optional<std::string> body;
    long timeout_secs = 0;          // CURLOPT_TIMEOUT; 0 = libcurl default (none)
    long connect_timeout_secs = 0;  // CURLOPT_CONNECTTIMEOUT; 0 = libcurl default
    std::string user_agent;         // empty = no User-Agent header
    bool follow_redirects = false;
    bool force_http_1_1 = false;
    bool tcp_keepalive = false;
    // Non-null: the transfer aborts (CURLE_ABORTED_BY_CALLBACK) once the
    // flag reads true, polled from libcurl's progress callback.
    const std::atomic<bool>* abort_flag = nullptr;
    // Collect the raw response header lines (CRLF stripped) into
    // Response::headers — qBittorrent's login reads its SID cookie there.
    bool capture_headers = false;
};

struct Response {
    CURLcode curl_code = CURLE_OK;
    long status = 0;  // 0 = no status line received
    std::string body;
    std::vector<std::string> headers;  // only when capture_headers
    // Empty on CURLE_OK; otherwise curl_easy_strerror(curl_code), or
    // "curl init failed" when no handle could be created.
    std::string error;

    bool transport_ok() const { return curl_code == CURLE_OK; }
};

// Perform one request. `reuse`, when non-null, is a caller-owned handle that
// is curl_easy_reset() and used instead of a fresh one: options go back to
// defaults but live connections, TLS sessions and the DNS cache survive —
// the artwork fetcher keeps one handle so its poster burst reuses one
// connection to the image CDN.
Response request(const Request& req, CURL* reuse = nullptr);

}  // namespace media_browser::http
