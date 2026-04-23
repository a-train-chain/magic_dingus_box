#include "media_browser/torrent/torrent_session.h"

#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/sha1_hash.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>
#include <filesystem>
#include <sstream>
#include <iomanip>

namespace lt = libtorrent;
namespace fs = std::filesystem;

namespace media_browser {

namespace {

// Portable hex conversion (avoids lt::aux internals).
std::string hex_from_info_hash(const lt::sha1_hash& h) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (auto b : h) {
        os << std::setw(2) << (static_cast<unsigned>(b) & 0xFF);
    }
    return os.str();
}

std::string human_state(lt::torrent_status::state_t s) {
    switch (s) {
        case lt::torrent_status::checking_files: return "checking";
        case lt::torrent_status::downloading_metadata: return "metadata";
        case lt::torrent_status::downloading: return "downloading";
        case lt::torrent_status::finished: return "finished";
        case lt::torrent_status::seeding: return "seeding";
        case lt::torrent_status::checking_resume_data: return "checking_resume";
        default: return "unknown";
    }
}

TorrentStatus to_status(const lt::torrent_status& ts) {
    TorrentStatus s;
    s.info_hash = hex_from_info_hash(ts.info_hashes.v1);
    s.name = ts.name;
    s.progress = ts.progress;
    s.download_rate_bps = ts.download_payload_rate;
    s.upload_rate_bps = ts.upload_payload_rate;
    s.num_peers = ts.num_peers;
    s.num_seeds = ts.num_seeds;
    s.state = human_state(ts.state);
    s.is_finished = ts.is_finished;
    if (ts.errc) {
        s.has_error = true;
        s.error_message = ts.errc.message();
    }
    return s;
}

}  // namespace

TorrentSession::TorrentSession(Config config) : cfg_(std::move(config)) {
    fs::create_directories(cfg_.download_dir);
    fs::create_directories(cfg_.complete_dir);

    lt::settings_pack pack;
    pack.set_str(lt::settings_pack::user_agent, "MagicDingusBox/1.0");
    pack.set_int(lt::settings_pack::alert_mask,
        lt::alert_category::error |
        lt::alert_category::storage |
        lt::alert_category::status);
    pack.set_bool(lt::settings_pack::enable_dht, true);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    pack.set_bool(lt::settings_pack::enable_upnp, true);
    pack.set_bool(lt::settings_pack::enable_natpmp, true);
    pack.set_int(lt::settings_pack::download_rate_limit, cfg_.max_download_rate_bps);
    pack.set_int(lt::settings_pack::upload_rate_limit, cfg_.max_upload_rate_bps);
    pack.set_int(lt::settings_pack::active_downloads, cfg_.active_downloads);

    ses_ = std::make_unique<lt::session>(pack);
    spdlog::info("[media_browser] torrent session started, dir={}", cfg_.download_dir);
}

TorrentSession::~TorrentSession() {
    if (ses_) {
        ses_->pause();
    }
}

std::string TorrentSession::add_magnet(const std::string& uri) {
    lt::error_code ec;
    lt::add_torrent_params p = lt::parse_magnet_uri(uri, ec);
    if (ec) {
        spdlog::error("[media_browser] parse_magnet_uri: {}", ec.message());
        return {};
    }
    p.save_path = cfg_.download_dir;
    lt::torrent_handle h = ses_->add_torrent(std::move(p), ec);
    if (ec) {
        spdlog::error("[media_browser] add_torrent: {}", ec.message());
        return {};
    }
    return hex_from_info_hash(h.info_hashes().v1);
}

std::string TorrentSession::add_torrent_file(const std::string& path) {
    lt::error_code ec;
    lt::add_torrent_params p;
    p.ti = std::make_shared<lt::torrent_info>(path, ec);
    if (ec) {
        spdlog::error("[media_browser] torrent_info({}): {}", path, ec.message());
        return {};
    }
    p.save_path = cfg_.download_dir;
    lt::torrent_handle h = ses_->add_torrent(std::move(p), ec);
    if (ec) {
        spdlog::error("[media_browser] add_torrent: {}", ec.message());
        return {};
    }
    return hex_from_info_hash(h.info_hashes().v1);
}

std::vector<TorrentStatus> TorrentSession::list() {
    std::vector<TorrentStatus> out;
    for (const auto& h : ses_->get_torrents()) {
        out.push_back(to_status(h.status()));
    }
    return out;
}

bool TorrentSession::get_status(const std::string& info_hash, TorrentStatus& out) {
    for (const auto& h : ses_->get_torrents()) {
        auto hex = hex_from_info_hash(h.info_hashes().v1);
        if (hex == info_hash) {
            out = to_status(h.status());
            return true;
        }
    }
    return false;
}

void TorrentSession::pump_alerts() {
    std::vector<lt::alert*> alerts;
    ses_->pop_alerts(&alerts);
    for (auto* a : alerts) {
        if (auto* e = lt::alert_cast<lt::torrent_error_alert>(a)) {
            spdlog::error("[media_browser] torrent error: {}", e->message());
        } else if (auto* f = lt::alert_cast<lt::torrent_finished_alert>(a)) {
            spdlog::info("[media_browser] torrent finished: {}", f->message());
            // Optionally: move to complete_dir here. Phase 1 leaves files in place.
        }
    }
}

bool TorrentSession::wait_for_completion(const std::string& info_hash, int timeout_secs) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::seconds(timeout_secs);
    while (clock::now() < deadline) {
        pump_alerts();
        TorrentStatus s;
        if (get_status(info_hash, s)) {
            if (s.is_finished) return true;
            if (s.has_error) return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

bool TorrentSession::remove(const std::string& info_hash, bool delete_files) {
    for (const auto& h : ses_->get_torrents()) {
        auto hex = hex_from_info_hash(h.info_hashes().v1);
        if (hex == info_hash) {
            ses_->remove_torrent(h,
                delete_files ? lt::session::delete_files : lt::remove_flags_t{});
            return true;
        }
    }
    return false;
}

}  // namespace media_browser
