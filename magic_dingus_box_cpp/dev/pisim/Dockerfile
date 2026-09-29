# Pi-userspace build/test image: Debian Trixie, linux/arm64.
#
# On an Apple Silicon Mac this runs NATIVELY (no QEMU instruction
# emulation) — same distro, glibc, compiler and library versions the
# boxes run, at Mac-core speed. It builds the REAL kiosk binary
# (BUILD_KIOSK=ON: DRM/GBM/EGL/evdev/libgpiod/GStreamer/sd_notify), which
# the Mac build skips entirely, and that binary is deployable to either
# board as-is — it is the same recipe .github/workflows/release.yml uses
# for the OTA binary, just without the QEMU tax.
#
# Keep the kiosk apt list in sync with release.yml + install_deps.sh
# (CLAUDE.md dual-board rule 4). Driven by ../pisim.sh — see
# dev/pisim/README.md.
FROM debian:trixie

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config git file ca-certificates \
      rsync openssh-client \
      libdrm-dev libgbm-dev libegl-dev libgles2-mesa-dev \
      libevdev-dev libgpiod-dev libsystemd-dev \
      libyaml-cpp-dev libjsoncpp-dev libcurl4-openssl-dev \
      libtorrent-rasterbar-dev libsqlite3-dev libboost-dev \
      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
      gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-gl \
      bats shellcheck yamllint jq \
      python3 python3-yaml python3-jsonschema python3-pytest python3-flask \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /repo
