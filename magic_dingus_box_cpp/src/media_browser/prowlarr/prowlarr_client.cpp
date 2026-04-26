#include "media_browser/prowlarr/prowlarr_client.h"

#include <curl/curl.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <sstream>

namespace media_browser {

namespace {

// Adult-content title filter for Prowlarr search results. We don't ban
// "sex" or "xxx" alone (legitimate films like "xXx" or "Sex and the
// City" use them). Instead we ban unambiguous porn studio watermarks
// and adult descriptors that would never appear in a legit release
// title. This is exclusively about the AVAILABILITY readout on Detail
// — actual download-picking is done by Radarr, which uses TMDB title
// matching (so a search for "Toy Story" won't auto-grab "Toy Story
// XXX Parody"). R-rated content (violence, language, mature themes)
// is intentionally NOT filtered — only commercial pornography.
const char* kAdultMarkers[] = {
    // Studio watermarks — every release from these studios includes
    // the studio name in the release title. These are unique enough
    // strings that they don't collide with legitimate film titles.
    "brazzers", "bangbros", "naughtyamerica", "naughty america",
    "realitykings", "reality kings", "evilangel", "evil angel",
    "kink.com", "pornhub", "blacked.com", "blacked raw",
    "vixen.com", "deeper.com", "tushy.com", "tushyraw",
    "wickedpictures", "wicked pictures", "digitalplayground",
    "digital playground", "metart", "lesbea", "nubilefilms",
    "nubile films", "mofos", "fakehub", "fake hub", "private.com",
    "manyvids", "onlyfans", "milfed.com", "tube8", "youporn",
    "redtube", "youjizz", "spankbang", "porngate",

    // Compound adult phrases — substrings that won't appear in any
    // legitimate movie title. Single-word generics like "sex", "xxx",
    // "porn" are deliberately excluded (they appear in legit films:
    // "Sex and the City", "xXx" Vin Diesel, etc.). Single-word
    // anatomical/act terms ("creampie", "deepthroat", "gangbang",
    // "cumshot", "bukkake") are also excluded — "Deep Throat" is a
    // 1972 film with documentary releases on indexers, and the bare
    // substring would make those silently disappear from the seeder
    // count. The phrases below are specifically pornographic
    // commercial-release watermarks.
    "xxx parody", "porn parody", "pornstar",
};

// Lowercase substring match. Returns true if title contains any
// adult-marker substring. Case-insensitive comparison since release
// titles vary in capitalization (BRAZZERS vs Brazzers vs brazzers).
bool title_looks_adult(const std::string& title) {
    std::string lower;
    lower.reserve(title.size());
    for (char c : title) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    for (const char* needle : kAdultMarkers) {
        if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

// curl write callback identical to the one in radarr_client.cpp /
// mb_settings_screen.cpp. Duplicated rather than extracted to keep this
// translation unit self-contained — the function is 4 lines.
size_t write_cb(char* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// URL-encode a query string component using libcurl's encoder. Returns
// the input unchanged if curl init fails (defensive — should never
// happen in practice).
std::string url_encode(const std::string& s) {
    CURL* c = curl_easy_init();
    if (!c) return s;
    char* encoded = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
    std::string out = encoded ? std::string(encoded) : s;
    if (encoded) curl_free(encoded);
    curl_easy_cleanup(c);
    return out;
}

}  // namespace

ProwlarrClient::ProwlarrClient(Config cfg) : cfg_(std::move(cfg)) {}

ProwlarrClient::~ProwlarrClient() {
    cancel();
}

void ProwlarrClient::cancel() {
    // Stop ALL workers before destruction. Bumps the generation counter
    // so any worker mid-CURL discards its eventual result, signals
    // abort_ as an early-exit hint, then joins every tracked worker.
    // Joining (rather than detaching) is critical for clean shutdown:
    // a detached worker holding a reference to *this can outlive the
    // ProwlarrClient instance and segfault on result publication.
    current_gen_.fetch_add(1);
    abort_.store(true);
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

void ProwlarrClient::search_async(const std::string& title, int year) {
    // Bump the generation counter unconditionally. Any in-flight worker
    // started under a previous generation will compare its captured
    // gen to current_gen_ at publish time, see they differ, and silently
    // drop its result — so we don't need to wait for it to finish.
    // This is the entire reason the UI thread doesn't block here.
    const uint64_t my_gen = current_gen_.fetch_add(1) + 1;

    if (!is_configured()) {
        // Surface a configured-vs-not signal through state. Caller can
        // distinguish from a network failure via peek_error().
        std::lock_guard<std::mutex> lk(result_mtx_);
        result_.reset();
        last_error_ = "Prowlarr API key not configured "
                      "(set MDB_PROWLARR_API_KEY)";
        state_.store(State::Failed);
        return;
    }

    // Opportunistically reap finished workers from prior search_async
    // calls. std::thread doesn't expose a non-blocking "is finished"
    // check, but we can join() a thread whose work has clearly ended
    // by checking whether it still maps to a running OS thread. We
    // settle for a simpler heuristic: if we have more than a handful
    // tracked, force-join the oldest few. They're either done (instant
    // join) or near done (CURL timeout is 12s — a worker that's been
    // alive that long is about to finish anyway).
    if (workers_.size() > 4) {
        // Join the oldest one. abort_ is already false from the
        // constructor / previous search_async, so this just waits for
        // its current iteration to finish naturally.
        if (workers_.front().joinable()) workers_.front().join();
        workers_.erase(workers_.begin());
    }

    state_.store(State::Searching);
    {
        std::lock_guard<std::mutex> lk(result_mtx_);
        result_.reset();
        last_error_.clear();
    }

    workers_.emplace_back(&ProwlarrClient::run_search, this,
                          my_gen, title, year);
}

std::optional<ReleaseSummary> ProwlarrClient::peek_result() const {
    std::lock_guard<std::mutex> lk(result_mtx_);
    return result_;
}

void ProwlarrClient::run_search(uint64_t gen, std::string title, int year) {
    // Helper: returns true if a newer search has been requested since we
    // started. When this happens the worker should silently abandon its
    // result — search_async has already updated state_ for the new
    // search and any write we do here would either clobber the new
    // search's "Searching" state or get clobbered by it.
    auto stale = [this, gen]() {
        return gen != current_gen_.load();
    };

    // Build the search query. Prowlarr's /api/v1/search endpoint takes
    // `query` (free-form) plus optional `categories[]` (movies = 2000)
    // and `type=search`. We tack on the year so the query disambiguates
    // remakes — "It (2017)" returns very different results from "It
    // (1990)" and Prowlarr passes the year through to the indexers'
    // tokenizers verbatim.
    std::ostringstream q;
    q << title;
    if (year > 0) q << ' ' << year;

    // Movies-only Newznab categories. We list the sub-categories
    // explicitly (rather than relying on the parent 2000) so an indexer
    // that only tags by sub-category still gets included, AND so we
    // never surface XXX (6000-series), Books (8000), or any other
    // non-movie content. Same set Radarr's syncCategories uses, kept in
    // sync with /opt/magic_dingus_box/services/.env's Prowlarr config.
    //   2000  Movies (parent)
    //   2010  Movies/Foreign     2050  Movies/HD
    //   2020  Movies/Other       2060  Movies/3D
    //   2030  Movies/SD          2070  Movies/UHD
    //   2040  Movies/HD          2080  Movies/BluRay
    //   2045  Movies/UHD
    std::string path = "/api/v1/search?query="
                     + url_encode(q.str())
                     + "&categories=2000&categories=2010&categories=2020"
                     + "&categories=2030&categories=2040&categories=2045"
                     + "&categories=2050&categories=2060&categories=2070"
                     + "&categories=2080"
                     + "&type=search";

    std::string body = http_get(path);

    // Aborted mid-flight (destructor) or superseded by a newer search.
    // In either case the worker should silently disappear without
    // touching state_ — search_async has already updated state_ for
    // the new search and any write here would clobber it.
    if (abort_.load() || stale()) return;

    if (body.empty()) {
        std::lock_guard<std::mutex> lk(result_mtx_);
        if (stale()) return;  // Re-check under the lock — defensive.
        if (last_error_.empty()) {
            // http_get sets last_error_ for hard failures; if it didn't,
            // an empty body with no curl error is an empty result set
            // (treat as zero releases rather than an error).
            ReleaseSummary empty;
            result_ = empty;
            state_.store(State::Ready);
            return;
        }
        state_.store(State::Failed);
        return;
    }

    // Parse the response as a JSON array of release objects. We only
    // care about three fields per release: title (for de-dup), seeders,
    // and indexerId (for visibility into how many indexers responded).
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(body);
    if (!Json::parseFromStream(rb, is, &root, &err) || !root.isArray()) {
        if (stale()) return;
        std::lock_guard<std::mutex> lk(result_mtx_);
        if (stale()) return;
        last_error_ = "Prowlarr returned non-array response";
        spdlog::warn("[prowlarr] parse error for query '{}': {}",
                     q.str(), err);
        state_.store(State::Failed);
        return;
    }

    ReleaseSummary summary;
    int filtered_adult = 0;
    for (const auto& r : root) {
        if (!r.isObject()) continue;

        // Family-safe filter: drop releases whose title contains an
        // unambiguous adult-content marker (porn studio watermark or
        // pornographic descriptor — see kAdultMarkers). This affects
        // ONLY the AVAILABILITY readout's seeder/release counts.
        // Radarr's actual download-picker is unaffected: when the user
        // adds a movie, Radarr matches releases by TMDB title which
        // already filters out "X Parody" content from legit grabs.
        const std::string title = r.get("title", "").asString();
        if (title_looks_adult(title)) {
            ++filtered_adult;
            continue;
        }

        // Prowlarr uses lowercase "seeders" and integer-typed values.
        // Some indexers omit the field entirely; default to 0.
        int seeders = 0;
        if (r.isMember("seeders") && r["seeders"].isIntegral()) {
            seeders = r["seeders"].asInt();
        }
        // Negative or absurd values: clamp at 0. Some indexers return
        // -1 to signal "unknown."
        if (seeders < 0) seeders = 0;
        summary.total_releases++;
        summary.total_seeders += seeders;
        if (seeders > summary.best_seeders) summary.best_seeders = seeders;
    }

    if (filtered_adult > 0) {
        spdlog::info("[prowlarr] search '{}': filtered {} adult-titled "
                     "result(s) before tally", q.str(), filtered_adult);
    }
    spdlog::info("[prowlarr] search '{}': {} releases, best={} seeders",
                 q.str(), summary.total_releases, summary.best_seeders);

    // Final stale-check before publishing. If a newer search has been
    // requested by the UI thread, drop our result on the floor — the
    // newer search already updated state_ to Searching and will publish
    // its own result. Writing State::Ready here would clobber that.
    if (stale()) return;
    {
        std::lock_guard<std::mutex> lk(result_mtx_);
        if (stale()) return;
        result_ = summary;
        last_error_.clear();
    }
    state_.store(State::Ready);
}

std::string ProwlarrClient::http_get(const std::string& path) {
    std::string url = cfg_.base_url + path;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::lock_guard<std::mutex> lk(result_mtx_);
        last_error_ = "curl init failed";
        return {};
    }

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    // NOSIGNAL is required when curl is invoked from a non-main thread
    // (libcurl's default DNS resolver uses SIGALRM otherwise). Without
    // this, the search worker can crash the kiosk with SIGSEGV on the
    // signal-handling path.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);

    struct curl_slist* hdrs = nullptr;
    std::string auth = "X-Api-Key: " + cfg_.api_key;
    hdrs = curl_slist_append(hdrs, auth.c_str());
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        std::lock_guard<std::mutex> lk(result_mtx_);
        last_error_ = std::string("curl: ") + curl_easy_strerror(rc);
        return {};
    }
    if (http_code >= 400) {
        std::lock_guard<std::mutex> lk(result_mtx_);
        last_error_ = "Prowlarr HTTP " + std::to_string(http_code);
        return {};
    }
    return body;
}

}  // namespace media_browser
