#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# CLAUDE.md dual-board rule 4: every system library the kiosk builds against
# is named in several hand-kept lists, and a list that falls behind ships a
# different binary. That is how libsystemd went missing from the CI release
# build: sd_notify compiled out and systemd killed a healthy kiosk on every
# box (v1.7.2). This suite turns "keep the lists in sync" into a check.
#
# Only BUILD-time (-dev) packages are compared. Runtime packages
# (gstreamer1.0-plugins-*, pulseaudio, auditd, ...) legitimately differ: CI
# builds the binary, it never runs it.
#
#   CMakeLists.txt          what the build asks pkg-config / find_package for
#   MODULE_MAP (below)      which apt package provides each of those
#   install_deps.sh         on-Pi builds          \  must all install exactly
#   release.yml             the OTA binary         | the -dev packages
#   test-local.yml          kiosk-build, per push  | MODULE_MAP names for
#   dev/pisim/Dockerfile    local arm64 builds    /  CMakeLists.txt
#   deploy_cpp.sh PKG_MAP   deploy-time top-up: must agree with MODULE_MAP
#   README.md               the human list: names every module
#
# Optional CMake deps (libgpiod, libsystemd) are covered like REQUIRED ones on
# purpose: they are the ones that compile out silently when a list drops them.

# <cmake module> <apt package, "-" = not an apt -dev> <name the README uses>
# The module column is exactly what CMakeLists.txt passes to
# pkg_check_modules (pkg-config name) or find_package.
MODULE_MAP='
PkgConfig            -                                 -        # the build tool itself
libdrm               libdrm-dev                        libdrm
gbm                  libgbm-dev                        libgbm
egl                  libegl-dev                        libegl   # egl.pc ships here (libglvnd); libegl1-mesa-dev only adds two unused Mesa extension headers
glesv2               libgles2-mesa-dev                 libgles2 # transitional on Trixie, pulls in libgles-dev (glesv2.pc)
libevdev             libevdev-dev                      libevdev
libgpiod             libgpiod-dev                      libgpiod
libsystemd           libsystemd-dev                    libsystemd
gstreamer-1.0        libgstreamer1.0-dev               gstreamer-1.0
gstreamer-app-1.0    libgstreamer-plugins-base1.0-dev  gstreamer-app-1.0
gstreamer-video-1.0  libgstreamer-plugins-base1.0-dev  gstreamer-video-1.0
gstreamer-gl-1.0     libgstreamer-plugins-base1.0-dev  gstreamer-gl-1.0
yaml-cpp             libyaml-cpp-dev                   yaml-cpp
jsoncpp              libjsoncpp-dev                    jsoncpp
libcurl              libcurl4-openssl-dev              libcurl
sqlite3              libsqlite3-dev                    libsqlite3
libtorrent-rasterbar libtorrent-rasterbar-dev          libtorrent-rasterbar
Boost                libboost-dev                      libboost-dev
'

# -dev packages allowed to appear in only some of the lists, or without a
# CMake consumer: one package per line, each with a comment saying why. Every
# entry is a place where an on-Pi build and the release binary may differ, so
# keep it short. Empty today.
DRIFT_ALLOWLIST='
'

setup() { cd "$TESTS_REPO_ROOT"; }

map_rows() { printf '%s\n' "$MODULE_MAP" | awk '{ sub(/#.*/, "") } NF >= 3'; }

# Field $2 (1 = module, 2 = apt package, 3 = README name) of module $1's row.
map_field() { map_rows | awk -v m="$1" -v c="$2" '$1 == m { print $c; exit }'; }

allowlisted() {
    printf '%s\n' "$DRIFT_ALLOWLIST" | awk '{ sub(/#.*/, "") } NF { print $1 }' | grep -qxF "$1"
}

# Every module CMakeLists.txt asks for, one per line. A pkg_check_modules call
# split over several lines prints "!UNPARSEABLE <line>" rather than being
# silently skipped.
cmake_modules() {
    awk '
        { sub(/#.*/, "") }
        /pkg_(check_modules|search_module)[[:space:]]*\(/ {
            if ($0 !~ /\)/) { print "!UNPARSEABLE " NR; next }
            s = $0; sub(/.*\(/, "", s); sub(/\).*/, "", s)
            n = split(s, w, /[[:space:]]+/); prefix_seen = 0
            for (i = 1; i <= n; i++) {
                if (w[i] == "") continue
                if (!prefix_seen) { prefix_seen = 1; continue }
                if (w[i] ~ /^(REQUIRED|QUIET|IMPORTED_TARGET|GLOBAL|NO_CMAKE_PATH|NO_CMAKE_ENVIRONMENT_PATH)$/) continue
                m = w[i]; sub(/[<>=].*/, "", m); print m
            }
        }
        /find_package[[:space:]]*\(/ {
            s = $0; sub(/.*find_package[[:space:]]*\([[:space:]]*/, "", s); sub(/[[:space:])].*/, "", s)
            print s
        }
    ' magic_dingus_box_cpp/CMakeLists.txt | sort -u
}

# The -dev packages named in apt install commands in file $1 (continuation
# lines followed). $2/$3, when given, are EREs bounding the section to read:
# reading starts after the line matching $2 and stops at the line matching $3.
apt_dev_packages() {
    awk -v start="${2:-}" -v stop="${3:-}" '
        start != "" && !on { if ($0 ~ start) on = 1; next }
        stop != "" && $0 ~ stop { exit }
        { line = $0; sub(/#.*/, "", line) }
        line ~ /apt(-get)?[[:space:]]+install/ { inblk = 1 }
        inblk {
            n = split(line, w, /[[:space:]]+/)
            for (i = 1; i <= n; i++) if (w[i] ~ /^[a-z0-9][a-z0-9.+-]*-dev$/) print w[i]
            if (line !~ /\\[[:space:]]*$/) inblk = 0
        }
    ' "$1" | sort -u
}

install_deps_devs() { apt_dev_packages magic_dingus_box_cpp/scripts/install_deps.sh; }
release_devs()      { apt_dev_packages .github/workflows/release.yml; }
kiosk_build_devs()  { apt_dev_packages .github/workflows/test-local.yml '^  kiosk-build:' '^  [^[:space:]#]'; }
pisim_devs()        { apt_dev_packages magic_dingus_box_cpp/dev/pisim/Dockerfile; }

# The file a *_devs function reads, for failure messages.
src_of() {
    case "$1" in
        install_deps_devs) echo "install_deps.sh" ;;
        release_devs)      echo "release.yml" ;;
        kiosk_build_devs)  echo "test-local.yml kiosk-build" ;;
        pisim_devs)        echo "dev/pisim/Dockerfile" ;;
    esac
}

# The apt packages MODULE_MAP names for the modules CMakeLists.txt asks for.
required_devs() {
    local m p
    for m in $(cmake_modules); do
        p="$(map_field "$m" 2)"
        [ -n "$p" ] && [ "$p" != "-" ] && echo "$p"
    done | sort -u
}

# A newline list on one line, for messages.
joined() { printf '%s' "$1" | tr '\n' ' '; }

# Lines in $1 but not $2 (both newline lists), minus allowlisted packages.
unallowed_only_in() {
    local p
    for p in $(comm -23 <(printf '%s\n' "$1") <(printf '%s\n' "$2")); do
        allowlisted "$p" || echo "$p"
    done
}

# Counts, not names: a real drop of one package must fail the parity tests
# below with their message, not this one.
@test "parsers see the dependency lists (guards against a vacuous pass)" {
    n="$(cmake_modules | grep -c .)"
    [ "$n" -ge 10 ] || { echo "CMakeLists.txt parser found only $n modules:"; cmake_modules; false; }
    for f in install_deps_devs release_devs kiosk_build_devs pisim_devs; do
        n="$($f | grep -c .)"
        [ "$n" -ge 10 ] || { echo "apt parser found only $n -dev packages in $(src_of "$f"):"; $f; false; }
    done
}

@test "every pkg_check_modules call in CMakeLists.txt is on one line" {
    run cmake_modules
    if echo "$output" | grep -q '^!UNPARSEABLE'; then
        echo "CMakeLists.txt line(s) this suite cannot read — keep each pkg_check_modules(...) on one line:"
        echo "$output" | grep '^!UNPARSEABLE'
        false
    fi
}

@test "every CMake dependency has a MODULE_MAP row" {
    missing=""
    for m in $(cmake_modules); do
        [[ "$m" == "!UNPARSEABLE" || "$m" =~ ^[0-9]+$ ]] && continue
        [ -n "$(map_field "$m" 1)" ] || missing+=" $m"
    done
    [ -z "$missing" ] || {
        echo "CMakeLists.txt asks for:$missing"
        echo "Add a MODULE_MAP row naming its Debian Trixie -dev package, then add that package to"
        echo "install_deps.sh, release.yml, test-local.yml kiosk-build, dev/pisim/Dockerfile and the README."
        false
    }
}

@test "every MODULE_MAP row is still a CMake dependency" {
    mods="$(cmake_modules)"
    stale=""
    for m in $(map_rows | awk '{ print $1 }'); do
        echo "$mods" | grep -qxF "$m" || stale+=" $m"
    done
    [ -z "$stale" ] || { echo "MODULE_MAP rows no longer in CMakeLists.txt:$stale (remove them, and their packages)"; false; }
}

@test "install_deps.sh and release.yml install the same -dev packages" {
    a="$(install_deps_devs)"; b="$(release_devs)"
    only_pi="$(unallowed_only_in "$a" "$b")"
    only_ci="$(unallowed_only_in "$b" "$a")"
    [ -z "$only_pi$only_ci" ] || {
        echo "On-Pi builds and the CI release binary would build against different libraries."
        [ -z "$only_pi" ] || echo "only in install_deps.sh: $(joined "$only_pi")"
        [ -z "$only_ci" ] || echo "only in release.yml:     $(joined "$only_ci")"
        false
    }
}

@test "test-local.yml kiosk-build and dev/pisim install release.yml's -dev packages" {
    ref="$(release_devs)"
    for f in kiosk_build_devs pisim_devs; do
        got="$($f)"
        extra="$(unallowed_only_in "$got" "$ref")"
        lacking="$(unallowed_only_in "$ref" "$got")"
        [ -z "$extra$lacking" ] || {
            echo "$(src_of "$f") differs from release.yml:"
            [ -z "$lacking" ] || echo "  lacks: $(joined "$lacking")"
            [ -z "$extra" ] || echo "  extra: $(joined "$extra")"
            false
        }
    done
}

@test "every CMake dependency's -dev package is installed by install_deps.sh and release.yml" {
    req="$(required_devs)"
    for f in install_deps_devs release_devs; do
        lacking="$(comm -23 <(echo "$req") <($f))"
        [ -z "$lacking" ] || { echo "$(src_of "$f") lacks: $(joined "$lacking") (CMakeLists.txt needs them)"; false; }
    done
}

@test "no -dev package is installed without a CMake consumer" {
    req="$(required_devs)"
    for f in install_deps_devs release_devs; do
        dead="$(unallowed_only_in "$($f)" "$req")"
        [ -z "$dead" ] || {
            echo "$(src_of "$f") installs $(joined "$dead") but nothing in CMakeLists.txt asks for it."
            echo "Remove it, or map its module in MODULE_MAP / justify it in DRIFT_ALLOWLIST."
            false
        }
    done
}

@test "deploy_cpp.sh PKG_MAP agrees with MODULE_MAP" {
    pairs="$(awk '/declare -A PKG_MAP=\(/ { on = 1; next } on && /^\)/ { exit } on' \
        magic_dingus_box_cpp/scripts/deploy_cpp.sh \
        | sed -nE 's/^[[:space:]]*\["([^"]+)"\]="([^"]+)".*/\1 \2/p')"
    [ -n "$pairs" ] || { echo "PKG_MAP not found in deploy_cpp.sh"; false; }
    bad=""
    while read -r mod pkg; do
        want="$(map_field "$mod" 2)"
        [ "$pkg" = "$want" ] || bad+="  $mod -> $pkg (MODULE_MAP says '${want:-<no row>}')"$'\n'
    done <<< "$pairs"
    [ -z "$bad" ] || { echo "deploy_cpp.sh PKG_MAP would install a different package:"; printf '%s' "$bad"; false; }
}

@test "README dependency list names every CMake dependency" {
    # shellcheck disable=SC2016  # literal Markdown backticks, not a command substitution
    names="$(awk '/Required system packages/ { on = 1; next } on && /^Plus header-only deps/ { exit } on' README.md \
        | grep -oE '`[^`]+`' | tr -d '`')"
    [ -n "$names" ] || { echo "README.md dependency list not found"; false; }
    missing=""
    for m in $(cmake_modules); do
        r="$(map_field "$m" 3)"
        [ -z "$r" ] || [ "$r" = "-" ] && continue
        echo "$names" | grep -qxF "$r" || missing+=" $r"
    done
    [ -z "$missing" ] || { echo "README.md 'Build & dependencies' list lacks:$missing"; false; }
}
