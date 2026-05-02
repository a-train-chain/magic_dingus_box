#!/usr/bin/env bash
# perf_report.sh — Magic Dingus Box system fingerprint
#
# Run on the Pi to capture a one-page snapshot of:
#   • Service health (kiosk + docker stack)
#   • Memory + disk + CPU baseline
#   • Network round-trips (TMDB / Prowlarr / Radarr / qBit)
#   • Poster fetch + decode timing (cold → warm)
#   • Disk I/O on SD card vs USB SSD (the thumb drive holding the library)
#   • Indexer responsiveness (per-indexer search timing)
#
# Output is plain text — diff two runs to see what changed after a tweak.
# Designed to be safe to run while the kiosk is using the Media Browser
# (read-only HTTP probes + small one-shot disk benches; no service restarts,
# no destructive writes outside /tmp).
#
# Usage:
#   sudo /opt/magic_dingus_box/scripts/perf_report.sh > ~/perf_$(date +%Y%m%d_%H%M%S).log
#
# Exit status:
#   0 always — this is a measurement tool, not a test. A "FAIL" line in the
#   report flags an issue but doesn't fail the script.

set -uo pipefail

ENV_FILE="/opt/magic_dingus_box/services/.env"
LIB_ROOT="${STORAGE_ROOT:-/mnt/ssd}"

# ----------------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------------

print_header() {
    echo ""
    echo "============================================================"
    echo "  $1"
    echo "============================================================"
}

# Median + p95 of a list of milliseconds piped on stdin.
percentile_summary() {
    python3 -c "
import sys
vals = sorted(float(x) for x in sys.stdin.read().split() if x.strip())
if not vals:
    print('  (no samples)')
else:
    n = len(vals)
    median = vals[n // 2]
    p95 = vals[min(int(n * 0.95), n - 1)]
    avg = sum(vals) / n
    print(f'  n={n}  median={median:.0f}ms  p95={p95:.0f}ms  avg={avg:.0f}ms  min={vals[0]:.0f}ms  max={vals[-1]:.0f}ms')
"
}

# Wrapper for `curl -w` that emits just the latency in milliseconds.
# Args: timeout url [extra-curl-args...]
curl_ms() {
    local timeout="$1"
    local url="$2"
    shift 2
    curl -s -o /dev/null -m "$timeout" \
         -w '%{time_total}\n' "$@" "$url" 2>/dev/null \
        | awk '{print $1 * 1000}'
}

read_api_key() {
    grep -E "^${1}=" "$ENV_FILE" 2>/dev/null | cut -d= -f2
}

# ----------------------------------------------------------------------------
# Header
# ----------------------------------------------------------------------------

echo "Magic Dingus Box performance report"
echo "Generated: $(date)"
echo "Host:      $(hostname) ($(uname -r))"
echo "Uptime:    $(uptime -p)"

# ----------------------------------------------------------------------------
# Service health
# ----------------------------------------------------------------------------

print_header "1. Service health"

echo ""
echo "Kiosk service:"
echo "  active:      $(systemctl is-active magic-dingus-box-cpp.service 2>&1)"
echo "  watchdog:    $(systemctl show magic-dingus-box-cpp.service -p WatchdogUSec --value 2>/dev/null)"
echo "  uptime:      $(systemctl show magic-dingus-box-cpp.service -p ActiveEnterTimestamp --value 2>/dev/null)"
echo "  pid:         $(systemctl show magic-dingus-box-cpp.service -p MainPID --value 2>/dev/null)"

echo ""
echo "Docker stack:"
docker ps --format '  {{.Names}}: {{.Status}}' 2>/dev/null | grep -E "mdb_" | sort \
    || echo "  (docker unreachable)"

# ----------------------------------------------------------------------------
# Resources
# ----------------------------------------------------------------------------

print_header "2. Resources"

echo ""
echo "Memory:"
free -h | sed 's/^/  /'

echo ""
echo "CPU temperature: $(vcgencmd measure_temp 2>/dev/null || echo 'n/a')"

echo ""
echo "Load average: $(cut -d' ' -f1-3 /proc/loadavg)"

echo ""
echo "Disk usage:"
df -h / "$LIB_ROOT" 2>/dev/null | sed 's/^/  /' || df -h / | sed 's/^/  /'

# ----------------------------------------------------------------------------
# Network round-trip times
# ----------------------------------------------------------------------------

print_header "3. Network round-trips (3 samples each)"

probe_endpoint() {
    local label="$1"
    local url="$2"
    shift 2
    echo ""
    echo "${label}:"
    {
        for _ in 1 2 3; do
            curl_ms 8 "$url" "$@"
        done
    } | percentile_summary
}

probe_endpoint "TMDB CDN (image.tmdb.org)" \
    "https://image.tmdb.org/t/p/w500/qJ2tW6WMUDux911r6m7haRef0WH.jpg"

probe_endpoint "TMDB API (api.themoviedb.org)" \
    "https://api.themoviedb.org/3/configuration"

PROWLARR_KEY="$(read_api_key PROWLARR_API_KEY)"
if [[ -n "$PROWLARR_KEY" ]]; then
    probe_endpoint "Prowlarr /ping" \
        "http://localhost:9696/ping"
fi

RADARR_KEY="$(read_api_key RADARR_API_KEY)"
if [[ -n "$RADARR_KEY" ]]; then
    probe_endpoint "Radarr /ping" \
        "http://localhost:7878/ping"
fi

probe_endpoint "qBittorrent /api/v2/app/version (no-auth probe)" \
    "http://localhost:8080/api/v2/app/version"

# ----------------------------------------------------------------------------
# Poster fetch + decode timing
# ----------------------------------------------------------------------------

print_header "4. Poster fetch latency (10 samples, cold)"

# Hit 10 distinct TMDB posters. CDN edge caching makes the 1st request slower
# than subsequent ones — to keep numbers honest we ALWAYS use unique URLs by
# appending a cache-bust querystring. That measures the worst-case "first
# visit" latency the kiosk pays today (no persistent disk cache).
echo ""
echo "Fetch time (TMDB w500 posters, cache-busted):"
{
    SAMPLE_URLS=(
        "https://image.tmdb.org/t/p/w500/qJ2tW6WMUDux911r6m7haRef0WH.jpg"
        "https://image.tmdb.org/t/p/w500/yihdXomYb5kTeSivtFndMy5iDmf.jpg"
        "https://image.tmdb.org/t/p/w500/v7TaX8kXMXs5yFFGR41guUDNcnB.jpg"
        "https://image.tmdb.org/t/p/w500/8nzJve63EXA79HGAyidZwivZrQ2.jpg"
        "https://image.tmdb.org/t/p/w500/arJF829RP9cYvh0NU70dC5TtXSa.jpg"
        "https://image.tmdb.org/t/p/w500/xHEfx0aNsY6Ki4RRiD9jLe6JXm2.jpg"
        "https://image.tmdb.org/t/p/w500/jU4ZTkJxO2EjBGkUZ4XT9o2Opu9.jpg"
        "https://image.tmdb.org/t/p/w500/2yhg0mZQMhDyvUQ4rG1IZ4oIA8L.jpg"
        "https://image.tmdb.org/t/p/w500/m2zXTuNPkywdYLyWlVyJZW2QOJH.jpg"
        "https://image.tmdb.org/t/p/w500/5kCEHZnUeAZFJvE3l8K5OYjIwjj.jpg"
    )
    for u in "${SAMPLE_URLS[@]}"; do
        # Cache-bust so we measure cold-path latency every time
        curl_ms 15 "${u}?cb=$RANDOM"
    done
} | percentile_summary

# ----------------------------------------------------------------------------
# Disk I/O — SD card vs USB SSD
# ----------------------------------------------------------------------------

print_header "5. Disk I/O (one-shot 64MB write+read+drop_caches+read)"

bench_disk() {
    local label="$1"
    local target_dir="$2"
    if [[ ! -w "$target_dir" ]]; then
        echo "  ${label}: SKIP (not writable: ${target_dir})"
        return
    fi
    local f="${target_dir}/.perf_report_${$}_$RANDOM.dat"
    echo ""
    echo "${label}:"

    # Sequential write
    local w_secs
    w_secs=$( { time dd if=/dev/zero of="$f" bs=1M count=64 oflag=direct 2>/dev/null ; } 2>&1 \
              | awk '/real/ {print $2}' | sed 's/[ms]/ /g' | awk '{print ($1 * 60) + $2}')
    local w_mbs
    w_mbs=$(awk "BEGIN {printf \"%.0f\", 64.0 / ${w_secs:-1}}")
    echo "  write 64MB:        ${w_secs}s  (${w_mbs} MB/s)"

    # Hot read (page cache may help us)
    local hr_secs
    hr_secs=$( { time dd if="$f" of=/dev/null bs=1M 2>/dev/null ; } 2>&1 \
               | awk '/real/ {print $2}' | sed 's/[ms]/ /g' | awk '{print ($1 * 60) + $2}')
    echo "  hot read:          ${hr_secs}s"

    # Drop caches and read again — true cold disk read
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
    local cr_secs
    cr_secs=$( { time dd if="$f" of=/dev/null bs=1M 2>/dev/null ; } 2>&1 \
               | awk '/real/ {print $2}' | sed 's/[ms]/ /g' | awk '{print ($1 * 60) + $2}')
    local cr_mbs
    cr_mbs=$(awk "BEGIN {printf \"%.0f\", 64.0 / ${cr_secs:-1}}")
    echo "  cold read:         ${cr_secs}s  (${cr_mbs} MB/s)"

    rm -f "$f"
}

bench_disk "SD card (root /)"      "/tmp"
bench_disk "USB SSD (${LIB_ROOT})" "${LIB_ROOT}"

# ----------------------------------------------------------------------------
# Indexer responsiveness (Prowlarr per-indexer)
# ----------------------------------------------------------------------------

print_header "6. Prowlarr indexer search timing (Inception, all-categories)"

if [[ -n "$PROWLARR_KEY" ]]; then
    # Issue a single search and report which indexers responded with how
    # many results. Slow indexers stretch the worst-case picker latency.
    SEARCH_BODY=$(curl -s -m 30 -H "X-Api-Key: $PROWLARR_KEY" \
        "http://localhost:9696/api/v1/search?query=Inception&categories=2000" 2>/dev/null)
    if [[ -z "$SEARCH_BODY" ]]; then
        echo ""
        echo "  (no results — Prowlarr unreachable or empty response)"
    else
        echo ""
        echo "$SEARCH_BODY" | python3 -c "
import json, sys
try:
    data = json.load(sys.stdin)
except Exception as e:
    print(f'  parse failed: {e}')
    sys.exit(0)
by_ix = {}
for r in data:
    ix = r.get('indexer', '?')
    by_ix.setdefault(ix, []).append(r.get('seeders', 0))
for ix, seeds in sorted(by_ix.items(), key=lambda kv: -len(kv[1])):
    n = len(seeds)
    best = max(seeds) if seeds else 0
    print(f'  {ix:30s}  n={n:3d}  best_seeders={best}')
print(f'  ----')
print(f'  TOTAL across all indexers: {len(data)} releases')
"
    fi
else
    echo "  (PROWLARR_API_KEY not set — skipping)"
fi

# ----------------------------------------------------------------------------
# Library + cache state
# ----------------------------------------------------------------------------

print_header "7. Library + cache"

if [[ -n "$RADARR_KEY" ]]; then
    LIB_COUNT=$(curl -s -H "X-Api-Key: $RADARR_KEY" "http://localhost:7878/api/v3/movie" \
                | python3 -c "import json,sys; print(len(json.load(sys.stdin)))" 2>/dev/null)
    echo ""
    echo "Radarr library: ${LIB_COUNT:-?} movies"
fi

POSTER_CACHE_DIR="${LIB_ROOT}/cache/posters"
if [[ -d "$POSTER_CACHE_DIR" ]]; then
    POSTER_COUNT=$(find "$POSTER_CACHE_DIR" -type f 2>/dev/null | wc -l)
    POSTER_BYTES=$(du -sh "$POSTER_CACHE_DIR" 2>/dev/null | awk '{print $1}')
    echo "Persistent poster cache: ${POSTER_COUNT} files (${POSTER_BYTES})"
else
    echo "Persistent poster cache: not yet present"
    echo "  (populated on first run after cache feature lands)"
fi

# ----------------------------------------------------------------------------
# Recent kiosk warnings/errors (sanity tail)
# ----------------------------------------------------------------------------

print_header "8. Recent kiosk anomalies (last 5 min)"

RECENT_ANOMALIES=$(sudo journalctl -u magic-dingus-box-cpp.service \
    --since "5 minutes ago" --no-pager 2>/dev/null \
    | grep -iE "error|fail|abort|segfault|crash|killed|watchdog timeout" \
    | grep -ivE "TMDB HTTP|alsa|pulseaudio|artwork|already.*active|no joystick" \
    | tail -10)
if [[ -z "$RECENT_ANOMALIES" ]]; then
    echo ""
    echo "  (none — clean)"
else
    echo ""
    echo "$RECENT_ANOMALIES" | sed 's/^/  /'
fi

echo ""
echo "============================================================"
echo "  Report complete"
echo "============================================================"
