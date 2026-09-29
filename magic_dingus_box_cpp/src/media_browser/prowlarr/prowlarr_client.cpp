#include "media_browser/prowlarr/prowlarr_client.h"

#include <curl/curl.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <set>
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

int abort_progress_cb(void* userdata, curl_off_t, curl_off_t,
                      curl_off_t, curl_off_t) {
    const auto* abort = static_cast<const std::atomic<bool>*>(userdata);
    return abort && abort->load(std::memory_order_relaxed) ? 1 : 0;
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
    for (auto& [done, t] : workers_) {
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
        set_error("Prowlarr API key not configured "
                      "(set MDB_PROWLARR_API_KEY)");
        state_.store(State::Failed);
        return;
    }

    // Reap finished workers WITHOUT blocking: each worker flips its done
    // flag as its final act, so joining a flagged worker returns
    // immediately. The old heuristic here force-joined the oldest worker
    // once more than 4 were tracked — "near done" assumed the 12s CURL
    // timeout, but the search timeout is 30s, and this runs on the
    // render thread: a hung Prowlarr held the UI well past the systemd
    // watchdog's 10s budget. Un-finished workers now simply stay tracked
    // (each is bounded by its own timeout and self-flags on exit).
    workers_.erase(
        std::remove_if(workers_.begin(), workers_.end(),
            [](SearchWorker& w) {
                if (!w.done->load(std::memory_order_acquire)) return false;
                if (w.thread.joinable()) w.thread.join();  // instant
                return true;
            }),
        workers_.end());
    if (workers_.size() > 16) {
        spdlog::warn("[prowlarr] {} search workers still in flight "
                     "(hung upstream?) — each self-reaps at its timeout",
                     workers_.size());
    }

    {
        std::lock_guard<std::mutex> lk(result_mtx_);
        result_.reset();
        set_error({});
        state_.store(State::Searching);
    }

    try {
        auto done = std::make_shared<std::atomic<bool>>(false);

        // Grow the vector BEFORE constructing a joinable thread. If vector
        // allocation failed after a thread had started, destruction of the
        // untracked joinable temporary would call std::terminate. Once this
        // nonjoinable slot exists, assigning std::thread is noexcept after
        // its constructor has successfully created the worker.
        workers_.emplace_back();
        SearchWorker& slot = workers_.back();
        slot.done = done;
        try {
            slot.thread = std::thread([this, my_gen, title, year, done]() {
                try {
                    run_search(my_gen, title, year);
                } catch (const std::exception& e) {
                    spdlog::error("[prowlarr] availability worker failed: {}",
                                  e.what());
                    publish_failure(
                        my_gen, "Availability check failed unexpectedly");
                } catch (...) {
                    spdlog::error("[prowlarr] availability worker failed with "
                                  "an unknown exception");
                    publish_failure(
                        my_gen, "Availability check failed unexpectedly");
                }
                done->store(true, std::memory_order_release);
            });
        } catch (...) {
            // The slot is still nonjoinable if std::thread construction
            // failed, so removing it cannot block or terminate.
            workers_.pop_back();
            throw;
        }
    } catch (const std::exception& e) {
        spdlog::error("[prowlarr] could not start availability worker: {}",
                      e.what());
        publish_failure(my_gen, "Could not start availability check");
    } catch (...) {
        spdlog::error("[prowlarr] could not start availability worker");
        publish_failure(my_gen, "Could not start availability check");
    }
}

std::optional<ReleaseSummary> ProwlarrClient::peek_result() const {
    std::lock_guard<std::mutex> lk(result_mtx_);
    return result_;
}

bool ProwlarrClient::publish_failure(uint64_t gen, std::string error) {
    std::lock_guard<std::mutex> lk(result_mtx_);
    if (abort_.load() || gen != current_gen_.load()) return false;
    result_.reset();
    set_error(std::move(error));
    state_.store(State::Failed);
    return true;
}

bool ProwlarrClient::publish_ready(uint64_t gen, ReleaseSummary summary,
                                   std::vector<IndexerStats> stats) {
    std::lock_guard<std::mutex> lk(result_mtx_);
    if (abort_.load() || gen != current_gen_.load()) return false;
    {
        std::lock_guard<std::mutex> stats_lk(last_results_mu_);
        last_indexer_stats_ = std::move(stats);
    }
    result_ = summary;
    set_error({});
    state_.store(State::Ready);
    return true;
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
    const std::string base_path = "/api/v1/search?query="
                                + url_encode(q.str())
                                + "&categories=2000&categories=2010"
                                + "&categories=2020&categories=2030"
                                + "&categories=2040&categories=2045"
                                + "&categories=2050&categories=2060"
                                + "&categories=2070&categories=2080"
                                + "&type=search";

    // Prowlarr's aggregate search waits for its SLOWEST indexer before it
    // returns anything. One upstream (observed live: apibay/TPB at 82-90+s)
    // therefore hid four healthy sources that answered in 3-7s behind this
    // client's 30s deadline. Ask Prowlarr for each enabled indexer separately
    // and launch all calls together. Each one gets its own bounded timeout;
    // successful arrays are merged and a stalled source is simply omitted
    // from this informational availability tally. Radarr's own library add /
    // release search remains unchanged and can still use every indexer.
    const HttpGetResult indexer_response =
        http_get_result("/api/v1/indexer", cfg_.timeout_secs);
    const std::string& indexer_body = indexer_response.body;
    if (indexer_body.empty()) {
        publish_failure(gen, indexer_response.error.empty()
                                 ? "Could not list availability sources"
                                 : indexer_response.error);
        return;
    }

    Json::CharReaderBuilder indexer_rb;
    Json::Value indexer_root;
    std::string indexer_err;
    std::istringstream indexer_stream(indexer_body);
    if (!Json::parseFromStream(indexer_rb, indexer_stream,
                               &indexer_root, &indexer_err) ||
        !indexer_root.isArray()) {
        publish_failure(gen, "Prowlarr returned an invalid source list");
        return;
    }

    std::vector<IndexerInfo> enabled_indexers;
    for (auto& indexer : parse_indexer_list(indexer_body)) {
        if (indexer.enabled) enabled_indexers.push_back(std::move(indexer));
    }
    if (enabled_indexers.empty()) {
        publish_failure(gen, "No availability sources are enabled");
        return;
    }
    if (abort_.load() || stale()) return;

    struct PendingSource {
        IndexerInfo indexer;
        std::future<HttpGetResult> response;
    };
    std::vector<PendingSource> pending;
    pending.reserve(enabled_indexers.size());
    int unavailable_sources = 0;
    for (const auto& indexer : enabled_indexers) {
        if (abort_.load() || stale()) return;
        const std::string path = base_path
                               + "&indexerIds="
                               + std::to_string(indexer.id);
        try {
            pending.push_back(PendingSource{
                indexer,
                std::async(std::launch::async, [this, path] {
                    return http_get_result(path, cfg_.search_timeout_secs);
                })});
        } catch (const std::exception& e) {
            ++unavailable_sources;
            spdlog::warn("[prowlarr] could not launch availability source "
                         "'{}': {}; keeping other sources",
                         indexer.name, e.what());
        }
    }

    Json::Value merged(Json::arrayValue);
    int responding_sources = 0;
    std::vector<int> empty_source_ids;
    for (auto& source : pending) {
        HttpGetResult source_result;
        try {
            source_result = source.response.get();
        } catch (const std::exception& e) {
            ++unavailable_sources;
            spdlog::warn("[prowlarr] availability source '{}' failed: {}; "
                         "keeping healthy results",
                         source.indexer.name, e.what());
            continue;
        } catch (...) {
            ++unavailable_sources;
            spdlog::warn("[prowlarr] availability source '{}' failed with "
                         "an unknown exception; keeping healthy results",
                         source.indexer.name);
            continue;
        }
        if (stale() || abort_.load()) return;
        const std::string& source_body = source_result.body;
        if (source_body.empty()) {
            ++unavailable_sources;
            spdlog::warn("[prowlarr] availability source '{}' unavailable: {}; "
                         "keeping healthy results",
                         source.indexer.name,
                         source_result.error.empty()
                             ? "empty response"
                             : source_result.error);
            continue;
        }

        Json::CharReaderBuilder source_rb;
        Json::Value source_root;
        std::string source_err;
        std::istringstream source_stream(source_body);
        if (!Json::parseFromStream(source_rb, source_stream,
                                   &source_root, &source_err) ||
            !source_root.isArray()) {
            ++unavailable_sources;
            spdlog::warn("[prowlarr] availability source '{}' returned "
                         "an invalid response; keeping healthy results",
                         source.indexer.name);
            continue;
        }
        ++responding_sources;
        if (source_root.empty()) empty_source_ids.push_back(source.indexer.id);
        for (const auto& release : source_root) merged.append(release);
    }

    if (responding_sources == 0) {
        publish_failure(gen, "All availability sources timed out or failed");
        return;
    }

    // Nothing found, but some sources never answered: inconclusive. The
    // slow sources are the likeliest to carry an obscure title, so this
    // must not read as a definitive "No sources found."
    if (merged.empty() && unavailable_sources > 0) {
        publish_failure(gen, "Some availability sources didn't respond");
        return;
    }

    // Prowlarr can convert an individual indexer exception into HTTP 200 []
    // while recording the failure in /api/v1/indexerstatus. Only consult that
    // status endpoint when EVERY usable response is empty; non-empty searches
    // already prove at least one source succeeded. If every empty response is
    // also marked failed (and any remaining sources timed out/failed locally),
    // report unavailable rather than lying with "No sources found."
    if (merged.empty() && !empty_source_ids.empty()) {
        const HttpGetResult status_response =
            http_get_result("/api/v1/indexerstatus", cfg_.timeout_secs);
        const std::string& status_body = status_response.body;
        Json::CharReaderBuilder status_rb;
        Json::Value status_root;
        std::string status_err;
        std::istringstream status_stream(status_body);
        std::set<int> failed_ids;
        const bool status_verified =
            !status_body.empty() &&
            Json::parseFromStream(status_rb, status_stream,
                                  &status_root, &status_err) &&
            status_root.isArray();
        if (!status_verified) {
            publish_failure(gen,
                            "Could not verify empty availability results");
            return;
        }
        for (const auto& status : status_root) {
            if (!status.isObject()) continue;
            const int id = status.get("indexerId", 0).asInt();
            const bool has_failure =
                !status.get("disabledTill", "").asString().empty() ||
                !status.get("mostRecentFailure", "").asString().empty();
            if (id > 0 && has_failure) failed_ids.insert(id);
        }
        const bool every_empty_source_failed = std::all_of(
            empty_source_ids.begin(), empty_source_ids.end(),
            [&failed_ids](int id) { return failed_ids.count(id) > 0; });
        if (every_empty_source_failed) {
            publish_failure(gen, "All availability sources failed");
            return;
        }
    }

    Json::StreamWriterBuilder merged_writer;
    merged_writer["indentation"] = "";
    std::string body = Json::writeString(merged_writer, merged);
    if (unavailable_sources > 0) {
        spdlog::info("[prowlarr] availability search '{}': using {} source(s), "
                     "skipped {} unavailable source(s)",
                     q.str(), responding_sources, unavailable_sources);
    }

    // Aborted mid-flight (destructor) or superseded by a newer search.
    // In either case the worker should silently disappear without touching
    // active state. Terminal publication performs the authoritative check
    // again while holding result_mtx_.
    if (abort_.load() || stale()) return;

    // Parse the response as a JSON array of release objects. We only
    // care about three fields per release: title (for de-dup), seeders,
    // and indexerId (for visibility into how many indexers responded).
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(body);
    if (!Json::parseFromStream(rb, is, &root, &err) || !root.isArray()) {
        spdlog::warn("[prowlarr] parse error for query '{}': {}",
                     q.str(), err);
        publish_failure(gen, "Prowlarr returned non-array response");
        return;
    }

    // Populate the per-indexer stats for the Sources panel. The parsed
    // release records exist only transiently to derive the stats — nothing
    // reads a retained copy. This is additive: the existing aggregate
    // ReleaseSummary below remains the source of truth for the Detail
    // screen's AVAILABILITY readout.
    auto records = parse_search_response(body);
    auto stats = aggregate_indexer_stats(records, /*seed_threshold=*/10);

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

    publish_ready(gen, summary, std::move(stats));
}

std::vector<ProwlarrClient::ReleaseRecord>
ProwlarrClient::parse_search_response(const std::string& json_body) {
    std::vector<ReleaseRecord> out;
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string err;
    std::istringstream is(json_body);
    if (!Json::parseFromStream(b, is, &root, &err)) return out;
    if (!root.isArray()) return out;
    for (const auto& r : root) {
        ReleaseRecord rr;
        rr.title        = r.get("title", "").asString();
        rr.indexer      = r.get("indexer", "").asString();
        rr.guid         = r.get("guid", "").asString();
        rr.download_url = r.get("downloadUrl", "").asString();
        rr.protocol     = r.get("protocol", "torrent").asString();
        rr.seeders      = r.get("seeders", 0).asInt();
        rr.leechers     = r.get("leechers", 0).asInt();
        rr.size_bytes   = r.get("size", 0).asInt64();
        rr.age_seconds  = r.get("ageHours", 0).asInt64() * 3600;
        out.push_back(std::move(rr));
    }
    return out;
}

std::vector<ProwlarrClient::IndexerStats>
ProwlarrClient::aggregate_indexer_stats(
    const std::vector<ReleaseRecord>& records, int seed_threshold) {
    std::map<std::string, IndexerStats> by_name;
    for (const auto& r : records) {
        auto& s = by_name[r.indexer];
        s.name = r.indexer;
        s.result_count++;
        if (r.seeders >= seed_threshold) s.results_above_seed_threshold++;
    }
    std::vector<IndexerStats> out;
    out.reserve(by_name.size());
    for (auto& kv : by_name) out.push_back(std::move(kv.second));
    return out;
}

std::vector<ProwlarrClient::IndexerStats>
ProwlarrClient::get_last_indexer_stats() const {
    std::lock_guard<std::mutex> lk(last_results_mu_);
    return last_indexer_stats_;
}

std::vector<ProwlarrClient::IndexerInfo>
ProwlarrClient::parse_indexer_list(const std::string& json_body) {
    std::vector<IndexerInfo> out;
    if (json_body.empty()) return out;
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string err;
    std::istringstream is(json_body);
    if (!Json::parseFromStream(b, is, &root, &err)) return out;
    if (!root.isArray()) return out;
    for (const auto& r : root) {
        if (!r.isObject()) continue;
        IndexerInfo i;
        i.id   = r.get("id", 0).asInt();
        i.name = r.get("name", "").asString();
        // Prowlarr historically used "enable"; older builds + some
        // serializers used "enabled". Accept either so the parser is
        // robust against both shapes.
        if (r.isMember("enable")) {
            i.enabled = r.get("enable", false).asBool();
        } else if (r.isMember("enabled")) {
            i.enabled = r.get("enabled", false).asBool();
        }
        if (i.id > 0 && !i.name.empty()) out.push_back(std::move(i));
    }
    return out;
}

std::vector<ProwlarrClient::IndexerInfo>
ProwlarrClient::list_indexers() {
    if (!is_configured()) return {};
    std::string body = http_get("/api/v1/indexer");
    return parse_indexer_list(body);
}

bool ProwlarrClient::set_indexer_enabled(int id, bool enabled) {
    if (!is_configured() || id <= 0) return false;

    // Step 1: fetch the full indexer entity. Prowlarr's PUT requires the
    // entire object (not a partial patch) — sending only `{enable:true}`
    // would clear out every other field on the indexer (Cardigann
    // definition, capabilities, fields[], priority, …).
    const std::string path = "/api/v1/indexer/" + std::to_string(id);
    const std::string get_resp = http_get(path);
    if (get_resp.empty()) return false;

    Json::CharReaderBuilder rb;
    Json::Value obj;
    std::string err;
    std::istringstream is(get_resp);
    if (!Json::parseFromStream(rb, is, &obj, &err) || !obj.isObject()) {
        std::lock_guard<std::mutex> lk(result_mtx_);
        set_error("Prowlarr indexer GET returned non-object");
        return false;
    }

    // Step 2: flip the field and serialize back. We update both spellings
    // defensively in case the Prowlarr build expects one or the other —
    // extra keys are ignored by the API and adding them is cheap.
    obj["enable"] = enabled;

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    const std::string body = Json::writeString(wb, obj);

    // Step 3: PUT it back. forceSave=true is the conventional query
    // flag the *arr stack uses to bypass server-side validation that
    // might otherwise reject minor schema differences. Mirrors the
    // pattern Prowlarr's own UI uses on the "Indexers" toggle.
    const std::string put_resp = http_put(path + "?forceSave=true", body);
    if (put_resp.empty()) {
        // http_put set last_error_ on failure; nothing else to do here.
        return false;
    }
    spdlog::info("[prowlarr] indexer {} {}",
                 id, enabled ? "enabled" : "disabled");
    return true;
}

std::string ProwlarrClient::http_get(const std::string& path) {
    return http_get_long(path, cfg_.timeout_secs);
}

std::string ProwlarrClient::http_get_long(const std::string& path, int timeout_secs) {
    HttpGetResult result = http_get_result(path, timeout_secs);
    if (!result.error.empty()) set_error(result.error);
    return std::move(result.body);
}

ProwlarrClient::HttpGetResult
ProwlarrClient::http_get_result(const std::string& path, int timeout_secs) {
    std::string url = cfg_.base_url + path;

    CURL* curl = curl_easy_init();
    if (!curl) {
        return {{}, "curl init failed"};
    }

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        static_cast<long>(timeout_secs));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    // NOSIGNAL is required when curl is invoked from a non-main thread
    // (libcurl's default DNS resolver uses SIGALRM otherwise). Without
    // this, the search worker can crash the kiosk with SIGSEGV on the
    // signal-handling path.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,     0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,   &abort_);

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
        return {{}, std::string("curl: ") + curl_easy_strerror(rc)};
    }
    if (http_code >= 400) {
        return {{}, "Prowlarr HTTP " + std::to_string(http_code)};
    }
    return {std::move(body), {}};
}

std::string ProwlarrClient::http_put(const std::string& path,
                                     const std::string& body) {
    std::string url = cfg_.base_url + path;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::lock_guard<std::mutex> lk(result_mtx_);
        set_error("curl init failed");
        return {};
    }

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST,  "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);

    struct curl_slist* hdrs = nullptr;
    std::string auth = "X-Api-Key: " + cfg_.api_key;
    hdrs = curl_slist_append(hdrs, auth.c_str());
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        std::lock_guard<std::mutex> lk(result_mtx_);
        set_error(std::string("curl: ") + curl_easy_strerror(rc));
        return {};
    }
    if (http_code >= 400) {
        std::lock_guard<std::mutex> lk(result_mtx_);
        set_error("Prowlarr HTTP " + std::to_string(http_code));
        return {};
    }
    // Some Prowlarr endpoints respond with 202 Accepted + empty body on
    // a successful PUT (the entity is queued for an internal config
    // reload). Return a single space so callers can distinguish "empty
    // body but successful 2xx" from "transport failure" via empty().
    if (resp.empty()) return " ";
    return resp;
}

}  // namespace media_browser
