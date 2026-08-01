# Phase 2a — Sonarr Service Stack Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up a fully-configured Sonarr 4.x container inside the existing Gluetun VPN network namespace, reconciled idempotently by `setup_services.sh`, with Prowlarr syncing the TV-capable indexer subset — provable by adding one series via Sonarr's API and watching the grab hit the queue (Task 7; the web UI over the SSH tunnel is the human-optional alternative). No kiosk feature is visible yet.

**Architecture:** Sonarr copies Radarr's netns-dependent compose pattern (`network_mode: "service:gluetun"`, `depends_on gluetun service_healthy`, `restart: always`, `/ping` healthcheck) and is exposed only through Gluetun's `ports:` block on `127.0.0.1:8989`. Every configuration surface (quality profile, custom formats, quality definitions, download client, qBit category, root folder, Prowlarr app-sync) is a Sonarr twin of the corresponding Radarr reconciler block in `setup_services.sh`, driven by new `scripts/data/sonarr_*.json` fixtures. Sonarr's quality-id space differs from Radarr's, so its quality-definition fixture is captured live once, then enforced idempotently. Five host-side scripts + two Python test files + one C++ comment that hardcode the container roster gain `mdb_sonarr` / `sonarr`.

**Tech Stack:** docker-compose, bash + embedded python3 reconcilers (urllib, sqlite3), Sonarr v4 REST API (`/api/v3`, `X-Api-Key`), Prowlarr `/api/v1`, qBittorrent WebUI API, systemd, C++ (comment-only edit).

## Global Constraints

Every task's requirements implicitly include this section. Values are copied verbatim from the controller decisions and CLAUDE.md.

- **Sonarr image pin:** `lscr.io/linuxserver/sonarr:4.0.19.2979-ls320` (full `<version>-lsNN` tag; current stable per the 2026-07 recon — Task 1 Step 5 pre-pulls it on the box as a hard existence gate before any `up -d`, since a bad tag mid-`up -d` would strand the whole cascaded stack). API served under `/api/v3` with `X-Api-Key` auth; port `8989`; container name `mdb_sonarr`; compose service name `sonarr`. Note this is a deliberate NEW pin style for the compose file — radarr/prowlarr/qbittorrent stay on bare upstream tags.
- **Dual-board contract:** board differences resolve at RUNTIME via `/proc/device-tree/model`, never `#ifdef`/hardcode. The Sonarr Pi-4 quality-definition override keys off the model string exactly like Radarr's.
- **ENABLE_MEDIA_BROWSER bit-identical invariant:** the only C++ edit (`vpn_health_monitor.cpp`) is comments-only, so the compiled binary is byte-identical with `ENABLE_MEDIA_BROWSER=ON` and `=OFF`. Verify both configure/compile in an isolated Pi scratch build.
- **Quoted paths:** the repo path contains spaces + emoji (`/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box `, with a trailing space in `magic_dingus_box `). ALWAYS quote paths in every command.
- **Commit style:** `feat(services): …` for new capability, `fix(services): …` for corrections, each ending with the footer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. Task 1 Step 0 creates the branch `feat/sonarr-stack`; NEVER commit to `main`. Every git command in this plan is written `git -C "${REPO}" …` so it works from any cwd. The worktree currently carries unrelated uncommitted Phase-1 changes (`browse_screen.cpp/.h`) — leave them unstaged; every commit block here adds explicit paths only.
- **Mac test suite invariant:** the C++ Mac suites must stay at **167 cases / 5039 assertions**. This plan adds NO Mac-testable C++ (the only C++ change is a comment), so that count must not move. The Python test edits in Task 6 (`test_gluetun_cascade.py`, `test_services_pause_marker.py`) are Pi/host-side tests, separate from the Mac count.
- **Live-box execution:** all steps run against the live box `magic@magicpi5.local` (fall back to `magic@magicpi.local` if that is the box's name). Each task ends with a verify step and is individually reversible.
- **Never `deploy_cpp.sh` mid-plan:** it restarts the kiosk the family may be using. Ship changed `scripts/`, `services/`, and `scripts/data/` files to the box with targeted `rsync` (kiosk untouched) and re-run `sudo setup_services.sh`. The C++ edit compiles via the isolated Pi scratch build only; its (byte-identical) deploy is the final, explicitly-flagged step.

## Conventions used in this plan

- `REPO` = `"/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box "` (trailing space real; always quoted).
- `APP` = `"${REPO}/magic_dingus_box_cpp"`. Scripts live in `"${APP}/scripts"`, fixtures in `"${APP}/scripts/data"`, compose in `"${APP}/services"`.
- `PI` = `magic@magicpi5.local`. On-box service dir is `/opt/magic_dingus_box/services`; on-box scripts run from `/opt/magic_dingus_box/magic_dingus_box_cpp/scripts` (and `/usr/local/bin` copies for the pause/cascade helpers).
- "Ship to box" for a file `F` under the repo means: `rsync -a --checksum "F" "${PI}:/opt/magic_dingus_box/<same-relative-path>"`. This never restarts the kiosk.
- **Line numbers are pre-plan snapshots** against the current 2213-line `setup_services.sh`. Tasks 2-7 mutate that file, so by Task 4 every number below an insertion point is stale by 200+ lines. Locate EVERY insertion point by the quoted content anchor (the exact `fi`/comment/code text each step quotes), never by the stated line number.
- **The box `.env` is root-owned mode 600** (`setup_services.sh` creates it with `chmod 600` as root). ALWAYS read it with `sudo grep` in on-box snippets — a non-sudo `grep` silently reads nothing and is indistinguishable from "key absent".
- **Playback-pause gate:** before ANY command that runs `setup_services.sh` or `docker compose up -d` on the box, run
  `ssh "${PI}" 'test ! -f /tmp/mdb_playback_services_paused && echo SAFE || echo BUSY-DEFER'`
  and proceed only on `SAFE` (if `BUSY-DEFER`, wait for playback to end and re-check). `setup_services.sh` has no marker awareness of its own — its `docker compose up -d` would restart the paused containers mid-game and its Step 7.6 restarts the kiosk.

## Deliberately deferred from 2a (visible scope call — later plans)

- `TmdbClient` TV endpoints, `SonarrClient`, all kiosk UI (Phase 2b+ per the spec).
- **Step 14c-S** (direct-DB `sonarr.db` indexer-sync fallback): sync-skipped indexers stay absent from Sonarr for now. Task 7's `check_sonarr_indexers` uses a ≥2 threshold with NO DB-fallback safety net behind it (Radarr's equivalent check is backstopped by 14c). If the Prowlarr app-sync silently skips a TV indexer the way Radarr's sync skipped LimeTorrents, the follow-up plan adds the `sonarr.db` fallback — capture Sonarr's live `Indexers` table schema first; do not copy Radarr's blindly. (The cheap **14b-S min-seeders twin IS included** — Task 5 Step 5.)
- Sonarr twins of the Radarr-only systemd helpers: clear-cooldowns (`sonarr.db` IndexerStatus), auto-blocklist (Sonarr queue warnings), `MissingEpisodeSearch` timer. Their absence recreates known Radarr-era incident classes on the TV side.
- **Revisit trigger for the two bullets above: the first observed Sonarr sync failure or TV junk grab.** When either happens, open the follow-up plan instead of hand-patching the box.
- OTA `update.sh` hook to re-run setup on Sonarr-less provisioned boxes (this plan's vehicle is a manual SSH run on magicpi5; fleet rollout rides the next OTA plan).
- Kiosk Confirm-Remove / auto-blocklist walking Sonarr history (TV torrent cleanup).

---

### Task 1: Sonarr compose service + Gluetun port 8989 + host TV dir

Brings the container up in Gluetun's netns and makes `127.0.0.1:8989` reachable from the host. No API config yet — this task proves the container is healthy and pingable.

**Files:**
- Modify: `magic_dingus_box_cpp/services/docker-compose.yml` (add `sonarr:` service block after the `radarr:` block ending at line 71; add `"127.0.0.1:8989:8989"` to gluetun's `ports:` list at lines 205-207)
- Modify: `magic_dingus_box_cpp/scripts/setup_services.sh:173` (extend the storage mkdir to create `library/tv`)

**Interfaces:**
- Produces: container `mdb_sonarr`, compose service `sonarr`, host-reachable `http://127.0.0.1:8989/ping`, host dir `${STORAGE_ROOT}/library/tv` mounted at `/data/library/tv` in the container. Later tasks consume `8989`, `mdb_sonarr`, `sonarr`, and `/data/library/tv`.

- [ ] **Step 0: Create the feature branch**

```bash
git -C "${REPO}" checkout -b feat/sonarr-stack
```

(The worktree carries pre-existing dirty `browse_screen.cpp/.h` files — they belong to the parallel Phase-1 work; leave them unstaged. Every commit in this plan `git add`s explicit paths only.)

- [ ] **Step 1: Add the `sonarr` service block to `docker-compose.yml`**

Insert this block immediately after the `radarr:` service ends (anchor: radarr's healthcheck closing line `      start_interval: 3s`, currently line 71, before `  prowlarr:`). It copies the Radarr template's structure exactly (netns, `depends_on: service_healthy`, `restart: always`, identical healthcheck timings) with two deliberate differences:

- **Pin style:** the image uses the full linuxserver `<version>-lsNN` tag — a NEW pin style for this compose file (radarr `5.14.0` / prowlarr `2.3.5` / qbittorrent `5.0.3` stay on bare upstream tags). The `-lsNN` suffix bumps on linuxserver baseimage rebuilds without an app-version change, so the full tag is the only truly reproducible pin; the other three images are left as-is (out of scope). Because a nonexistent tag would fail mid-`up -d` — AFTER the gluetun recreate has already cascaded every dependent — Step 5 pre-pulls the image as a hard gate.
- **Volumes:** drops Radarr's legacy `${STORAGE_ROOT}/library:/library` mount (Sonarr is greenfield — root folder `/data/library/tv` from day one, no legacy-path DB debt), keeping `${STORAGE_ROOT}/downloads:/downloads` (so Sonarr can import on boxes whose qBit still reports `/downloads/...` save paths) and the `${STORAGE_ROOT}:/data` parent mount (hardlink-capable imports on migrated boxes).

```yaml
  sonarr:
    image: lscr.io/linuxserver/sonarr:4.0.19.2979-ls320
    container_name: mdb_sonarr
    # Behind Gluetun: shares VPN netns beside Radarr. Sonarr's TMDB/TVDB
    # metadata fetches exit via the VPN like the rest of the stack.
    # No per-service ports: block — Sonarr's :8989 is exposed via
    # gluetun's ports: section (see below), same as Radarr/Prowlarr.
    network_mode: "service:gluetun"
    depends_on:
      gluetun:
        condition: service_healthy
    environment:
      - PUID=${PUID}
      - PGID=${PGID}
      - TZ=${TZ}
    volumes:
      - ./config/sonarr:/config
      # Legacy-compat mount: on a box that has NOT run
      # migrate_hardlink_layout.sh, qBit still reports /downloads/... save
      # paths, and Sonarr needs this mount to see completed torrents at
      # the path qBit names. Imports via this mount COPY, not hardlink
      # (link(2) across two bind mounts returns EXDEV even on one
      # filesystem — see the radarr comments above).
      - ${STORAGE_ROOT}/downloads:/downloads
      # Parent mount matching Radarr's /data. Sonarr's root folder is
      # /data/library/tv. Whether an import HARDLINKS or COPIES depends on
      # which path qBit reports at import time: on a MIGRATED box (qBit
      # save paths under /data/downloads) source and library share this
      # one mount and imports hardlink instantly; on an unmigrated box the
      # import crosses the two mounts above and copies. setup_services.sh
      # mirrors the live qBit 'radarr' category savePath when creating the
      # 'sonarr' category, so both worlds behave correctly (see
      # scripts/data/qbit_categories.json and the plan's Task 4 capture).
      - ${STORAGE_ROOT}:/data
    # `always` (not `unless-stopped`) — Sonarr, like Radarr, can exit 0 on
    # internal config reloads. `unless-stopped` would read that as
    # "user stopped it" and leave it down indefinitely.
    restart: always
    healthcheck:
      test: ["CMD", "curl", "-fsS", "http://localhost:8989/ping"]
      interval: 10s
      timeout: 5s
      retries: 3
      start_period: 20s
      start_interval: 3s
```

- [ ] **Step 2: Add port 8989 to Gluetun's `ports:` list**

In the gluetun service's `ports:` block, after the `"127.0.0.1:8191:8191"   # Byparr (Cloudflare bypass)` line (line 207), add:

```yaml
      - "127.0.0.1:8989:8989"   # Sonarr
```

- [ ] **Step 3: Extend the storage mkdir for the TV library**

In `setup_services.sh`, change line 173 from:

```bash
sudo mkdir -p "${STORAGE_ROOT}"/{downloads/incomplete,downloads/complete,library,backups}
```

to:

```bash
sudo mkdir -p "${STORAGE_ROOT}"/{downloads/incomplete,downloads/complete,library,library/tv,backups}
```

(The `chown -R "${TARGET_USER}"` on the next line already covers the new dir, so Sonarr's PUID owns it and the root-folder POST in Task 4 succeeds.)

- [ ] **Step 4: Ship the two changed files to the box (no kiosk restart)**

Run:

```bash
rsync -a --checksum "${APP}/services/docker-compose.yml" "${PI}:/opt/magic_dingus_box/services/docker-compose.yml"
rsync -a --checksum "${APP}/scripts/setup_services.sh"   "${PI}:/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_services.sh"
```

- [ ] **Step 5: Create the host TV dir and bring the stack up on the box**

> WARNING: adding a port to Gluetun's `ports:` block forces a Gluetun container RECREATE on `up -d`, which cascades a full restart of every netns dependent (Radarr/Prowlarr/qBit/Byparr). Run this ONLY when no game or movie is playing (no `/tmp/mdb_playback_services_paused` marker). Verify first:

```bash
ssh "${PI}" 'test ! -f /tmp/mdb_playback_services_paused && echo SAFE || echo BUSY-DEFER'
```
Expected: `SAFE`

Then pre-pull the pinned image as a hard gate — a bad/nonexistent `-lsNN` tag must fail HERE, not mid-`up -d` after the gluetun recreate has already cascaded every dependent:

```bash
ssh "${PI}" 'docker pull lscr.io/linuxserver/sonarr:4.0.19.2979-ls320'
```
Expected: pull completes (`Status: Downloaded newer image …` or `Image is up to date …`). If it errors (`manifest unknown`), STOP — check https://github.com/linuxserver/docker-sonarr/releases for the current stable `<version>-lsNN` tag, update the compose pin and the Global Constraints line, and re-run this gate. Do NOT proceed to `up -d` with an unverified tag.

Then:

```bash
ssh "${PI}" 'sudo mkdir -p /mnt/ssd/library/tv && sudo chown magic:magic /mnt/ssd/library/tv && cd /opt/magic_dingus_box/services && docker compose up -d --remove-orphans'
```
Expected: `mdb_sonarr` reported `Started` (or `Created`+`Started`); gluetun + the four existing dependents recreated/restarted cleanly.

- [ ] **Step 6: Verify Sonarr is healthy and pingable**

```bash
ssh "${PI}" 'docker inspect -f "{{.State.Health.Status}}" mdb_sonarr; curl -fsS http://127.0.0.1:8989/ping'
```
Expected: `healthy` on the first line, and a `{"status":"OK"}` JSON body from `/ping` on the second (may take up to ~40s after start; re-run if the first attempt shows `starting`).

- [ ] **Step 7: Commit**

```bash
git -C "${REPO}" add "magic_dingus_box_cpp/services/docker-compose.yml" "magic_dingus_box_cpp/scripts/setup_services.sh"
git -C "${REPO}" commit -m "feat(services): add Sonarr container to the Gluetun netns stack

Copies the Radarr netns pattern; exposes 8989 via gluetun's ports block;
creates the /mnt/ssd/library/tv root-folder dir. No API config yet.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

**Reversal:** `git -C "${REPO}" checkout` both files (removes the `sonarr:` block, the `8989` port line, and the mkdir edit), **re-run this task's Step 4 rsync ship against the reverted files**, then — after passing the same SAFE/BUSY-DEFER marker check as Step 5 (un-mapping 8989 recreates gluetun and cascades all dependents) — `ssh "${PI}" 'cd /opt/magic_dingus_box/services && docker compose up -d --remove-orphans'` (removes `mdb_sonarr` as an orphan), and `ssh "${PI}" 'sudo rm -rf /opt/magic_dingus_box/services/config/sonarr'`.

---

### Task 2: SONARR_API_KEY .env plumbing (scaffold + append-if-missing + extraction)

Makes Sonarr's auto-generated API key available to every later reconciler and to the kiosk (via the existing `magic-dingus-box-cpp.service` `EnvironmentFile=`). Must survive on ALREADY-PROVISIONED boxes whose `.env` predates this line — hence the append-if-missing path (the Step-7 `sed` no-ops when the key line is absent).

**Files:**
- Modify: `magic_dingus_box_cpp/services/.env.example` (document the new var)
- Modify: `magic_dingus_box_cpp/scripts/setup_services.sh` — Step 3 scaffold (after line 190), Step 6 extraction (after line 632) + guard (line 634), Step 7 write-back (after line 642), credentials heredoc (lines 2187-2204)

**Interfaces:**
- Consumes: `mdb_sonarr` config at `${SERVICES_DIR}/config/sonarr/config.xml` (produced by Task 1's first container start).
- Produces: shell var `SONARR_KEY` and `.env` line `SONARR_API_KEY=<32 hex>`, both available to Tasks 3-7's reconcilers and to `verify_services.sh`.

> Line numbers below are pre-plan snapshots — locate every edit by the quoted text anchor, never by the number (see Conventions).

- [ ] **Step 1: Document the var in `.env.example`**

In `services/.env.example`, after the `PROWLARR_API_KEY=__REPLACE_ME__` line, add:

```
SONARR_API_KEY=__REPLACE_ME__
```

- [ ] **Step 2: Add the scaffold line to Step 3's heredoc**

In `setup_services.sh`, inside the `.env` generation heredoc (Step 3), after line 190 (`PROWLARR_API_KEY=__WILL_BE_SET_AFTER_FIRST_START__`), add:

```
SONARR_API_KEY=__WILL_BE_SET_AFTER_FIRST_START__
```

- [ ] **Step 3: Extract Sonarr's key in Step 6 and guard on it**

After line 632 (the `PROWLARR_KEY=$(grep …)` line), add:

```bash
SONARR_KEY=$(grep -oP '(?<=<ApiKey>)[^<]+' "${SERVICES_DIR}/config/sonarr/config.xml" 2>/dev/null || echo "")
```

Then change the guard at line 634 from:

```bash
if [ -z "${RADARR_KEY}" ] || [ -z "${PROWLARR_KEY}" ]; then
```

to:

```bash
if [ -z "${RADARR_KEY}" ] || [ -z "${PROWLARR_KEY}" ] || [ -z "${SONARR_KEY}" ]; then
```

(The existing warning "Re-run this script in a minute…" and `exit 1` recovery already cover a still-initializing third `*arr` app.)

- [ ] **Step 4: Write the key back to `.env` with the append-if-missing pattern**

After line 642 (the `sed -i "s|PROWLARR_API_KEY=.*|…"` line), add — do NOT use a bare `sed` replace, which silently no-ops on provisioned boxes whose `.env` has no `SONARR_API_KEY=` line (mirror the `MDB_QBIT_PASS` grep-then-append pattern at lines 736-741):

```bash
# Sonarr key: append-if-missing. On boxes provisioned before Sonarr
# existed, the .env has no SONARR_API_KEY= line, so a plain `sed`
# replace above would silently no-op and the key would never land.
if grep -q '^SONARR_API_KEY=' "${ENV_FILE}"; then
    sed -i "s|^SONARR_API_KEY=.*|SONARR_API_KEY=${SONARR_KEY}|" "${ENV_FILE}"
else
    echo "SONARR_API_KEY=${SONARR_KEY}" >> "${ENV_FILE}"
fi
```

- [ ] **Step 5: Extend the operator credentials block**

In the closing credentials heredoc, add `8989` to the SSH tunnel forward. Change lines 2187-2191 from:

```bash
  ssh -L 7878:localhost:7878 \\
      -L 9696:localhost:9696 \\
      -L 8080:localhost:8080 \\
      -L 8191:localhost:8191 \\
      magic@magicpi.local
```

to:

```bash
  ssh -L 7878:localhost:7878 \\
      -L 9696:localhost:9696 \\
      -L 8989:localhost:8989 \\
      -L 8080:localhost:8080 \\
      -L 8191:localhost:8191 \\
      magic@magicpi.local
```

And after the `Radarr → …` credentials block (ending at line 2196 with the Radarr `API key: ${RADARR_KEY}`), add:

```bash
Sonarr    → http://localhost:8989 (via SSH tunnel only — see operator guide)
            API key: ${SONARR_KEY}

```

- [ ] **Step 6: Ship the changed files and re-run just the key-plumbing portion**

```bash
rsync -a --checksum "${APP}/scripts/setup_services.sh" "${PI}:/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_services.sh"
rsync -a --checksum "${APP}/services/.env.example"     "${PI}:/opt/magic_dingus_box/services/.env.example"
```

The full script re-runs at the end of the plan (Task 7). To prove the key plumbing NOW without a full run, extract + append on the box directly using the same commands the script uses:

The `.env` is root-owned mode 600, so ALL reads use `sudo` — a non-sudo `grep` on it silently reads nothing, which under `set -e` would masquerade as "key absent" and duplicate the line:

```bash
ssh "${PI}" 'set -e
  SONARR_KEY=$(grep -oP "(?<=<ApiKey>)[^<]+" /opt/magic_dingus_box/services/config/sonarr/config.xml)
  ENV=/opt/magic_dingus_box/services/.env
  if sudo grep -q "^SONARR_API_KEY=" "$ENV"; then sudo sed -i "s|^SONARR_API_KEY=.*|SONARR_API_KEY=${SONARR_KEY}|" "$ENV";
  else echo "SONARR_API_KEY=${SONARR_KEY}" | sudo tee -a "$ENV" >/dev/null; fi
  sudo grep "^SONARR_API_KEY=" "$ENV"'
```

(`config/sonarr/config.xml` is PUID-owned — `magic` — so its read needs no sudo.)

- [ ] **Step 7: Verify the key landed and authenticates**

```bash
ssh "${PI}" 'K=$(sudo grep "^SONARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  echo "key len: ${#K}";
  curl -fsS -H "X-Api-Key: ${K}" http://localhost:8989/api/v3/system/status | python3 -c "import sys,json; d=json.load(sys.stdin); print(\"appName:\", d.get(\"appName\"), \"version:\", d.get(\"version\"))"'
```
Expected: `key len: 32`, then `appName: Sonarr version: 4.0.19.2979` (or the pinned build).

- [ ] **Step 8: Commit**

```bash
git -C "${REPO}" add "magic_dingus_box_cpp/scripts/setup_services.sh" "magic_dingus_box_cpp/services/.env.example"
git -C "${REPO}" commit -m "feat(services): plumb SONARR_API_KEY through .env (scaffold + append-if-missing)

Extract Sonarr's config.xml key post-start; write back with the
grep-then-append pattern so provisioned boxes don't lose it on re-run.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

**Reversal:** `git -C "${REPO}" checkout` both files, **re-run this task's Step 6 rsync ship against the reverted files**, then remove the `SONARR_API_KEY=` line from the box `.env` (harmless if left — it is just unused).

---

### Task 3: Sonarr quality profile + Custom Formats reconciler

Codifies the "Any" quality profile restricted to 720p/1080p and the Custom-Format score map (mirrors the movie SCORE_MAP, minus the legacy `Trusted small-release groups` entry which no Sonarr box has ever had). All CF specs are `ReleaseTitleSpecification` title regexes, so the fixture is byte-identical to Radarr's.

**Files:**
- Create: `magic_dingus_box_cpp/scripts/data/sonarr_custom_formats.json` (copy of `radarr_custom_formats.json`)
- Modify: `magic_dingus_box_cpp/scripts/setup_services.sh` (insert "Step 10-S" after the Radarr Custom-Formats block ends at line 1092)

**Interfaces:**
- Consumes: `SONARR_KEY` (Task 2).
- Produces: a Sonarr "Any" profile with `minFormatScore=-200`, cutoff `Bluray-720p`, only the eight 720p/1080p H.264 leaf qualities allowed, and 10 named Custom Formats scored per the map. Task 7's `check_sonarr_quality_profile` / `check_sonarr_custom_formats` assert this. 10-S also carries the plan's ONE Sonarr readiness probe (it is Sonarr's first hard-fail API contact in the script's step order — every later Sonarr block runs after it).

> Line numbers below are pre-plan snapshots — locate every edit by the quoted text anchor, never by the number (see Conventions).

- [ ] **Step 1: Create the Sonarr Custom-Formats fixture**

```bash
cp "${APP}/scripts/data/radarr_custom_formats.json" "${APP}/scripts/data/sonarr_custom_formats.json"
```

Verify it is the 10-format list (all title-regex specs, portable to Sonarr):

```bash
jq -r '.[].name' "${APP}/scripts/data/sonarr_custom_formats.json"
```
Expected (exactly these 10, in order): `AV1 codec (UNWATCHABLE on Pi 4)`, `x265/HEVC 1080p+`, `HDR / Dolby Vision`, `Remux / Raw-HD`, `x264 codec (BONUS)`, `Quality release groups`, `Low-bitrate size-optimized groups`, `Malware/scam executable in title`, `Known scam aggregator branding`, `Non-English title signals`.

- [ ] **Step 2: Insert the Sonarr quality-profile + Custom-Formats reconciler**

In `setup_services.sh`, immediately after the Radarr Step 10 block's closing `fi` (line 1092), insert the block below. It is the Radarr Step 10 python with three deltas: `BASE` is `:8989`; `SCORE_MAP` omits the legacy `Trusted small-release groups` (greenfield); `ALLOWED_QUALITY_NAMES` lists LEAF names (Sonarr's default profile grouping is not asserted, so leaf-name matching is robust — a leaf is allowed when its own name is listed, and each group row reflects its leaves). It does NOT touch profile language (Sonarr v4 handles language outside the quality profile; out of scope for 2a).

````bash
# 10-S. Codify Sonarr Custom Formats + Any-profile score map.
#
# Sonarr twin of the Radarr block above. Same codec/quality policy: the
# kiosk decodes H.264 720p/1080p; AV1/HEVC-1080p+/HDR/Remux and non-English
# / scam releases are pushed below the -200 minFormatScore floor. All CF
# specs are ReleaseTitleSpecification title regexes (identical fixture to
# Radarr's), so they port verbatim. Two Sonarr-specific choices:
#   * SCORE_MAP omits the legacy "Trusted small-release groups" (0) entry —
#     that entry only exists to neutralize pre-2026-07-26 RADARR boxes;
#     Sonarr is greenfield and never had it.
#   * ALLOWED_QUALITY_NAMES lists LEAF quality names (HDTV-720p, WEBDL-720p,
#     …) rather than group names. Sonarr's default profile grouping isn't
#     assumed; a leaf is allowed when its own name matches, and group rows
#     reflect their leaves. No profile-language mutation (Sonarr v4 handles
#     language separately from the quality profile).
echo "Configuring Sonarr Custom Formats + 'Any' profile score map..."
SONARR_CF_DATA_FILE="${SCRIPT_DIR}/data/sonarr_custom_formats.json"
if [[ ! -f "${SONARR_CF_DATA_FILE}" ]]; then
    echo "  WARN: ${SONARR_CF_DATA_FILE} not found — skipping. Verify Sonarr Custom Formats via web UI."
else
    # Sonarr readiness probe. This is the script's FIRST hard-fail Sonarr
    # API contact (the python below makes ~10 unguarded urllib calls), so
    # the warm-up wait lives HERE, not later: a still-initializing Sonarr
    # either answers within this loop or every later Sonarr step was
    # doomed anyway. (Radarr's equivalent probe sits in Step 14 because
    # its earlier contacts at Steps 8-9 are ||-guarded.)
    for i in {1..30}; do
        if curl -fsS -o /dev/null -H "X-Api-Key: ${SONARR_KEY}" \
            http://localhost:8989/api/v3/system/status; then
            break
        fi
        sleep 2
    done
    SONARR_CF_SUMMARY=$(python3 - "${SONARR_CF_DATA_FILE}" "${SONARR_KEY}" <<'PYEOF'
import json, sys, urllib.request, urllib.error

cf_data_path, api_key = sys.argv[1], sys.argv[2]
BASE = "http://localhost:8989/api/v3"

SCORE_MAP = {
    "AV1 codec (UNWATCHABLE on Pi 4)": -1000,
    "x265/HEVC 1080p+":                -250,
    "HDR / Dolby Vision":              -200,
    "Remux / Raw-HD":                  -500,
    "x264 codec (BONUS)":               50,
    "Quality release groups":           30,
    "Low-bitrate size-optimized groups": -30,
    "Malware/scam executable in title": -10000,
    "Known scam aggregator branding":   -10000,
    "Non-English title signals":        -10000,
}
MIN_FORMAT_SCORE = -200

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def cf_specs_match(live, desired):
    if live.get("name") != desired.get("name"): return False
    if live.get("includeCustomFormatWhenRenaming") != desired.get("includeCustomFormatWhenRenaming"): return False
    ls, ds = live.get("specifications", []), desired.get("specifications", [])
    if len(ls) != len(ds): return False
    for a, b in zip(ls, ds):
        if a.get("name") != b.get("name"): return False
        if a.get("implementation") != b.get("implementation"): return False
        if bool(a.get("negate")) != bool(b.get("negate")): return False
        if bool(a.get("required")) != bool(b.get("required")): return False
        av = next((f.get("value") for f in a.get("fields", []) if f.get("name") == "value"), None)
        bv = next((f.get("value") for f in b.get("fields", []) if f.get("name") == "value"), None)
        if av != bv: return False
    return True

with open(cf_data_path) as f:
    desired_cfs = json.load(f)

live_cfs = http("GET", "/customformat")
live_by_name = {c["name"]: c for c in live_cfs}

created, updated, unchanged = [], [], []
name_to_id = {}

for desired in desired_cfs:
    name = desired["name"]
    payload = {k: v for k, v in desired.items() if k != "id"}
    if name in live_by_name:
        live = live_by_name[name]
        if cf_specs_match(live, payload):
            unchanged.append(name)
            name_to_id[name] = live["id"]
        else:
            payload["id"] = live["id"]
            result = http("PUT", "/customformat/%d" % live["id"], payload)
            updated.append(name)
            name_to_id[name] = result["id"]
    else:
        result = http("POST", "/customformat", payload)
        created.append(name)
        name_to_id[name] = result["id"]

profiles = http("GET", "/qualityprofile")
any_profile = next((p for p in profiles if p["name"] == "Any"), None)
profile_changed = False
score_changes = []

if any_profile is None:
    print("WARN: 'Any' profile missing; cannot apply score map", file=sys.stderr)
    print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged,
                      "profile_changed": False, "score_changes": []}))
    sys.exit(0)

if any_profile.get("minFormatScore") != MIN_FORMAT_SCORE:
    any_profile["minFormatScore"] = MIN_FORMAT_SCORE
    profile_changed = True
    score_changes.append("minFormatScore=%d" % MIN_FORMAT_SCORE)
for fi in any_profile.get("formatItems", []):
    fname = fi.get("name")
    if fname in SCORE_MAP:
        want = SCORE_MAP[fname]
        if fi.get("score") != want:
            fi["score"] = want
            profile_changed = True
            score_changes.append("%s=%+d" % (fname, want))

# Leaf-name allowed set: only the eight H.264 720p/1080p tiers.
ALLOWED_QUALITY_NAMES = {
    "HDTV-720p", "WEBDL-720p", "WEBRip-720p", "Bluray-720p",
    "HDTV-1080p", "WEBDL-1080p", "WEBRip-1080p", "Bluray-1080p",
}
DESIRED_CUTOFF_QUALITY_NAME = "Bluray-720p"

def find_quality_id(items, target_name):
    for item in items:
        if item.get("name") == target_name and item.get("id"):
            return item["id"]
        for sub in (item.get("items") or []):
            q = sub.get("quality") or {}
            if q.get("name") == target_name:
                return q.get("id")
        q = item.get("quality") or {}
        if q.get("name") == target_name:
            return q.get("id")
    return None

desired_cutoff = find_quality_id(any_profile.get("items", []), DESIRED_CUTOFF_QUALITY_NAME)
if desired_cutoff and any_profile.get("cutoff") != desired_cutoff:
    any_profile["cutoff"] = desired_cutoff
    profile_changed = True
    score_changes.append("cutoff=%s(id=%d)" % (DESIRED_CUTOFF_QUALITY_NAME, desired_cutoff))

def item_name(item):
    return item.get("name") or (item.get("quality") or {}).get("name")

def enforce_allowed(items, group_allowed=False):
    changed = False
    for item in items:
        nm = item_name(item)
        named = nm in ALLOWED_QUALITY_NAMES if nm else False
        want_allowed = named or group_allowed
        children = item.get("items") or []
        if children:
            if enforce_allowed(children, want_allowed):
                changed = True
            want_allowed = any(c.get("allowed") for c in children)
        if item.get("allowed") != want_allowed:
            item["allowed"] = want_allowed
            changed = True
            score_changes.append("%s.allowed=%s" % (nm, want_allowed))
    return changed

if enforce_allowed(any_profile.get("items", [])):
    profile_changed = True

if profile_changed:
    http("PUT", "/qualityprofile/%d" % any_profile["id"], any_profile)

print(json.dumps({
    "created": created,
    "updated": updated,
    "unchanged": unchanged,
    "profile_changed": profile_changed,
    "score_changes": score_changes,
}))
PYEOF
)
    printf '%s' "${SONARR_CF_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
if s["profile_changed"]:
    print("  ✓ Sonarr Any profile score map updated: " + ", ".join(s["score_changes"]))
else:
    print("  ✓ Sonarr Any profile score map already matches desired state")
'
fi
````

- [ ] **Step 3: Ship the fixture + script and run this block on the box**

```bash
rsync -a --checksum "${APP}/scripts/data/sonarr_custom_formats.json" "${PI}:/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/data/sonarr_custom_formats.json"
rsync -a --checksum "${APP}/scripts/setup_services.sh"               "${PI}:/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_services.sh"
```

The full run happens in Task 7; to exercise ONLY this block now, first confirm Sonarr ships an "Any" profile (the reconciler warns + no-ops if it does not):

```bash
ssh "${PI}" 'K=$(sudo grep "^SONARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  curl -fsS -H "X-Api-Key: ${K}" http://localhost:8989/api/v3/qualityprofile | python3 -c "import sys,json; print([p[\"name\"] for p in json.load(sys.stdin)])"'
```
Expected: a list that includes `Any` (Sonarr v4 seeds `Any`, `SD`, `HD-720p`, `HD-1080p`, `HD - 720p/1080p`, `Ultra-HD`). If `Any` is absent, STOP and report — the reconciler's target profile name would need revisiting.

Then re-run the script (idempotent; safe to run repeatedly) to apply just through this block, or run the full script in Task 7. For a scoped check now, run the full script — every prior block is idempotent. First pass the playback-pause gate (a setup run restarts paused containers and the kiosk — see Conventions):

```bash
ssh "${PI}" 'test ! -f /tmp/mdb_playback_services_paused && echo SAFE || echo BUSY-DEFER'
```
Expected: `SAFE` (on `BUSY-DEFER`, wait for playback to end and re-check). Then:

```bash
ssh "${PI}" 'sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_services.sh 2>&1 | tail -40'
```

- [ ] **Step 4: Verify the profile and formats via API**

```bash
ssh "${PI}" 'K=$(sudo grep "^SONARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  curl -fsS -H "X-Api-Key: ${K}" http://localhost:8989/api/v3/qualityprofile | python3 -c "
import sys,json
p=next(x for x in json.load(sys.stdin) if x[\"name\"]==\"Any\")
def leaves(items):
    for it in items:
        q=it.get(\"quality\")
        if q: yield it,q
        yield from leaves(it.get(\"items\",[]))
allowed=sorted(q[\"name\"] for it,q in leaves(p[\"items\"]) if it.get(\"allowed\"))
print(\"minFormatScore\", p[\"minFormatScore\"])
print(\"allowed\", allowed)";
  curl -fsS -H "X-Api-Key: ${K}" http://localhost:8989/api/v3/customformat | python3 -c "import sys,json; print(\"CF count\", len(json.load(sys.stdin)))"'
```
Expected: `minFormatScore -200`; `allowed` = exactly the eight `Bluray-1080p, Bluray-720p, HDTV-1080p, HDTV-720p, WEBDL-1080p, WEBDL-720p, WEBRip-1080p, WEBRip-720p`; `CF count 10`.

- [ ] **Step 5: Commit**

```bash
git -C "${REPO}" add "magic_dingus_box_cpp/scripts/data/sonarr_custom_formats.json" "magic_dingus_box_cpp/scripts/setup_services.sh"
git -C "${REPO}" commit -m "feat(services): reconcile Sonarr Any profile + custom formats (720p/1080p)

Sonarr twin of the Radarr CF/profile block; 10 title-regex formats, -200
floor, cutoff Bluray-720p, leaf-name allowed set. No legacy small-release
entry (greenfield); no profile-language mutation (Sonarr v4 differs).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

**Reversal:** `git -C "${REPO}" checkout` the script + delete the fixture, **re-run this task's Step 3 rsync ship against the reverted files** (and delete the shipped fixture on the box); the profile/CF state is overwritten idempotently on the next run and is otherwise inert (nothing consumes it until the kiosk TV UI in a later plan).

---

### Task 4: Sonarr quality definitions (live-captured) + download client + qBit category + root folder + import scan

Everything Sonarr needs to accept a series add and pull a download. Sonarr's quality-id space differs from Radarr's, so the quality-definition fixture is captured LIVE once (documented capture step, not a placeholder), then enforced idempotently.

**Files:**
- Create: `magic_dingus_box_cpp/scripts/data/sonarr_qualitydefinitions.json` (captured live in Step 1)
- Create: `magic_dingus_box_cpp/scripts/data/sonarr_downloadclients.json`
- Modify: `magic_dingus_box_cpp/scripts/data/qbit_categories.json` (add the `sonarr` category)
- Modify: `magic_dingus_box_cpp/scripts/setup_services.sh` — root-folder step (before the Sonarr download-client step), download-client "Step 15-S" (after Radarr Step 15 ends at line 1889), quality-definition "Step 16-S" (after Radarr Step 16 ends at line 1998), import-scan "Step 17-S" (after the Radarr reconcile ends at line 2148)

**Interfaces:**
- Consumes: `SONARR_KEY` (Task 2), `QBIT_PW` from `.env`, host dir `/data/library/tv` (Task 1), `qbit_categories.json` apply loop (Step 17, already generic).
- Produces: Sonarr root folder `/data/library/tv`; enabled qBittorrent download client with `tvCategory=sonarr`; qBit category `sonarr` (savePath mirrored from the live `radarr` category — Step 1b); tuned 720p/1080p quality definitions; an orphan-import scan pointed at the captured download base. Task 7's `check_sonarr_download_client` / `check_sonarr_root_folder` assert this; the add-a-series smoke consumes all of it.

> Line numbers below are pre-plan snapshots — locate every edit by the quoted text anchor, never by the number (see Conventions).

- [ ] **Step 1: Capture Sonarr's quality definitions live and build the fixture**

Sonarr's quality ids are NOT Radarr's, so the fixture cannot be copied. Capture the live set from the running container (Task 1) and cap only the eight allowed 720p/1080p leaves at the movie-mirrored MB-per-minute targets (720p → max 60 / preferred 40; 1080p → max 100 / preferred 70); every other quality keeps its live values so the reconciler no-ops on it. Stage the live JSON in a file and pass it to the transform as **argv** — do NOT pipe into `python3 -` while also using a heredoc (the heredoc redirection replaces the pipe as stdin, the transform reads EOF, and the `>` redirect truncates the fixture to an empty file):

```bash
ssh "${PI}" 'K=$(sudo grep "^SONARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  curl -fsS -H "X-Api-Key: ${K}" http://localhost:8989/api/v3/qualitydefinition' > /tmp/sonarr_qd_live.json
python3 - /tmp/sonarr_qd_live.json > "${APP}/scripts/data/sonarr_qualitydefinitions.json" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    live = json.load(f)
CAP720  = {"HDTV-720p", "WEBDL-720p", "WEBRip-720p", "Bluray-720p"}
CAP1080 = {"HDTV-1080p", "WEBDL-1080p", "WEBRip-1080p", "Bluray-1080p"}
out = []
for q in live:
    name = q["quality"]["name"]
    e = {"quality": {"id": q["quality"]["id"], "name": name}}
    if name in CAP720:
        e.update(minSize=0, maxSize=60, preferredSize=40)
    elif name in CAP1080:
        e.update(minSize=0, maxSize=100, preferredSize=70)
    else:
        e.update(minSize=q.get("minSize"), maxSize=q.get("maxSize"),
                 preferredSize=q.get("preferredSize"))
    out.append(e)
json.dump(out, sys.stdout, indent=2)
sys.stdout.write("\n")
PY
```

Verify the fixture is non-empty, capped the eight leaves, and preserved real Sonarr ids (the regex is anchored to the exact leaf names so `Bluray-1080p Remux` and 2160p rows do not appear):

```bash
jq -c '.[] | select(.quality.name|test("^(HDTV|WEBDL|WEBRip|Bluray)-(720|1080)p$")) | {id: .quality.id, name: .quality.name, maxSize, preferredSize}' "${APP}/scripts/data/sonarr_qualitydefinitions.json"
```
Expected: EXACTLY eight rows — the four 720p leaves at `maxSize 60, preferredSize 40` and the four 1080p leaves at `maxSize 100, preferredSize 70`. Remux/2160p entries stay in the fixture with their live (uncapped) values but are excluded by the anchored regex. Note the ids are Sonarr's own — they will NOT match Radarr's `4,5,14,6 / 9,3,15,7`. Zero rows means the capture failed (empty fixture) — re-run Step 1.

- [ ] **Step 1b: Capture the live qBit `radarr` category savePath and choose Sonarr's download paths**

Whether Sonarr's imports can hardlink depends on where qBit actually saves — which depends on whether this box ran `migrate_hardlink_layout.sh`. Do not hardcode either answer; mirror the LIVE `radarr` category:

```bash
ssh "${PI}" 'QP=$(sudo grep "^QBITTORRENT_ADMIN_PASSWORD=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  J=$(mktemp);
  curl -fsS -c "$J" -X POST -d "username=admin" --data-urlencode "password=${QP}" http://localhost:8080/api/v2/auth/login >/dev/null;
  curl -fsS -b "$J" http://localhost:8080/api/v2/torrents/categories; rm -f "$J"' \
| python3 -c "import sys,json; print(repr(json.load(sys.stdin).get('radarr',{}).get('savePath','')))"
```
Expected: either `''` (unmigrated box — radarr inherits qBit's `/downloads`-based default) or a `/data/...` path such as `'/data/downloads/complete'` (migrated box).

**Decision rule — record both values now; Steps 3 and 7 bake them in:**

| live `radarr` savePath | `SONARR_CAT_SAVEPATH` (Step 3 fixture) | `SONARR_SCAN_PATH` (Step 7 scan) | import behavior |
|---|---|---|---|
| starts with `/data` (e.g. `/data/downloads/complete`) | same base + `/tv` suffix → `/data/downloads/complete/tv` | same as `SONARR_CAT_SAVEPATH` | HARDLINK (source + library share the `/data` mount) |
| empty `''` | `""` (inherit qBit's `/downloads` default) | `/downloads/complete` | COPY (crosses the `/downloads` and `/data` mounts — matches Radarr's current behavior on this box) |

Any other value (a non-`/data`, non-empty path) is unexpected — STOP and report before continuing.

- [ ] **Step 2: Create the Sonarr download-client fixture**

Write `scripts/data/sonarr_downloadclients.json` — the Radarr fixture with the category field renamed `movieCategory` → `tvCategory` and its value `radarr` → `sonarr`:

```json
[
  {
    "enable": true,
    "protocol": "torrent",
    "priority": 1,
    "removeCompletedDownloads": true,
    "removeFailedDownloads": true,
    "name": "qBittorrent",
    "fields": [
      {
        "order": 0,
        "name": "host",
        "label": "Host",
        "value": "localhost",
        "type": "textbox",
        "advanced": false,
        "privacy": "normal",
        "isFloat": false
      },
      {
        "order": 1,
        "name": "port",
        "label": "Port",
        "value": 8080,
        "type": "textbox",
        "advanced": false,
        "privacy": "normal",
        "isFloat": false
      },
      {
        "order": 4,
        "name": "username",
        "label": "Username",
        "value": "admin",
        "type": "textbox",
        "advanced": false,
        "privacy": "userName",
        "isFloat": false
      },
      {
        "order": 5,
        "name": "password",
        "label": "Password",
        "value": "********",
        "type": "password",
        "advanced": false,
        "privacy": "password",
        "isFloat": false
      },
      {
        "order": 6,
        "name": "tvCategory",
        "label": "Category",
        "helpText": "Adding a category specific to Sonarr avoids conflicts with unrelated non-Sonarr downloads. Using a category is optional, but strongly recommended.",
        "value": "sonarr",
        "type": "textbox",
        "advanced": false,
        "privacy": "normal",
        "isFloat": false
      }
    ],
    "implementationName": "qBittorrent",
    "implementation": "QBittorrent",
    "configContract": "QBittorrentSettings",
    "infoLink": "https://wiki.servarr.com/sonarr/supported#qbittorrent",
    "tags": []
  }
]
```

- [ ] **Step 3: Add the `sonarr` qBit category (savePath from Step 1b)**

Replace the contents of `scripts/data/qbit_categories.json`, using the `SONARR_CAT_SAVEPATH` value decided in Step 1b. On a MIGRATED box (radarr savePath under `/data`):

```json
[
  {
    "name": "radarr",
    "savePath": ""
  },
  {
    "name": "sonarr",
    "savePath": "/data/downloads/complete/tv"
  }
]
```

On an UNMIGRATED box (radarr savePath empty):

```json
[
  {
    "name": "radarr",
    "savePath": ""
  },
  {
    "name": "sonarr",
    "savePath": ""
  }
]
```

(Step 17's apply loop iterates the fixture and already handles per-category savePath via `editCategory`, so this addition is the whole qBit-side change. The `radarr` entry stays exactly as it is on the live box — do not "fix" it here.)

- [ ] **Step 4: Insert the Sonarr root-folder step**

In `setup_services.sh`, immediately after the Radarr download-client block's closing `fi` (line 1889), insert the root-folder step (Sonarr must have `/data/library/tv` registered before a series can be added; idempotent via GET-first):

```bash
# 15-S0. Sonarr root folder /data/library/tv.
#
# Greenfield — Sonarr uses the hardlink-capable /data mount from day one
# (host /mnt/ssd/library/tv, created in Step 2). POST /rootfolder requires
# the path to exist and be writable by PUID; the mkdir + chown in Step 2
# guarantees both. Idempotent: skip if already present.
echo "Configuring Sonarr root folder /data/library/tv..."
# The curl inside the substitution MUST be ||-guarded: under the script's
# set -euo pipefail, a bare failing curl in a $(pipeline) aborts the whole
# run before the tolerant python path ever executes (same convention as
# PROFILE_JSON at Step 8: `curl ... || echo "[]"`).
SONARR_RF_PRESENT=$( (curl -fsS -H "X-Api-Key: ${SONARR_KEY}" \
    http://localhost:8989/api/v3/rootfolder 2>/dev/null || echo "[]") \
    | python3 -c "import sys,json
try: print(any(r.get('path')=='/data/library/tv' for r in json.load(sys.stdin)))
except Exception: print(False)")
if [[ "${SONARR_RF_PRESENT}" == "True" ]]; then
    echo "  ✓ Sonarr root folder /data/library/tv already present"
else
    if curl -fsS -X POST -H "X-Api-Key: ${SONARR_KEY}" -H "Content-Type: application/json" \
        -d '{"path":"/data/library/tv"}' \
        http://localhost:8989/api/v3/rootfolder >/dev/null 2>&1; then
        echo "  ✓ Sonarr root folder /data/library/tv created"
    else
        echo "  WARN: failed to create Sonarr root folder; verify /data/library/tv is writable"
    fi
fi
```

- [ ] **Step 5: Insert the Sonarr download-client reconciler (Step 15-S)**

Immediately after the root-folder step from Step 4, insert the download-client block. It is the Radarr Step 15 python with `BASE` = `:8989` and the Sonarr fixture (`tvCategory` is compared automatically — `match()` iterates every fixture field):

````bash
# 15-S. Sonarr → qBittorrent download client.
#
# Sonarr twin of the Radarr download-client block. Same qBittorrent
# container (reachable via gluetun:8080), category=sonarr so Sonarr's
# torrents segregate from Radarr's. Password injected from .env at apply
# time; masked on GET so drift-detection ignores it. Field name is
# tvCategory (Sonarr) vs Radarr's movieCategory — compared generically.
echo "Configuring Sonarr → qBittorrent download client..."
SONARR_DLCLIENTS_FILE="${SCRIPT_DIR}/data/sonarr_downloadclients.json"
if [[ ! -f "${SONARR_DLCLIENTS_FILE}" ]]; then
    echo "  WARN: ${SONARR_DLCLIENTS_FILE} not found — skipping."
else
    QBIT_PW=$(grep '^QBITTORRENT_ADMIN_PASSWORD=' "${ENV_FILE}" | cut -d= -f2-)
    if [[ -z "${QBIT_PW}" ]]; then
        echo "  WARN: QBITTORRENT_ADMIN_PASSWORD missing from ${ENV_FILE} — skipping."
    else
        SONARR_DLCLIENT_SUMMARY=$(python3 - "${SONARR_DLCLIENTS_FILE}" "${SONARR_KEY}" "${QBIT_PW}" <<'PYEOF'
import json, sys, urllib.request
clients_path, api_key, qbit_pw = sys.argv[1], sys.argv[2], sys.argv[3]
BASE = "http://localhost:8989/api/v3"

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def fields_to_dict(fields):
    return {f["name"]: f.get("value") for f in (fields or [])}

def inject_password(payload):
    new_fields = []
    for f in payload.get("fields", []):
        if f.get("name") == "password":
            f = dict(f)
            f["value"] = qbit_pw
        new_fields.append(f)
    payload = dict(payload)
    payload["fields"] = new_fields
    return payload

def shape_payload(desired):
    payload = {k: v for k, v in desired.items() if k != "id"}
    return inject_password(payload)

def match(live, desired_payload):
    keys = ("name", "enable", "protocol", "priority", "implementation",
            "configContract", "removeCompletedDownloads",
            "removeFailedDownloads")
    for k in keys:
        if live.get(k) != desired_payload.get(k):
            return False
    live_fd = fields_to_dict(live.get("fields"))
    des_fd = fields_to_dict(desired_payload.get("fields"))
    live_fd.pop("password", None)
    des_fd.pop("password", None)
    for k in des_fd:
        if live_fd.get(k) != des_fd[k]:
            return False
    if sorted(live.get("tags", [])) != sorted(desired_payload.get("tags", [])):
        return False
    return True

with open(clients_path) as f:
    desired_clients = json.load(f)

live_clients = http("GET", "/downloadclient") or []
live_by_name = {c["name"]: c for c in live_clients}

created, updated, unchanged = [], [], []
for desired in desired_clients:
    name = desired["name"]
    payload = shape_payload(desired)
    if name in live_by_name:
        live = live_by_name[name]
        if match(live, payload):
            unchanged.append(name)
        else:
            payload["id"] = live["id"]
            http("PUT", "/downloadclient/%d" % live["id"], payload)
            updated.append(name)
    else:
        http("POST", "/downloadclient", payload)
        created.append(name)

print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged}))
PYEOF
)
        echo "${SONARR_DLCLIENT_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
'
    fi
fi
````

- [ ] **Step 6: Insert the Sonarr quality-definition reconciler (Step 16-S)**

Immediately after the Radarr Step 16 block's closing `fi` (line 1998), insert the block below. It is the Radarr Step 16 python with `BASE` = `:8989`, the Sonarr fixture, and the Pi-4 override keyed by leaf-NAME (Sonarr ids aren't pre-known, so name matching is the robust equivalent of Radarr's id map):

````bash
# 16-S. Sonarr quality definitions (720p/1080p MB-per-minute caps).
#
# Sonarr twin of the Radarr block. Its quality-id space differs, so the
# fixture (sonarr_qualitydefinitions.json) was captured LIVE from this
# Sonarr and carries Sonarr's own ids. Match by quality.id (stable), PUT
# on drift. Pi 4B keeps leaner preferred sizes, selected by leaf NAME
# (Sonarr ids aren't known ahead of time, unlike Radarr's hardcoded map).
echo "Configuring Sonarr quality definitions..."
SONARR_QUALITY_FILE="${SCRIPT_DIR}/data/sonarr_qualitydefinitions.json"
if [[ ! -f "${SONARR_QUALITY_FILE}" ]]; then
    echo "  WARN: ${SONARR_QUALITY_FILE} not found — skipping."
else
    SONARR_QD_SUMMARY=$(python3 - "${SONARR_QUALITY_FILE}" "${SONARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
qd_path, api_key = sys.argv[1], sys.argv[2]
BASE = "http://localhost:8989/api/v3"

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

with open(qd_path) as f:
    desired_qds = json.load(f)

# Board gate: fixture is the Pi 5 tuning; a Pi 4B keeps leaner preferred
# sizes. Select by leaf NAME (720p family → 25, 1080p family → 40).
CAP720  = {"HDTV-720p", "WEBDL-720p", "WEBRip-720p", "Bluray-720p"}
CAP1080 = {"HDTV-1080p", "WEBDL-1080p", "WEBRip-1080p", "Bluray-1080p"}
def pi_model():
    try:
        with open("/proc/device-tree/model", "rb") as f:
            return f.read().decode(errors="replace")
    except OSError:
        return ""

if pi_model().startswith("Raspberry Pi 4 "):
    for d in desired_qds:
        nm = d["quality"]["name"]
        if nm in CAP720:
            d["preferredSize"] = 25
        elif nm in CAP1080:
            d["preferredSize"] = 40

live_qds = http("GET", "/qualitydefinition") or []
live_by_quality_id = {q["quality"]["id"]: q for q in live_qds}

unchanged, updated, missing = [], [], []
for desired in desired_qds:
    qid = desired["quality"]["id"]
    if qid not in live_by_quality_id:
        missing.append(desired["quality"]["name"])
        continue
    live = live_by_quality_id[qid]
    drift = (
        live.get("minSize") != desired.get("minSize")
        or live.get("maxSize") != desired.get("maxSize")
        or live.get("preferredSize") != desired.get("preferredSize")
    )
    if not drift:
        unchanged.append(desired["quality"]["name"])
        continue
    payload = dict(live)
    payload["minSize"] = desired.get("minSize")
    payload["maxSize"] = desired.get("maxSize")
    payload["preferredSize"] = desired.get("preferredSize")
    http("PUT", "/qualitydefinition/%d" % live["id"], payload)
    updated.append(desired["quality"]["name"])

print(json.dumps({"updated": updated, "unchanged": unchanged, "missing": missing}))
PYEOF
)
    echo "${SONARR_QD_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def count(label, items):
    if items:
        print("  " + label + ": " + str(len(items)) + " (" + ", ".join(items[:3]) + ("..." if len(items) > 3 else "") + ")")
count("updated  ", s["updated"])
count("unchanged", s["unchanged"])
count("missing  ", s["missing"])
'
fi
````

- [ ] **Step 7: Insert the Sonarr import-scan reconcile (Step 17-S)**

Immediately after the Radarr reconcile block ends (line 2148, after its `fi` — anchor: the `echo "  WARN: failed to trigger Radarr import scan; verify via web UI"` line and its closing `fi`), insert the Sonarr twin using `DownloadedEpisodesScan` (Sonarr's analog of Radarr's `DownloadedMoviesScan`). The scan `path` is the `SONARR_SCAN_PATH` chosen in Step 1b — the block below shows the MIGRATED-box value `/data/downloads/complete/tv`; on an unmigrated box substitute `/downloads/complete` (both are captured values, recorded in Step 1b):

```bash
# 17-S. Sonarr import reconcile — catch completed-but-orphaned episodes.
#
# Sonarr twin of the Radarr reconcile above. RefreshMonitoredDownloads
# polls the now-configured download client; DownloadedEpisodesScan walks
# the sonarr category's save dir and imports anything matching a
# monitored series. The path mirrors the live qBit wiring chosen at
# provisioning time (see qbit_categories.json): /data-based on a
# hardlink-migrated box, /downloads/complete otherwise.
# Both commands no-op on a clean setup with no orphaned downloads.
echo "Reconciling Sonarr import state (catches completed-but-orphaned episodes)..."
SONARR_RECONCILE_OK="✓"
curl -fsS -X POST -H "X-Api-Key: ${SONARR_KEY}" -H "Content-Type: application/json" \
    -d '{"name":"RefreshMonitoredDownloads"}' \
    "http://localhost:8989/api/v3/command" >/dev/null \
    || SONARR_RECONCILE_OK="WARN"
sleep 5
curl -fsS -X POST -H "X-Api-Key: ${SONARR_KEY}" -H "Content-Type: application/json" \
    -d '{"name":"DownloadedEpisodesScan","path":"/data/downloads/complete/tv","importMode":"Auto"}' \
    "http://localhost:8989/api/v3/command" >/dev/null \
    || SONARR_RECONCILE_OK="WARN"
if [[ "${SONARR_RECONCILE_OK}" == "✓" ]]; then
    echo "  ✓ Sonarr scan + import triggered (any orphaned downloads reconcile within ~30s)"
else
    echo "  WARN: failed to trigger Sonarr import scan; verify via web UI"
fi
```

- [ ] **Step 8: Ship the fixtures + script and run**

```bash
for f in sonarr_qualitydefinitions.json sonarr_downloadclients.json qbit_categories.json; do
  rsync -a --checksum "${APP}/scripts/data/${f}" "${PI}:/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/data/${f}"
done
rsync -a --checksum "${APP}/scripts/setup_services.sh" "${PI}:/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_services.sh"
```

Pass the playback-pause gate, then run:

```bash
ssh "${PI}" 'test ! -f /tmp/mdb_playback_services_paused && echo SAFE || echo BUSY-DEFER'
ssh "${PI}" 'sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_services.sh 2>&1 | tail -50'
```
(Proceed only on `SAFE`; on `BUSY-DEFER`, wait for playback to end and re-check.)

- [ ] **Step 9: Verify root folder, download client, category, and quality caps**

```bash
ssh "${PI}" 'K=$(sudo grep "^SONARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  echo "-- rootfolder --";   curl -fsS -H "X-Api-Key: ${K}" http://localhost:8989/api/v3/rootfolder    | python3 -c "import sys,json; print([r[\"path\"] for r in json.load(sys.stdin)])";
  echo "-- downloadclient --"; curl -fsS -H "X-Api-Key: ${K}" http://localhost:8989/api/v3/downloadclient | python3 -c "import sys,json; c=json.load(sys.stdin)[0]; f={x[\"name\"]:x.get(\"value\") for x in c[\"fields\"]}; print(\"enable\",c[\"enable\"],\"host\",f.get(\"host\"),\"port\",f.get(\"port\"),\"tvCategory\",f.get(\"tvCategory\"))";
  echo "-- qbit cats --"; QP=$(sudo grep "^QBITTORRENT_ADMIN_PASSWORD=" /opt/magic_dingus_box/services/.env | cut -d= -f2-); J=$(mktemp); curl -fsS -c "$J" -X POST -d "username=admin" --data-urlencode "password=${QP}" http://localhost:8080/api/v2/auth/login >/dev/null; curl -fsS -b "$J" http://localhost:8080/api/v2/torrents/categories | python3 -c "import sys,json; d=json.load(sys.stdin); print({k: v.get(\"savePath\",\"\") for k,v in sorted(d.items())})"; rm -f "$J"'
```
Expected: rootfolder list includes `/data/library/tv`; download client `enable True host localhost port 8080 tvCategory sonarr`; qBit categories dict includes both `radarr` and `sonarr`, with `sonarr`'s savePath equal to the Step-1b `SONARR_CAT_SAVEPATH` value.

- [ ] **Step 10: Commit**

```bash
git -C "${REPO}" add "magic_dingus_box_cpp/scripts/data/sonarr_qualitydefinitions.json" "magic_dingus_box_cpp/scripts/data/sonarr_downloadclients.json" "magic_dingus_box_cpp/scripts/data/qbit_categories.json" "magic_dingus_box_cpp/scripts/setup_services.sh"
git -C "${REPO}" commit -m "feat(services): Sonarr root folder, download client, qBit category, quality caps

Live-captured quality definitions (Sonarr id space); tvCategory=sonarr
download client; /data/library/tv root folder; DownloadedEpisodesScan
import reconcile; sonarr qBit category savePath mirrors the live radarr
category (hardlinks on migrated boxes, /downloads default otherwise).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

**Reversal:** `git -C "${REPO}" checkout` the script + `qbit_categories.json`, delete the two new fixtures, **re-run this task's Step 8 rsync ship against the reverted files** (and delete the shipped new fixtures on the box); Sonarr's config is overwritten idempotently on the next gated run. Optionally remove the `sonarr` qBit category via the qBit UI.

---

### Task 5: Prowlarr → Sonarr app sync + EZTV enable + per-app API-key fix

Wires Prowlarr's auto-sync to push the TV-capable indexer subset into Sonarr, enables EZTV for season-pack coverage, and fixes Step 14's `inject_api_key` so the Sonarr app gets the SONARR key (not the Radarr key).

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/data/prowlarr_applications.json` (append a Sonarr app entry — idempotent jq)
- Modify: `magic_dingus_box_cpp/scripts/data/prowlarr_indexers.json` (EZTV `enable: false` → `true`)
- Modify: `magic_dingus_box_cpp/scripts/setup_services.sh` — Step 13 EZTV non-fatal guard + summary label (lines 1355-1402), Step 14 per-app key map (argv 1439-1441, `inject_api_key` 1455-1467), NEW Step 14b-S Sonarr min-seeders bump (after the Radarr 14b block), Step 14c Radarr-injection capability filter (lines 1682-1685)

**Interfaces:**
- Consumes: `PROWLARR_KEY`, `RADARR_KEY`, `SONARR_KEY` (the Sonarr readiness probe already ran in 10-S — Task 3).
- Produces: a Prowlarr "Sonarr" application (fullSync, baseUrl `http://localhost:8989`, 5000-family syncCategories, apiKey = live SONARR key) that pushes TV-capable indexers into Sonarr; EZTV enabled but non-fatal at apply time; Sonarr indexers at `minimumSeeders=3`; Step 14c no longer force-injects TV-only indexers into Radarr (which would have tripped `check_radarr_indexers`'s exact-set `extra=` assertion). Task 7's `check_sonarr_indexers` asserts the synced set.

> Line numbers below are pre-plan snapshots — locate every edit by the quoted text anchor, never by the number (see Conventions).

- [ ] **Step 1: Append the Sonarr app to `prowlarr_applications.json`**

The Radarr entry embeds a ~230-line `selectOptions` catalog inside its `syncCategories` field; clone that object exactly and mutate only the app-identity fields + `baseUrl` + `syncCategories` value (Prowlarr's `SonarrSettings` contract uses the same field NAMES `prowlarrUrl`/`baseUrl`/`apiKey`/`syncCategories`, so the clone is valid; Prowlarr regenerates field labels/helpText from its schema on POST). The jq is self-guarding — a re-run (e.g. after a partial failure) must NOT append a second `Sonarr` entry, because Step 14 would then POST a duplicate app name that Prowlarr 400s on, fatally. Run:

```bash
cd "${APP}/scripts/data"
jq '
  if any(.[]; .name == "Sonarr") then . else
  (.[0]
    | .name = "Sonarr"
    | .implementation = "Sonarr"
    | .implementationName = "Sonarr"
    | .configContract = "SonarrSettings"
    | .infoLink = "https://wiki.servarr.com/prowlarr/supported#sonarr"
    | (.fields |= map(
        if .name == "baseUrl" then .value = "http://localhost:8989"
        elif .name == "syncCategories" then .value = [5000,5010,5020,5030,5040,5045,5050,5060,5070,5080,5090]
        else . end))
  ) as $sonarr | . + [$sonarr] end
' prowlarr_applications.json > /tmp/pa.json && mv /tmp/pa.json prowlarr_applications.json
```

Verify:

```bash
jq -r '.[] | "\(.name) impl=\(.implementation) contract=\(.configContract) baseUrl=\([.fields[]|select(.name=="baseUrl")|.value][0]) syncCats=\([.fields[]|select(.name=="syncCategories")|.value][0]|length)"' "${APP}/scripts/data/prowlarr_applications.json"
```
Expected two lines: `Radarr impl=Radarr contract=RadarrSettings baseUrl=http://localhost:7878 syncCats=10` and `Sonarr impl=Sonarr contract=SonarrSettings baseUrl=http://localhost:8989 syncCats=11`.

- [ ] **Step 2: Enable EZTV in `prowlarr_indexers.json`**

In the EZTV entry (the block with `"definitionName": "eztv"` and `"name": "EZTV"`, around lines 154-193), change:

```json
        "enable": false,
```
to:
```json
        "enable": true,
```

(This is the only edit in that block. `enable` for the EZTV object is currently at line 158.)

- [ ] **Step 3: Make an enabled EZTV apply-failure non-fatal in Step 13**

Step 13's reconciler re-raises `HTTPError` for any `enable:true` indexer as FATAL (lines 1379-1385). EZTV is Cardigann + cloudflare-tagged (routes through Byparr) and can 404/challenge at apply time; a down EZTV must not brick provisioning. In the Step 13 python heredoc, add a constant just below `BASE = "http://localhost:9696/api/v1"` (line 1295):

```python
# EZTV is enabled for season-pack coverage but is cloudflare-tagged and
# routes through Byparr — its Cardigann definition can 404/challenge at
# apply time. Treat an EZTV apply failure as a warning, not fatal, so a
# transient Byparr/EZTV outage never bricks a whole setup run. The stable
# indexers the smoke test asserts on are unaffected.
NON_FATAL_ENABLED = {"EZTV"}
```

Then change the `except` block (lines 1379-1385) from:

```python
    except urllib.error.HTTPError as e:
        # Re-raise for ENABLED indexers — a live/active indexer failing
        # is a real problem the operator needs to see fail loudly.
        if desired.get("enable", False):
            sys.stderr.write("FATAL: enabled indexer %r failed: %s\n" % (name, e))
            raise
        skipped.append(name)
```

to — note the broadened exception tuple: EZTV routes through Byparr, whose Cloudflare challenge solves take 10-60s, so the failure can surface as a socket timeout (`URLError`/`TimeoutError`), not only an HTTP status; a bare `HTTPError` catch would let a timeout propagate to bash and kill the run under `set -e`:

```python
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as e:
        # Re-raise for ENABLED indexers — a live/active indexer failing
        # is a real problem the operator needs to see fail loudly. EZTV is
        # the deliberate exception: enabled but allowed to fail soft
        # (HTTP error OR Byparr-challenge timeout).
        if desired.get("enable", False) and name not in NON_FATAL_ENABLED:
            sys.stderr.write("FATAL: enabled indexer %r failed: %s\n" % (name, e))
            raise
        skipped.append(name)
```

Also update the bash summary label (line 1400) so a soft-failed ENABLED EZTV is not mislabeled as a disabled indexer. Change:

```bash
show("skipped (disabled, stale upstream definition)", s.get("skipped", []))
```
to:
```bash
show("skipped (disabled or soft-fail, stale/unreachable upstream)", s.get("skipped", []))
```

- [ ] **Step 4: Fix Step 14's per-implementation API-key injection**

Change the Step 14 python invocation (line 1439) from:

```bash
    APPS_SUMMARY=$(python3 - "${PROWLARR_APPS_FILE}" "${PROWLARR_KEY}" "${RADARR_KEY}" <<'PYEOF'
```

to add the Sonarr key as a fourth argv:

```bash
    APPS_SUMMARY=$(python3 - "${PROWLARR_APPS_FILE}" "${PROWLARR_KEY}" "${RADARR_KEY}" "${SONARR_KEY}" <<'PYEOF'
```

Change the argv unpack (line 1441) from:

```python
apps_path, prowlarr_key, radarr_key = sys.argv[1], sys.argv[2], sys.argv[3]
```
to:
```python
apps_path, prowlarr_key, radarr_key, sonarr_key = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
```

Replace `inject_api_key` (lines 1455-1467) — it currently pushes the single `radarr_key` into EVERY app — with an implementation-keyed version:

```python
def inject_api_key(payload):
    # Select the key by target application: the Radarr app gets
    # RADARR_KEY, the Sonarr app gets SONARR_KEY. Before this fix the
    # single radarr_key was injected into every app, so a Sonarr app
    # would have been configured with Radarr's key and silently failed
    # to sync. Mutate a deep-copied list so the source dict stays clean.
    key = sonarr_key if payload.get("implementation") == "Sonarr" else radarr_key
    new_fields = []
    for f in payload.get("fields", []):
        if f.get("name") == "apiKey":
            f = dict(f)
            f["value"] = key
        new_fields.append(f)
    payload = dict(payload)
    payload["fields"] = new_fields
    return payload
```

(No Sonarr readiness probe is added here — Sonarr's first hard-fail API contact is the 10-S block, which carries the plan's single probe; see Task 3 Step 2. By Step 14, Sonarr has either already answered 10-S or the run is dead.)

- [ ] **Step 5: Insert the Sonarr min-seeders bump (Step 14b-S)**

Sonarr's synced indexers arrive from Prowlarr with `minimumSeeders=1` (no dead-swarm filter — the exact metaDL-hang incident class Radarr's 14b was built for). Insert this near-verbatim twin of the Radarr block immediately BEFORE the line `# 14c. Indexer-sync fallback: directly inject Prowlarr indexers that` (i.e. after the Radarr 14b block's closing `'` of its summary printer). Note the twin fixes the Radarr block's cosmetic label bug (`min=5` strings for a `TARGET = 3` bump):

````bash
# 14b-S. Sonarr: minimum-seeders threshold per indexer.
#
# Sonarr twin of the Radarr 14b block above — same rationale (dead-swarm
# releases from Prowlarr's sync default of 1 hang qBit at metaDL 0%),
# same idempotency (PUT only when the live value differs from 3), same
# caveat (a Prowlarr apps re-sync can reset these to 1; a script re-run
# restores them).
echo "Configuring Sonarr indexer minimum_seeders threshold..."
SONARR_SEEDER_SUMMARY=$(python3 - "${SONARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
api_key = sys.argv[1]
BASE = "http://localhost:8989/api/v3"
TARGET = 3

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

result = {"updated": [], "unchanged": [], "skipped_disabled": []}
for ix in http("GET", "/indexer") or []:
    name = ix.get("name", "?")
    if not (ix["enableRss"] or ix["enableAutomaticSearch"] or ix["enableInteractiveSearch"]):
        result["skipped_disabled"].append(name)
        continue
    bumped = False
    for fld in ix.get("fields", []):
        if fld["name"] == "minimumSeeders":
            old = fld.get("value")
            if old != TARGET:
                fld["value"] = TARGET
                bumped = True
            break
    if bumped:
        http("PUT", "/indexer/%d?forceSave=true" % ix["id"], ix)
        result["updated"].append(name)
    else:
        result["unchanged"].append(name)

print(json.dumps(result))
PYEOF
)
echo "${SONARR_SEEDER_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("updated to min=3  ", s["updated"])
show("already at 3      ", s["unchanged"])
show("skipped (disabled)", s["skipped_disabled"])
'
````

- [ ] **Step 5b: Filter TV-only indexers out of Step 14c's Radarr DB injection**

Without this fix, enabling EZTV BREAKS the existing Radarr smoke test: Step 14c injects EVERY Prowlarr-enabled indexer missing from Radarr into `radarr.db` — and EZTV is *correctly* missing from Radarr (TV-only caps, no 2000-family intersection), so 14c would force it in with movie categories and `check_radarr_indexers`'s exact-set assertion would fail with `extra=EZTV` (and Radarr would live-search a TV-only indexer for movies). In Step 14c's python heredoc, change the `missing` computation (lines 1682-1685) from:

```python
missing = [
    p for p in prowlarr_enabled
    if f"{p['name']} (Prowlarr)" not in radarr_names
]
```

to a GENERIC capability-based filter (not a name allowlist — it self-maintains if more TV-only indexers get enabled later):

```python
# Skip indexers with no Movies capability (e.g. EZTV, TV-only): Prowlarr
# rightly never syncs them to Radarr, so their absence from Radarr is
# CORRECT — not a sync failure for this fallback to repair. Injecting
# one would point Radarr's movie searches at a TV-only indexer and trip
# the smoke test's exact-set indexer assertion.
def has_movies_caps(p):
    cats = (p.get("capabilities") or {}).get("categories") or []
    return any(c.get("name") == "Movies" for c in cats)

missing = [
    p for p in prowlarr_enabled
    if f"{p['name']} (Prowlarr)" not in radarr_names
    and has_movies_caps(p)
]
```

- [ ] **Step 6: Ship fixtures + script and run**

```bash
for f in prowlarr_applications.json prowlarr_indexers.json; do
  rsync -a --checksum "${APP}/scripts/data/${f}" "${PI}:/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/data/${f}"
done
rsync -a --checksum "${APP}/scripts/setup_services.sh" "${PI}:/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_services.sh"
```

Pass the playback-pause gate, then run:

```bash
ssh "${PI}" 'test ! -f /tmp/mdb_playback_services_paused && echo SAFE || echo BUSY-DEFER'
ssh "${PI}" 'sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_services.sh 2>&1 | tail -60'
```
(Proceed only on `SAFE`; on `BUSY-DEFER`, wait for playback to end and re-check.)

- [ ] **Step 7: Verify the Sonarr app synced TV indexers**

The Prowlarr app-sync can take up to ~60s after the app is created; poll Sonarr's indexer list:

```bash
ssh "${PI}" 'K=$(sudo grep "^SONARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  curl -fsS -H "X-Api-Key: ${K}" http://localhost:8989/api/v3/indexer | python3 -c "
import sys,json,re
data=json.load(sys.stdin)
S=re.compile(r\"\s*\((?:Prowlarr|Jackett)\)\s*\$\", re.I)
names=sorted(S.sub(\"\", i[\"name\"]) for i in data if i.get(\"enableAutomaticSearch\"))
print(\"enabled TV indexers in Sonarr:\", names)"'
```
Expected: at least two of `The Pirate Bay`, `TorrentDownload`, `LimeTorrents`, `EZTV` (YTS and Knaben are movies-only and will NOT sync to the 5000-category Sonarr app). EZTV may be absent if Byparr/EZTV was flaky — that is acceptable (it is enabled but soft-fails). Also confirm Prowlarr shows both apps:

```bash
ssh "${PI}" 'K=$(sudo grep "^PROWLARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  curl -fsS -H "X-Api-Key: ${K}" http://localhost:9696/api/v1/applications | python3 -c "import sys,json; print(sorted(a[\"name\"] for a in json.load(sys.stdin)))"'
```
Expected: `['Radarr', 'Sonarr']`.

Also confirm the 14c filter held — EZTV must NOT have been injected into Radarr:

```bash
ssh "${PI}" 'K=$(sudo grep "^RADARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  curl -fsS -H "X-Api-Key: ${K}" http://localhost:7878/api/v3/indexer | python3 -c "import sys,json; print(sorted(i[\"name\"] for i in json.load(sys.stdin)))"'
```
Expected: the pre-existing five Radarr indexers only — NO `EZTV (Prowlarr)` entry.

> Note (upstream-definition caveat): "EZTV is TV-only" and "Knaben/YTS are movies-only" rest on the CURRENT upstream Cardigann category definitions, which Prowlarr recomputes server-side. If `check_radarr_indexers` ever fails with `extra=EZTV` in the future, it means the upstream `eztv` definition grew movie categories — fix by adding it to that check's expected list or restricting its categories, not by hunting a provisioning bug. Symmetrically, if Knaben's definition gains TV categories it will silently join the Sonarr sync (harmless — the ≥2 threshold check still passes).

- [ ] **Step 8: Commit**

```bash
git -C "${REPO}" add "magic_dingus_box_cpp/scripts/data/prowlarr_applications.json" "magic_dingus_box_cpp/scripts/data/prowlarr_indexers.json" "magic_dingus_box_cpp/scripts/setup_services.sh"
git -C "${REPO}" commit -m "feat(services): Prowlarr->Sonarr app sync, EZTV enable, per-app key fix

Add Sonarr app (5000-family TV cats, baseUrl :8989); enable EZTV with a
soft-fail guard + broadened exception tuple in the indexer reconciler;
select RADARR_KEY vs SONARR_KEY per application in Step 14 inject_api_key;
add 14b-S Sonarr min-seeders bump; filter TV-only indexers out of 14c's
Radarr DB injection.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

**Reversal:** `git -C "${REPO}" checkout` all three files, **re-run this task's Step 6 rsync ship against the reverted files**; in Prowlarr delete the Sonarr application via the UI (or `DELETE /api/v1/applications/<id>`); EZTV reverts to disabled on the next gated setup run.

---

### Task 6: Host container-roster updates + VpnHealthMonitor comment + roster tests

Teaches the four host-side scripts (cascade, pause, VPN leak test, storage re-link) plus `admin.py`, the two Python roster tests, and the C++ health-monitor comment about `mdb_sonarr` / `sonarr`. Sonarr joins the playback-pause set (it is Radarr-sized RSS on a 2 GB box, so it must free RAM during games/movies like the other three). The paused set is pinned in FIVE places that MUST change together — `playback_services_pause.sh`, `gluetun_cascade_restart.sh`, `admin.py`, and the two test files — or the cascade will revive/kill it inconsistently (a live bug on 2026-07-31 for the existing trio).

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/gluetun_cascade_restart.sh:35` (`DEPENDENTS`), `:51` (`PAUSED_CONTAINERS`)
- Modify: `magic_dingus_box_cpp/scripts/playback_services_pause.sh:48` (`CONTAINERS`)
- Modify: `magic_dingus_box/web/admin.py:254` (`PLAYBACK_PAUSED_CONTAINERS`), `:3676-3682` (`EXPECTED_CONTAINERS`)
- Modify: `magic_dingus_box_cpp/scripts/check_vpn_required.sh:17` (`EXPECTED_VPN_CONTAINERS`)
- Modify: `magic_dingus_box_cpp/scripts/storage_attach.sh:91` (orphan sweep), `:103-104` (re-link rm/up lists)
- Modify: `magic_dingus_box_cpp/scripts/tests/test_gluetun_cascade.py` (exact roster strings)
- Modify: `magic_dingus_box/web/tests/test_services_pause_marker.py:40` (`PAUSED_SET`) + docker-ps fixtures
- Modify: `magic_dingus_box_cpp/src/media_browser/health/vpn_health_monitor.cpp:22-24, 71-74` (comments only)

**Interfaces:**
- Consumes: container name `mdb_sonarr` / compose service `sonarr` (Task 1). Naming contract: compose service name is EXACTLY `sonarr` and container name EXACTLY `mdb_sonarr` (the pause script derives compose names via `${c#mdb_}`).
- Produces: a roster-consistent host layer where Sonarr pauses/re-links/leak-tests alongside the trio.

> Line numbers below are pre-plan snapshots — locate every edit by the quoted text anchor, never by the number (see Conventions).

- [ ] **Step 1: Update the roster test expectations FIRST (they encode the contract)**

In `scripts/tests/test_gluetun_cascade.py`, update the exact-string assertions to insert `sonarr` after `radarr` (the order matches the `DEPENDENTS`/`CONTAINERS` order chosen below). Change:
- `"restart radarr prowlarr qbittorrent byparr"` → `"restart radarr sonarr prowlarr qbittorrent byparr"`
- `"up -d radarr prowlarr qbittorrent byparr"` → `"up -d radarr sonarr prowlarr qbittorrent byparr"`
- In `test_marker_enforces_stop_of_paused_three` (rename mentally to "four"), the loop `for name in ("mdb_radarr", "mdb_prowlarr", "mdb_byparr")` → `for name in ("mdb_radarr", "mdb_sonarr", "mdb_prowlarr", "mdb_byparr")`
- In `UnpauseFallbackTests.test_failed_start_falls_back_to_compose_up`, `"up -d radarr prowlarr byparr"` → `"up -d radarr sonarr prowlarr byparr"`

In `web/tests/test_services_pause_marker.py`:
- `PAUSED_SET = {"mdb_radarr", "mdb_prowlarr", "mdb_byparr"}` → add `"mdb_sonarr"`
- In `DOCKER_PS_PAUSED`, add a row `"mdb_sonarr\tExited (137) 6 minutes ago",` after the `mdb_radarr` row
- In `DOCKER_PS_ALL_UP`, add a row `"mdb_sonarr\tUp 2 minutes",` after the `mdb_radarr` row

- [ ] **Step 2: Run the Python roster tests to confirm they now FAIL against unchanged scripts**

```bash
cd "${APP}/scripts/tests" && python3 -m pytest test_gluetun_cascade.py -q 2>&1 | tail -15
```
Expected: FAIL — the scripts still emit the old 4-name strings (`sonarr` missing). This proves the tests are exercising the roster.

- [ ] **Step 3: Update `gluetun_cascade_restart.sh`**

Line 35: `DEPENDENTS=(radarr prowlarr qbittorrent byparr)` → `DEPENDENTS=(radarr sonarr prowlarr qbittorrent byparr)`

Line 51: `PAUSED_CONTAINERS=(mdb_radarr mdb_prowlarr mdb_byparr)` → `PAUSED_CONTAINERS=(mdb_radarr mdb_sonarr mdb_prowlarr mdb_byparr)`

- [ ] **Step 4: Update `playback_services_pause.sh`**

Line 48: `CONTAINERS=(mdb_radarr mdb_prowlarr mdb_byparr)` → `CONTAINERS=(mdb_radarr mdb_sonarr mdb_prowlarr mdb_byparr)`

- [ ] **Step 5: Update `admin.py`**

Line 254: `PLAYBACK_PAUSED_CONTAINERS = frozenset({"mdb_radarr", "mdb_prowlarr", "mdb_byparr"})` → add `"mdb_sonarr"`:

```python
PLAYBACK_PAUSED_CONTAINERS = frozenset({"mdb_radarr", "mdb_sonarr", "mdb_prowlarr", "mdb_byparr"})
```

`EXPECTED_CONTAINERS` (lines 3676-3682) — add `"mdb_sonarr"` after `"mdb_radarr"`:

```python
    EXPECTED_CONTAINERS = [
        "mdb_gluetun",
        "mdb_radarr",
        "mdb_sonarr",
        "mdb_prowlarr",
        "mdb_qbittorrent",
        "mdb_byparr",
    ]
```

- [ ] **Step 6: Update `check_vpn_required.sh`**

Line 17: `EXPECTED_VPN_CONTAINERS=(mdb_gluetun mdb_qbittorrent mdb_radarr mdb_prowlarr mdb_byparr)` → add `mdb_sonarr`:

```bash
EXPECTED_VPN_CONTAINERS=(mdb_gluetun mdb_qbittorrent mdb_radarr mdb_sonarr mdb_prowlarr mdb_byparr)
```

- [ ] **Step 7: Update `storage_attach.sh` re-link lists**

Sonarr's `${STORAGE_ROOT}/downloads:/downloads` subdir bind and its `${STORAGE_ROOT}:/data` parent bind both resolve at container start, so a late drive attach leaves Sonarr seeing the empty SD-card placeholder dirs exactly like Radarr (the mount event lands at the host path AFTER the binds resolved; propagation cannot refresh them) — hence Sonarr joins the rm/up re-link lists. Line 91 orphan sweep — add the sonarr filter:

```bash
    for orphan in $(docker ps -aq --filter 'name=_mdb_radarr' --filter 'name=_mdb_sonarr' --filter 'name=_mdb_qbittorrent' 2>/dev/null); do
```

Lines 103-104 re-link — add `sonarr` to both compose commands:

```bash
            timeout 120 docker compose rm -s -f radarr sonarr qbittorrent 2>&1
            timeout 300 docker compose up -d radarr sonarr qbittorrent 2>&1
```

(The staleness probe at lines 58-64/109 uses `mdb_radarr`'s `/library` as the sole signal; Sonarr shares the same drive, so when Radarr's bind is stale Sonarr's is too — recreating both together is correct. No probe change needed.)

- [ ] **Step 8: Update the VpnHealthMonitor comments (comments only — no logic)**

Radarr `/ping` stays the single whole-netns signal (Sonarr shares the namespace), so there is NO logic change. Only the two stale comments update. In `vpn_health_monitor.cpp`, the four-port enumeration (lines 22-23) `only the four app / ports (7878, 8080, 8191, 9696) are.` → `only the app ports (7878, 8080, 8191, 9696, 8989) are.` And the GameQuietMode-stopped set comment (lines 71-74) naming `Radarr / Prowlarr / Byparr` → `Radarr / Sonarr / Prowlarr / Byparr`. Since these are comments, the compiled binary is byte-identical.

- [ ] **Step 9: Run both Python roster test files — expect PASS now**

```bash
cd "${APP}/scripts/tests" && python3 -m pytest test_gluetun_cascade.py -q 2>&1 | tail -5
cd "${REPO}/magic_dingus_box/web" && python3 -m pytest tests/test_services_pause_marker.py -q 2>&1 | tail -5
```
Expected: both green.

- [ ] **Step 10: Isolated Pi compile-verify of the C++ comment change (both configs, never deploy_cpp.sh)**

Per the isolated-build method (rsync the worktree to a scratch dir on the Pi and build there — NEVER `deploy_cpp.sh`, which restarts the live kiosk). `assets/` MUST be synced or cmake `file COPY` fails late. Build both `ENABLE_MEDIA_BROWSER=ON` and `=OFF` to confirm the comment change is a no-op for codegen:

```bash
rsync -a --checksum --delete \
  --exclude build --exclude dev_data \
  "${APP}/" "${PI}:/tmp/mdb_scratch/"
ssh "${PI}" 'set -eo pipefail; cd /tmp/mdb_scratch
  for MB in ON OFF; do
    d=build_$MB; rm -rf "$d"; mkdir "$d"; cd "$d"
    cmake -DENABLE_MEDIA_BROWSER=$MB .. >/dev/null
    make -j3 magic_dingus_box_cpp 2>&1 | tail -3
    cd ..
  done'
```
Expected: both builds link successfully (`[100%] Built target magic_dingus_box_cpp`). `pipefail` matters: without it the `make | tail` pipeline reports tail's exit status and a failed build would exit 0. This does NOT touch `/opt` or the live kiosk (the live unit is `magic-dingus-box-cpp.service`; the scratch build never installs).

- [ ] **Step 11: Ship the changed host scripts to the box + reinstall the /usr/local/bin copies**

Script edits are inert on the box until re-installed to `/usr/local/bin` (the pause + cascade helpers run from there). Ship the sources; the full setup re-run in Task 7 reinstalls them, but reinstall now to activate the roster immediately:

```bash
for f in gluetun_cascade_restart.sh playback_services_pause.sh check_vpn_required.sh storage_attach.sh; do
  rsync -a --checksum "${APP}/scripts/${f}" "${PI}:/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/${f}"
done
rsync -a --checksum "${REPO}/magic_dingus_box/web/admin.py" "${PI}:/opt/magic_dingus_box/magic_dingus_box/web/admin.py"
ssh "${PI}" 'sudo install -m 0755 /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/playback_services_pause.sh /usr/local/bin/playback_services_pause.sh
  sudo install -m 0755 /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/gluetun_cascade_restart.sh /usr/local/bin/gluetun_cascade_restart.sh
  sudo systemctl restart gluetun-cascade-restart.service magic-dingus-web.service'
```

- [ ] **Step 12: Verify the pause/unpause cycle includes Sonarr on the box**

This test itself stops/starts containers — run the same gate first (a genuinely active playback session must not be interrupted, and a pre-existing marker would make the test's own results meaningless):

```bash
ssh "${PI}" 'test ! -f /tmp/mdb_playback_services_paused && echo SAFE || echo BUSY-DEFER'
```
Expected: `SAFE` (on `BUSY-DEFER`, wait and re-check). Then:

```bash
ssh "${PI}" '/usr/local/bin/playback_services_pause.sh pause; sleep 4; docker ps --format "{{.Names}}\t{{.Status}}" | grep -E "mdb_(radarr|sonarr|prowlarr|byparr|qbittorrent)"; echo "-- unpausing --"; /usr/local/bin/playback_services_pause.sh unpause; sleep 6; docker ps --format "{{.Names}}\t{{.Status}}" | grep -E "mdb_(radarr|sonarr|prowlarr|byparr)"'
```
Expected: after `pause`, `mdb_sonarr` (and radarr/prowlarr/byparr) show `Exited`, `mdb_qbittorrent` stays `Up`; after `unpause`, `mdb_sonarr` is back `Up`.

- [ ] **Step 13: Commit**

```bash
git -C "${REPO}" add "magic_dingus_box_cpp/scripts/gluetun_cascade_restart.sh" "magic_dingus_box_cpp/scripts/playback_services_pause.sh" "magic_dingus_box_cpp/scripts/check_vpn_required.sh" "magic_dingus_box_cpp/scripts/storage_attach.sh" "magic_dingus_box_cpp/scripts/tests/test_gluetun_cascade.py" "magic_dingus_box/web/admin.py" "magic_dingus_box/web/tests/test_services_pause_marker.py" "magic_dingus_box_cpp/src/media_browser/health/vpn_health_monitor.cpp"
git -C "${REPO}" commit -m "feat(services): teach the host container roster about mdb_sonarr

Sonarr joins the playback-pause set + cascade + leak test + storage
re-link; update both roster test files; VpnHealthMonitor comments note
port 8989 (Radarr ping stays the single netns signal — no logic change).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

**Reversal:** `git -C "${REPO}" checkout` all eight files; reinstall the prior `/usr/local/bin` copies (re-run this task's Step 11 against the reverted sources) and restart the two services.

---

### Task 7: verify_services + verify_box coverage, full idempotent re-run, add-a-series smoke, C++ deploy note

Adds Sonarr smoke assertions, bumps the acceptance container count, then does the plan's real acceptance: a clean full `setup_services.sh` re-run that ends green, and adding one series via Sonarr's API with the grab landing in the queue (web UI over the tunnel as the human-optional alternative).

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/verify_services.sh` — `require_env` (lines 113-115), new `check_sonarr_*` functions, call sequence (lines 449-456)
- Modify: `magic_dingus_box_cpp/scripts/verify_box.sh:286` (container count 5 → 6, message)

**Interfaces:**
- Consumes: everything from Tasks 1-6.
- Produces: a green `verify_services.sh` including Sonarr checks and a `verify_box.sh` that asserts 6 containers. Final acceptance: a series added via the Sonarr API queues a season search.

> Line numbers below are pre-plan snapshots — locate every edit by the quoted text anchor, never by the number (see Conventions).

- [ ] **Step 1: Require SONARR_API_KEY in `verify_services.sh`**

After line 115 (`: "${QBITTORRENT_ADMIN_PASSWORD:?…}"`), add:

```bash
    : "${SONARR_API_KEY:?SONARR_API_KEY missing from .env}"
```

- [ ] **Step 2: Add the Sonarr check functions**

Insert these FIVE functions after `check_radarr_custom_formats` (after line 316, before `check_qbit_auth` — anchor on the closing `}` of `check_radarr_custom_formats`). They mirror the Radarr checks against `:8989`. `check_sonarr_indexers` uses a threshold (≥2 of the TV-capable set) rather than exact-set equality, because per-indexer Prowlarr app-sync quirks (and EZTV's soft-fail) make the exact synced set nondeterministic — a hard exact-match would flake:

````bash
check_sonarr_root_folder() {
    header "Sonarr root folder (/data/library/tv)"
    local response
    response=$(curl -fsS -H "X-Api-Key: ${SONARR_API_KEY}" \
        "http://localhost:8989/api/v3/rootfolder" 2>/dev/null) || {
        fail "Sonarr /api/v3/rootfolder unreachable"
        return
    }
    if echo "${response}" | python3 -c "import sys,json; sys.exit(0 if any(r.get('path')=='/data/library/tv' for r in json.load(sys.stdin)) else 1)" 2>/dev/null; then
        pass "Sonarr root folder /data/library/tv present"
    else
        fail "Sonarr root folder /data/library/tv missing"
    fi
}

check_sonarr_indexers() {
    header "Sonarr indexers (TV-capable subset)"
    local response
    response=$(curl -fsS -H "X-Api-Key: ${SONARR_API_KEY}" \
        "http://localhost:8989/api/v3/indexer" 2>/dev/null) || {
        fail "Sonarr /api/v3/indexer unreachable"
        return
    }
    local result
    result=$(python3 - "${response}" <<'PYEOF'
import json, re, sys
# TV-capable indexers Prowlarr's 5000-category app-sync can push into
# Sonarr. YTS + Knaben are movies-only and never sync here. EZTV is
# enabled but soft-fails when Byparr/EZTV is flaky, so we require a
# THRESHOLD (>=2 present) rather than an exact set — this catches a
# totally-broken sync without flaking on per-indexer quirks.
POOL = {"The Pirate Bay", "TorrentDownload", "LimeTorrents", "EZTV"}
SUFFIX_RE = re.compile(r"\s*\((?:Prowlarr|Jackett)\)\s*$", re.I)
data = json.loads(sys.argv[1])
got = sorted(
    SUFFIX_RE.sub("", ix["name"])
    for ix in data
    if ix.get("enableAutomaticSearch")
)
tv = [n for n in got if n in POOL]
if len(tv) >= 2:
    print("OK:" + ",".join(tv))
else:
    print("FAIL:only " + str(len(tv)) + " TV indexers synced (" + ",".join(tv) + "); want >=2 of " + ",".join(sorted(POOL)))
PYEOF
)
    if [[ "${result}" == OK* ]]; then
        pass "Sonarr has TV indexers (${result#OK:})"
    else
        fail "Sonarr indexer sync weak: ${result#FAIL:}"
    fi
}

check_sonarr_download_client() {
    header "Sonarr → qBittorrent download client"
    local response
    response=$(curl -fsS -H "X-Api-Key: ${SONARR_API_KEY}" \
        "http://localhost:8989/api/v3/downloadclient" 2>/dev/null) || {
        fail "Sonarr /api/v3/downloadclient unreachable"
        return
    }
    local result
    result=$(python3 - "${response}" <<'PYEOF'
import json, sys
data = json.loads(sys.argv[1])
qbit = next((c for c in data if c.get("implementation") == "QBittorrent" and c.get("enable")), None)
if not qbit:
    print("FAIL:no enabled QBittorrent client"); sys.exit()
fields = {f["name"]: f.get("value") for f in qbit.get("fields", [])}
checks = []
if not fields.get("host"):        checks.append("host empty")
if not fields.get("port"):        checks.append("port empty")
if not fields.get("tvCategory"):  checks.append("tvCategory empty")
if not qbit.get("name"):          checks.append("name empty")
if checks:
    print("FAIL:" + ",".join(checks))
else:
    print(f"OK:host={fields.get('host')}:{fields.get('port')} category={fields.get('tvCategory')}")
PYEOF
)
    if [[ "${result}" == OK* ]]; then
        pass "Sonarr qBittorrent download client wired (${result#OK:})"
    else
        fail "Sonarr download client misconfigured: ${result#FAIL:}"
    fi
}

check_sonarr_quality_profile() {
    header "Sonarr quality profile (Any → Bluray-720p cutoff)"
    local response
    response=$(curl -fsS -H "X-Api-Key: ${SONARR_API_KEY}" \
        "http://localhost:8989/api/v3/qualityprofile" 2>/dev/null) || {
        fail "Sonarr /api/v3/qualityprofile unreachable"
        return
    }
    local result
    result=$(python3 - "${response}" <<'PYEOF'
import json, sys
data = json.loads(sys.argv[1])
expected_allowed = {
    "HDTV-720p", "WEBDL-720p", "WEBRip-720p", "Bluray-720p",
    "HDTV-1080p", "WEBDL-1080p", "WEBRip-1080p", "Bluray-1080p",
}
prof = next((p for p in data if p.get("name") == "Any"), None)
if not prof:
    print("FAIL:profile 'Any' not found"); sys.exit()
problems = []
if prof.get("minFormatScore") != -200:
    problems.append(f"minFormatScore={prof.get('minFormatScore')} (want -200)")
def walk(items):
    for it in items:
        q = it.get("quality")
        if q is not None: yield it, q
        for si, sq in walk(it.get("items", [])): yield si, sq
allowed = {q["name"] for it, q in walk(prof.get("items", [])) if it.get("allowed")}
missing = expected_allowed - allowed
extra = allowed - expected_allowed
if missing: problems.append("missing-allowed=" + ",".join(sorted(missing)))
if extra:   problems.append("extra-allowed=" + ",".join(sorted(extra)))
cutoff_id = prof.get("cutoff")
def name_by_id(items, target):
    for it in items:
        q = it.get("quality")
        if q and q.get("id") == target: return q.get("name")
        if it.get("id") == target: return it.get("name")
        sub = name_by_id(it.get("items", []), target)
        if sub: return sub
    return None
if name_by_id(prof.get("items", []), cutoff_id) != "Bluray-720p":
    problems.append(f"cutoff={name_by_id(prof.get('items', []), cutoff_id)} (want Bluray-720p)")
print("FAIL:" + "; ".join(problems) if problems else f"OK:allowed={len(allowed)}")
PYEOF
)
    if [[ "${result}" == OK* ]]; then
        pass "Sonarr quality profile correct (${result#OK:})"
    else
        fail "Sonarr quality profile drift: ${result#FAIL:}"
    fi
}

check_sonarr_custom_formats() {
    header "Sonarr Custom Formats"
    local response
    response=$(curl -fsS -H "X-Api-Key: ${SONARR_API_KEY}" \
        "http://localhost:8989/api/v3/customformat" 2>/dev/null) || {
        fail "Sonarr /api/v3/customformat unreachable"
        return
    }
    local expected=(
        "AV1 codec (UNWATCHABLE on Pi 4)"
        "x265/HEVC 1080p+"
        "HDR / Dolby Vision"
        "Remux / Raw-HD"
        "x264 codec (BONUS)"
        "Quality release groups"
        "Low-bitrate size-optimized groups"
        "Malware/scam executable in title"
        "Known scam aggregator branding"
        "Non-English title signals"
    )
    local result
    result=$(python3 - "${response}" "${expected[@]}" <<'PYEOF'
import json, sys
data = json.loads(sys.argv[1])
expected = set(sys.argv[2:])
got = {cf["name"] for cf in data}
missing = expected - got
print("FAIL:missing=" + ",".join(sorted(missing)) if missing else f"OK:{len(got)} formats present")
PYEOF
)
    if [[ "${result}" == OK* ]]; then
        pass "Sonarr Custom Formats applied (${result#OK:})"
    else
        fail "Sonarr Custom Formats drift: ${result#FAIL:}"
    fi
}
````

- [ ] **Step 3: Add the Sonarr checks to the call sequence**

In the `# ── Main ──` block, after the existing calls (lines 449-456), add the five Sonarr checks after `check_radarr_custom_formats`:

```bash
check_radarr_indexers
check_radarr_download_client
check_radarr_quality_profile
check_radarr_custom_formats
check_sonarr_root_folder
check_sonarr_indexers
check_sonarr_download_client
check_sonarr_quality_profile
check_sonarr_custom_formats
check_qbit_auth
check_kiosk_qbit_password
check_no_active_cooldowns
check_live_search
```

- [ ] **Step 4: Bump the acceptance container count in `verify_box.sh`**

Line 286: change from:

```bash
  (( UP >= 5 )) && pass "${UP} containers up" || fail "only ${UP} containers up (expect 5)"
```
to:
```bash
  (( UP >= 6 )) && pass "${UP} containers up" || fail "only ${UP} containers up (expect 6)"
```

- [ ] **Step 5: Ship the verify scripts and do the full idempotent re-run**

```bash
rsync -a --checksum "${APP}/scripts/verify_services.sh" "${PI}:/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/verify_services.sh"
rsync -a --checksum "${APP}/scripts/verify_box.sh"      "${PI}:/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/verify_box.sh"
```

Pass the playback-pause gate, then run (twice — the second run proves idempotency):

```bash
ssh "${PI}" 'test ! -f /tmp/mdb_playback_services_paused && echo SAFE || echo BUSY-DEFER'
ssh "${PI}" 'chmod +x /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/verify_services.sh /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/verify_box.sh
  sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_services.sh 2>&1 | tail -70'
```
(Proceed only on `SAFE`; on `BUSY-DEFER`, wait for playback to end and re-check — a setup run restarts paused containers and the kiosk.)

Expected tail: the smoke test at the end reports all checks PASSED (including the new Sonarr checks). A second consecutive run (same gate) must report every reconciler block `unchanged` / `already matches` — proving idempotency.

- [ ] **Step 6: Run the acceptance checks explicitly**

```bash
ssh "${PI}" '/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/verify_services.sh 2>&1 | tail -30'
ssh "${PI}" 'sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/verify_box.sh 2>&1 | grep -E "containers up"'
```
Expected: `✓ All N smoke-test checks PASSED` (N grew by the five Sonarr checks); `✓ 6 containers up`.

- [ ] **Step 7: Add one series (API-first acceptance; web UI as the human-optional alternative)**

The plan's real acceptance is an actual series ADD that queues a season search — proving indexers + download client + root folder + quality profile all cohere. The primary path is fully scriptable (an SSH-driven executor cannot click a UI). Run the whole sequence on the box:

**7a — lookup** (TVDB id is Sonarr's unambiguous lookup key; `tvdb:81189` = Breaking Bad. `tmdb:` prefix support is version-dependent in Sonarr v4 — Phase 2b's kiosk client depends on it, so ALSO run the informational tmdb probe below, but do not gate acceptance on it):

```bash
ssh "${PI}" 'K=$(sudo grep "^SONARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  curl -fsS -H "X-Api-Key: ${K}" "http://localhost:8989/api/v3/series/lookup?term=tvdb:81189" > /tmp/sonarr_lookup.json;
  python3 -c "import json; d=json.load(open(\"/tmp/sonarr_lookup.json\")); print(\"lookup rows:\", len(d), \"| first:\", (d[0][\"title\"] if d else None))"'
```
Expected: `lookup rows: 1 | first: Breaking Bad`.

Informational (not gating): `curl -fsS -H "X-Api-Key: ${K}" "http://localhost:8989/api/v3/series/lookup?term=tmdb:1396"` — record whether it returns the same series (Phase 2b relies on the `tmdb:` path; if it returns nothing, note it in the Phase 2b plan).

**7b — POST the series** (lookup resource + `qualityProfileId` of the `Any` profile + root folder + `firstSeason` monitoring + immediate search):

```bash
ssh "${PI}" 'K=$(sudo grep "^SONARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  PROFILE_ID=$(curl -fsS -H "X-Api-Key: ${K}" http://localhost:8989/api/v3/qualityprofile | python3 -c "import sys,json; print(next(p[\"id\"] for p in json.load(sys.stdin) if p[\"name\"]==\"Any\"))");
  python3 - "$PROFILE_ID" <<PYEOF > /tmp/sonarr_add.json
import json, sys
series = json.load(open("/tmp/sonarr_lookup.json"))[0]
series["qualityProfileId"] = int(sys.argv[1])
series["rootFolderPath"] = "/data/library/tv"
series["monitored"] = True
series["seasonFolder"] = True
series["addOptions"] = {"monitor": "firstSeason", "searchForMissingEpisodes": True}
print(json.dumps(series))
PYEOF
  curl -fsS -X POST -H "X-Api-Key: ${K}" -H "Content-Type: application/json" \
    -d @/tmp/sonarr_add.json http://localhost:8989/api/v3/series \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(\"added series id:\", d[\"id\"], \"| title:\", d[\"title\"])"'
```
Expected: `added series id: <N> | title: Breaking Bad`. (A 400 mentioning the tvdbId means the series is already in the library from an earlier attempt — fetch its id via `GET /api/v3/series` and continue.)

**7c — poll the queue** for a grab attributed to that series (season packs appear as N per-episode rows sharing one `downloadId`; any row with the series id proves the pipeline):

```bash
ssh "${PI}" 'K=$(sudo grep "^SONARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2-);
  SID=$(curl -fsS -H "X-Api-Key: ${K}" "http://localhost:8989/api/v3/series" | python3 -c "import sys,json; print(next(s[\"id\"] for s in json.load(sys.stdin) if s[\"tvdbId\"]==81189))");
  for i in $(seq 1 12); do
    N=$(curl -fsS -H "X-Api-Key: ${K}" "http://localhost:8989/api/v3/queue?pageSize=50&includeSeries=false" | python3 -c "import sys,json; d=json.load(sys.stdin); print(sum(1 for r in d.get(\"records\",[]) if r.get(\"seriesId\")==${SID}))" 2>/dev/null || echo 0);
    if [ "${N}" -gt 0 ]; then echo "QUEUE OK: ${N} record(s) for seriesId ${SID}"; exit 0; fi;
    sleep 5;
  done;
  echo "no queue record after 60s — check /api/v3/history/series?seriesId=${SID} for a grabbed event (a very fast grab may already be importing), else inspect indexer search results"; exit 1'
```
Expected: `QUEUE OK: <n> record(s) for seriesId <N>` within ~60s. If it times out, the history fallback distinguishes "grabbed-and-importing" (fine) from "no release found" (investigate indexers before calling the task done).

**Human-optional UI alternative:** tunnel + browser, if a human wants to see it — `TUNNEL_PID` capture instead of job control (`kill %1` assumes an interactive shell):

```bash
ssh -N -L 8989:localhost:8989 "${PI}" & TUNNEL_PID=$!
# browse http://localhost:8989 → Series → Add New → "Breaking Bad" →
# Root Folder /data/library/tv, Quality Profile Any, Monitor: First Season →
# Add + Start Search; watch Activity → Queue.
kill "${TUNNEL_PID}"
```

**Cleanup (optional):** the test series can stay (it is a real, family-safe library entry), or remove it: `DELETE /api/v3/series/<id>?deleteFiles=true` plus removing any queue items with `removeFromClient=true`.

- [ ] **Step 8: Commit the verify changes**

```bash
git -C "${REPO}" add "magic_dingus_box_cpp/scripts/verify_services.sh" "magic_dingus_box_cpp/scripts/verify_box.sh"
git -C "${REPO}" commit -m "feat(services): Sonarr smoke assertions + verify_box container count 6

Five check_sonarr_* checks (root folder, indexers threshold, download
client, quality profile, custom formats); require SONARR_API_KEY; bump
verify_box acceptance from 5 to 6 containers.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

- [ ] **Step 9: C++ deploy note (comment-only change — kiosk restart deferrable)**

The only C++ edit (Task 6, `vpn_health_monitor.cpp`) is comments-only, so the compiled binary is BYTE-IDENTICAL to what is already running on the box. Therefore **no functional deploy is required for Plan 2a**, and the kiosk the family may be using does NOT need to be restarted for this plan. The source change is committed and ships with the next routine kiosk deploy (e.g. Plan 2b's `SonarrClient`).

> If a deploy is nonetheless desired to keep the box's `/opt` source in lockstep, run it ONLY when the box is idle — it restarts the kiosk:
>
> ```bash
> # WARNING: this restarts magic-dingus-box-cpp.service — the live kiosk.
> # Do NOT run during a game or movie. Comment-only change → identical binary.
> ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
> ```

**Reversal:** `git -C "${REPO}" checkout` the two verify scripts and re-run this task's Step 5 rsync ship against the reverted files; `verify_box`'s threshold reverts to 5 (but a 6-container box then over-passes — keep the bump unless fully rolling back the whole plan).

---

## Self-Review

**1. Spec coverage** (Phase 2 "Stack/Clients" bullets + controller IN list):
- Sonarr 4.x `<version>-lsNN` pin, Gluetun netns, restart:always, /ping healthcheck → Task 1. ✓
- Gluetun ports gains 8989 → Task 1. ✓
- `gluetun_cascade_restart.sh` DEPENDENTS + PAUSED_CONTAINERS; `playback_services_pause.sh` CONTAINERS → Task 6. ✓
- VpnHealthMonitor port list (`vpn_health_monitor.cpp:22`) → Task 6 (comment-only, Radarr ping stays the signal). ✓
- Quality fixtures mirror movie set (profile 720p/1080p, AV1/HEVC/HDR penalties, MB/min defs) reconciled in setup + verify coverage → Tasks 3, 4, 7. ✓
- Prowlarr syncs TV-capable subset to Sonarr; YTS won't sync; enable EZTV (soft-fail guarded, broadened exception tuple; 14c capability filter keeps EZTV out of Radarr) → Task 5. ✓
- Storage root `/data/library/tv` ↔ `/mnt/ssd/library/tv` under the `${STORAGE_ROOT}:/data` mount → Tasks 1 (mount+mkdir) + 4 (root folder POST); hardlink-vs-copy wiring mirrors the live qBit `radarr` category (Task 4 Step 1b capture), so migrated and unmigrated boxes both behave correctly. ✓
- Controller IN items all mapped: SONARR_API_KEY append-if-missing + extraction (T2); quality profile, custom formats+SCORE_MAP, quality definitions (live), download client tvCategory + qBit sonarr category, DownloadedEpisodesScan, Step-14 per-app key fix, Prowlarr Sonarr-app 5000-family, EZTV enable (T3/T4/T5); 14b-S min-seeders twin (T5, controller decision 2); fixtures (T3/T4/T5); host units incl. admin.py EXPECTED_CONTAINERS + storage_attach + check_vpn_required (T6); verify_services/verify_box (T7); vpn_health_monitor C++ (T6, isolated build, pipefail-guarded). ✓
- OUT items (TmdbClient TV, SonarrClient, kiosk UI, 14c-S DB fallback, systemd helper twins, OTA hook) excluded AND made visible in the "Deliberately deferred from 2a" section with the revisit trigger. ✓

**2. Placeholder scan:** No "TBD/TODO/adapt as needed/similar to Task N". Every reconciler block is written out in full. The live-derived values — Sonarr quality-definition ids/fixture (Task 4 Step 1, staged-file + argv pipeline), the qBit `radarr`-category savePath decision (Task 4 Step 1b, explicit decision table), and the API key (Task 2) — are documented capture steps with exact commands and expected outputs, not placeholders.

**3. Type/name consistency:** `mdb_sonarr` (container) and `sonarr` (compose service) used uniformly; `${c#mdb_}` contract respected. `SONARR_KEY` (shell) vs `SONARR_API_KEY` (.env) used consistently with their Radarr analogs. `tvCategory` used in the fixture, the reconciler comparison (generic), and the verify check. Port `8989` and `BASE = http://localhost:8989/api/v3` consistent across Tasks 3-5, 7. The DEPENDENTS/PAUSED_CONTAINERS/CONTAINERS insertion order (`radarr sonarr prowlarr …`) matches the exact test strings updated in Task 6 Step 1. `Bluray-720p` cutoff and the eight allowed leaf names identical in Task 3 (reconciler) and Task 7 (verify). SCORE_MAP names identical to the CF fixture names (10 entries, no legacy). Step 1b's two captured values flow consistently: `SONARR_CAT_SAVEPATH` → Task 4 Step 3 fixture, `SONARR_SCAN_PATH` → Task 4 Step 7 scan command. All `.env` reads in on-box snippets use `sudo grep`; all git commands use `git -C "${REPO}"` on branch `feat/sonarr-stack`.

**4. Reviewer-audit fixes carried:** capture pipeline restructured to staged-file+argv (heredoc-stdin trap); anchored jq verify regex; idempotent guarded jq append for the Prowlarr app fixture; pipefail-safe root-folder substitution; broadened Step-13 exception tuple + summary label; readiness probe moved to 10-S; 14c generic Movies-capability filter; playback-pause gate on every setup run and on the pause-cycle test; per-task quoted-anchor reminders; reversals re-ship reverted files; `-lsNN` tag pre-pull gate; API-first add-a-series acceptance with `tvdb:81189` (tmdb probe informational); `$!` tunnel-PID capture; "five functions" wording.

**Ambiguity resolved:** The spec/controller did not confirm Sonarr's default quality-profile name or its profile-language handling. Resolution: target the `"Any"` profile (Sonarr v4 seeds it) with a WARN-and-no-op guard if absent, and SKIP any profile-language mutation (Sonarr v4 handles language outside the quality profile, unlike Radarr) — with a live pre-check in Task 3 Step 3 to catch a missing `Any` before relying on it, and leaf-name (not group-name) allowed matching so the policy holds regardless of Sonarr's default grouping. Additionally, the spec's "no new bind mounts / hardlink-preserving" storage bullet conflicts with the box's possibly-unmigrated qBit save paths — resolved per controller decision by keeping the legacy `/downloads` mount and mirroring the live qBit `radarr` category at provisioning time (hardlinks when the box is migrated, correct copy-imports otherwise), with the compose comment describing exactly that conditional behavior.
