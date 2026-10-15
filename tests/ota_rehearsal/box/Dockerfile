# A Magic Dingus Box userspace for the OTA rehearsal: Debian Trixie arm64
# (the OS floor on both boards), the package set the OLD release's
# install_deps.sh puts on a box (copied in by run.sh as
# .install_deps.old.sh and parsed here, so the image follows that script
# rather than a hand-copied list), the `magic` user with passwordless sudo
# like Raspberry Pi OS, and flask-sock from pip exactly as install_deps.sh
# does. python3-gunicorn is deliberately ABSENT: fielded boxes do not have it
# (install_deps.sh only gained it in v1.10.0 and an OTA never re-runs it).
#
# Not a Raspberry Pi OS image: no archive.raspberrypi.com overlay, no
# systemd as PID 1, no /dev/dri, no /proc/device-tree. See scenario.sh for
# how each of those is stood in for.
FROM debian:trixie

ENV DEBIAN_FRONTEND=noninteractive
COPY .install_deps.old.sh /tmp/install_deps.old.sh

# Every `apt install -y \` block in the old install_deps.sh (base + Media
# Browser), minus python3-gunicorn should a future OLD_REF carry it.
RUN awk '/apt(-get)? install -y/ {grab=1; next} \
         grab { line=$0; cont=sub(/\\[[:space:]]*$/, "", line); \
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", line); \
                if (line != "") print line; if (!cont) grab=0 }' /tmp/install_deps.old.sh \
      | tr ' ' '\n' | grep -v '^$' | grep -vx 'python3-gunicorn' > /tmp/box-packages.txt \
 && echo "box packages from install_deps.sh:" && tr '\n' ' ' < /tmp/box-packages.txt && echo \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      $(cat /tmp/box-packages.txt) \
      sudo curl rsync file ca-certificates openssl git procps util-linux jq iproute2 \
      python3 python3-flask python3-yaml python3-pip \
 && pip3 install --break-system-packages 'flask-sock>=0.7.0' \
 && rm -rf /var/lib/apt/lists/*

# The kiosk's FetchContent spdlog, mirrored at image-build time so an
# on-box SOURCE build (update.sh's compile fallback / build.new path) works
# while github.com inside the container resolves to the fake GitHub.
RUN git clone --mirror --quiet https://github.com/gabime/spdlog.git /opt/mirrors/spdlog.git \
 && git config --system url."file:///opt/mirrors/spdlog.git".insteadOf https://github.com/gabime/spdlog.git \
 && git config --system --add safe.directory /opt/mirrors/spdlog.git

# The box user. uid 1000 + passwordless sudo, as on Raspberry Pi OS.
# env_keep for MAGIC_CF_PROBE_ATTEMPTS is a rehearsal-only deviation: it lets
# converge_custom_formats.sh (run via `sudo -n`) give up on the absent
# Radarr/Sonarr after 2 probes instead of 30 (2 x 60 s per install).
RUN for g in sudo video input audio; do getent group "$g" >/dev/null || groupadd -r "$g"; done \
 && useradd -m -u 1000 -s /bin/bash -G sudo,video,input,audio magic \
 && echo 'magic ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/010_magic-nopasswd \
 && echo 'Defaults env_keep += "MAGIC_CF_PROBE_ATTEMPTS"' > /etc/sudoers.d/020_ota_rehearsal \
 && chmod 0440 /etc/sudoers.d/*
