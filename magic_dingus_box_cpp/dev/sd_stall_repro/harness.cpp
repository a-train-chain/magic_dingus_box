// Off-box reproduction of the 2026-11-15 watchdog kill: a stall reload on
// the REAL GstPlayer while the media file's reads take seconds, as they do
// behind an SD card saturated by writes. See README.md.
//
//   harness <video> <read_delay_ms> <open_delay_ms>
//
// read()/pread()/open() are interposed (this executable is linked with
// -rdynamic, so its definitions win over libc's for every GStreamer plugin)
// and delayed only for paths containing "/media/". The "render loop" below
// does what main.cpp does with the player each frame; its longest gap is
// what the systemd watchdog (WatchdogSec=10) sees.
#include "video/gst_player.h"

#include <gst/app/gstappsink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

using clk = std::chrono::steady_clock;

namespace {
std::atomic<int> g_read_delay_ms{0};
std::atomic<int> g_open_delay_ms{0};
std::atomic<bool> g_slow_fd[4096];
const char* const kMatch = "/media/";

bool matches(const char* p) { return p && std::strstr(p, kMatch); }
void track(int fd, const char* path) {
    if (fd >= 0 && fd < 4096) g_slow_fd[fd] = matches(path);
}
void read_delay(int fd) {
    if (fd >= 0 && fd < 4096 && g_slow_fd[fd]) {
        const int ms = g_read_delay_ms.load();
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}
void open_delay(const char* path) {
    if (!matches(path)) return;
    const int ms = g_open_delay_ms.load();
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
mode_t mode_arg(int flags, va_list ap) {
    return (flags & (O_CREAT | O_TMPFILE)) ? static_cast<mode_t>(va_arg(ap, int)) : 0;
}
}  // namespace

extern "C" {
using read_fn = ssize_t (*)(int, void*, size_t);
using pread_fn = ssize_t (*)(int, void*, size_t, off_t);
using open_fn = int (*)(const char*, int, ...);
using openat_fn = int (*)(int, const char*, int, ...);

ssize_t read(int fd, void* b, size_t n) {
    static auto real = reinterpret_cast<read_fn>(dlsym(RTLD_NEXT, "read"));
    read_delay(fd);
    return real(fd, b, n);
}
ssize_t pread(int fd, void* b, size_t n, off_t o) {
    static auto real = reinterpret_cast<pread_fn>(dlsym(RTLD_NEXT, "pread"));
    read_delay(fd);
    return real(fd, b, n, o);
}
ssize_t pread64(int fd, void* b, size_t n, off_t o) {
    static auto real = reinterpret_cast<pread_fn>(dlsym(RTLD_NEXT, "pread64"));
    read_delay(fd);
    return real(fd, b, n, o);
}
int open(const char* path, int flags, ...) {
    static auto real = reinterpret_cast<open_fn>(dlsym(RTLD_NEXT, "open"));
    va_list ap; va_start(ap, flags); const mode_t m = mode_arg(flags, ap); va_end(ap);
    open_delay(path);
    const int fd = real(path, flags, m);
    track(fd, path);
    return fd;
}
int open64(const char* path, int flags, ...) {
    static auto real = reinterpret_cast<open_fn>(dlsym(RTLD_NEXT, "open64"));
    va_list ap; va_start(ap, flags); const mode_t m = mode_arg(flags, ap); va_end(ap);
    open_delay(path);
    const int fd = real(path, flags, m);
    track(fd, path);
    return fd;
}
int openat(int dfd, const char* path, int flags, ...) {
    static auto real = reinterpret_cast<openat_fn>(dlsym(RTLD_NEXT, "openat"));
    va_list ap; va_start(ap, flags); const mode_t m = mode_arg(flags, ap); va_end(ap);
    open_delay(path);
    const int fd = real(dfd, path, flags, m);
    track(fd, path);
    return fd;
}
int openat64(int dfd, const char* path, int flags, ...) {
    static auto real = reinterpret_cast<openat_fn>(dlsym(RTLD_NEXT, "openat64"));
    va_list ap; va_start(ap, flags); const mode_t m = mode_arg(flags, ap); va_end(ap);
    open_delay(path);
    const int fd = real(dfd, path, flags, m);
    track(fd, path);
    return fd;
}
}  // extern "C"

namespace {
// GstPlayer::frames_current() where it exists — `run.sh --ref` builds this
// against trees from before the control thread too.
template <typename P>
auto frames_current(P& p, int) -> decltype(p.frames_current()) { return p.frames_current(); }
template <typename P>
bool frames_current(P&, long) { return true; }

double secs(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

// One render-loop iteration, as main.cpp drives the player.
struct Loop {
    video::GstPlayer& p;
    clk::time_point last = clk::now();
    double max_gap = 0;
    void tick() {
        const auto now = clk::now();
        max_gap = std::max(max_gap, secs(last, now));
        last = now;
        p.update_state();
        if (frames_current(p, 0)) {
            if (GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(p.get_appsink()), 0)) {
                gst_sample_unref(s);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    void run_for(double s) {
        const auto end = clk::now() + std::chrono::duration<double>(s);
        while (clk::now() < end) tick();
    }
    void reset() { max_gap = 0; last = clk::now(); }
};

template <typename F>
double timed(const char* what, F f) {
    const auto a = clk::now();
    f();
    const double d = secs(a, clk::now());
    std::printf("  %-28s %7.3f s\n", what, d);
    return d;
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* file = argc > 1 ? argv[1] : "/media/a.mp4";
    const int read_ms = argc > 2 ? std::atoi(argv[2]) : 15000;
    const int open_ms = argc > 3 ? std::atoi(argv[3]) : 0;

    gst_init(nullptr, nullptr);
    video::GstPlayer p;
    if (!p.initialize()) {
        std::puts("GstPlayer::initialize failed");
        return 1;
    }
    // No PulseAudio in the container: a clock-synced fakesink stands in.
    GstElement* fake = gst_element_factory_make("fakesink", "audio-sink");
    g_object_set(fake, "sync", TRUE, nullptr);
    g_object_set(p.get_pipeline(), "audio-sink", fake, nullptr);

    Loop loop{p};
    p.load_file(file, 0.0);
    loop.run_for(4.0);
    std::printf("playing=%d at %.2f s\n", static_cast<int>(p.is_playing()), p.get_position());

    std::printf("card saturated: each media read takes %d ms, each open %d ms\n", read_ms, open_ms);
    g_read_delay_ms = read_ms;
    g_open_delay_ms = open_ms;
    // PlaybackStallWatchdog: the position frozen for 3 s while playing.
    double last_pos = p.get_position();
    auto frozen_since = clk::now();
    while (secs(frozen_since, clk::now()) < 3.0) {
        loop.tick();
        const double pos = p.get_position();
        if (pos != last_pos) {
            last_pos = pos;
            frozen_since = clk::now();
        }
    }
    std::printf("stalled at %.2f s: stall reload, in Controller::load_item_media's order\n", last_pos);

    loop.reset();
    const auto reload_start = clk::now();
    double worst = 0;
    worst = std::max(worst, timed("stop()", [&] { p.stop(); }));
    loop.reset();
    loop.run_for(0.2);  // wait_with_callback(200)
    worst = std::max(worst, timed("load_file(path, offset)", [&] { p.load_file(file, last_pos); }));
    worst = std::max(worst, timed("set_volume()", [&] { p.set_volume(80); }));
    worst = std::max(worst, timed("play()", [&] { p.play(); }));
    loop.reset();
    const auto wait_end = clk::now() + std::chrono::milliseconds(1000);  // wait_for_playback_start
    while (clk::now() < wait_end && !p.is_playing() && !p.has_error()) loop.tick();
    worst = std::max(worst, timed("get_volume()", [&] { (void)p.get_volume(); }));
    std::printf("reload: %.3f s on the loop, longest call %.3f s, longest loop gap %.3f s\n",
                secs(reload_start, clk::now()), worst, loop.max_gap);
    std::printf("RESULT %s (a call or gap over 10 s is a systemd watchdog kill)\n",
                std::max(worst, loop.max_gap) < 1.5 ? "loop-never-blocked" : "loop-BLOCKED");

    std::printf("card recovers\n");
    g_read_delay_ms = 0;
    g_open_delay_ms = 0;
    loop.reset();
    for (int i = 1; i <= 6; ++i) {
        loop.run_for(5.0);
        std::printf("  +%2d s: playing=%d at %.2f s, error=%d\n", i * 5,
                    static_cast<int>(p.is_playing()), p.get_position(),
                    static_cast<int>(p.has_error()));
    }
    std::printf("longest loop gap after the reload: %.3f s\n", loop.max_gap);
    p.cleanup();
    std::fflush(stdout);
    std::_Exit(0);
}
