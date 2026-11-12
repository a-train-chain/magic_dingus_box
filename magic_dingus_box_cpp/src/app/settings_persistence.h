#pragma once

#include "app_state.h"
#include "../utils/result.h"
#include <chrono>
#include <functional>
#include <string>

namespace app {

class SettingsPersistence {
public:
    // Save current settings.
    //
    // Serializes `state` on the CALLING thread (AppState is not
    // thread-safe) and hands the bytes to a background writer; the file
    // I/O — tmp create, fsync, rename, directory fsync — never runs on the
    // caller. Every call site (~23: each Settings toggle, the volume hold,
    // Media Browser sort/filter changes, the unlock sequence) is on the
    // render loop, which pings the systemd watchdog (WatchdogSec=10); with
    // the SD card saturated by a Content Manager upload that fsync alone
    // can outlast it (kiosk_status.json's plain create+rename was sampled
    // at ~6 s per call, 2026-10-26 — see status_writer.h).
    //
    // Latest wins: every save is a full snapshot, so one the writer has
    // not started is replaced by a newer one. The write is as durable and
    // atomic as before; it lands a moment after this returns instead of
    // before. What that changes for callers:
    //   - main() calls flush_pending_saves() before its _exit(), so the
    //     LAST change is not lost on a clean stop;
    //   - a reload from disk goes through reload_from_disk(), never bare
    //     load_settings(), so a save still in the air cannot land on top
    //     of the file that was just loaded.
    // A failed write is logged by the writer and retried once by
    // flush_pending_saves(); the returned Result only says the snapshot
    // was queued. Call from ONE thread (the main loop).
    static utils::Result<> save_settings(const AppState& state);

    // Blocks until every queued save is on disk, or `timeout` passes (a
    // wedged card must not hold shutdown past systemd's stop timeout). A
    // save that FAILED is retried once here. Returns false when a write is
    // still outstanding or the retry failed too. Shutdown and tests only —
    // NEVER the render loop.
    static bool flush_pending_saves(std::chrono::milliseconds timeout);

    // A save is queued or being written: the file on disk is older than
    // the kiosk's in-memory settings.
    static bool save_pending();

    // load_settings() for a file something ELSE just replaced (the web
    // admin's backup restore, then settings_reload_request). If one of our
    // own saves was still queued or in flight it predates the restore, and
    // would otherwise land on top of it AFTER this load — leaving the disk
    // with settings the kiosk no longer has in memory until the next save
    // (or forever, across a reboot). So in that case the loaded state is
    // saved again: it replaces a queued save and follows one in flight,
    // and the disk converges on what was loaded.
    static utils::Result<> reload_from_disk(AppState& state);

    // The exact bytes save_settings() writes for `state`.
    static std::string serialize(const AppState& state);

    // Test seam: replaces the durable file write. Runs on the writer
    // thread; returns false on failure. An empty sink restores the real
    // one. Applies to saves made after the call.
    using FileSink = std::function<bool(const std::string& path,
                                        const std::string& body)>;
    static void set_file_sink_for_tests(FileSink sink);

    // Load settings from file into state
    // Returns Result with error message on failure
    static utils::Result<> load_settings(AppState& state);

    // Read ONLY display.mode from settings.json, without touching
    // AppState. The DRM mode has to be chosen before the display is
    // initialised — hundreds of lines before load_settings() runs — and
    // MODERN_TV now boots 1920x1080 while CRT_NATIVE must stay at 720p
    // (no composite out on Pi 5; CRT rigs go through an HDMI->composite
    // converter that CRT_MAX_HEIGHT protects). Returns true for
    // CRT_NATIVE, which is also the safe default when the file is
    // missing or unreadable: an unprovisioned box must never come up
    // driving a converter at a resolution it may not accept.
    static bool peek_is_crt_native();

private:
    static std::string get_settings_path();
    static utils::Result<> load_settings_unguarded(AppState& state);
    // tmp write + fsync + rename + directory fsync. Writer thread only.
    static bool write_file_durably(const std::string& path,
                                   const std::string& body);
};

} // namespace app
