# Hardware validation kit (`hw_validate.sh`)

One command, run on a real box before a release is tagged, that turns the
findings of the release reviews into PASS / FAIL / WARN / MANUAL lines.
It complements `verify_box.sh` (which it runs and folds in) and the
emulator smoke test (which it drives); it does not replace either.

Run it on **both boards** when a release touches the platform layer
(CLAUDE.md, dual-board contract rule 7). It is written for the Pi 4B, the
tighter target, and adapts on a Pi 5 (no headphone output, no hardware
H.264, N64/Dreamcast offered).

## Usage

On the box, as user `magic` (passwordless sudo), ideally inside `tmux` so a
dropped SSH link cannot interrupt a restore:

```bash
cd /opt/magic_dingus_box/magic_dingus_box_cpp/scripts
bash hw_validate.sh                  # read-only checks, ~1 min, safe any time
bash hw_validate.sh --yes            # full run: games, stop test, audio routing, screenshots
bash hw_validate.sh --dry-run --yes  # print the plan + every state-changing command, run none
```

Options: `--skip-games`, `--skip-audio`, `--skip-screenshots`,
`--no-services` (verify_box without `--with-services`),
`--listen-seconds N` (how long each audio-routing game plays; default 8).

Before `--yes`: TV on, speakers audible, headphones (or a speaker) in the
3.5 mm jack on a Pi 4B, and **one phone paired** (Settings > Connect a
Device) — the kiosk is driven through the phone-remote debug endpoint
exactly like `emulator_smoke_test.py`. The script refuses to start while a
game runs or a movie plays (`kiosk_status.json`), or as root.

Outputs:

* terminal: grouped `[PASS]/[FAIL]/[WARN]/[MANUAL]` lines, a MANUAL
  checklist, a summary;
* `~/hw_validate_<ts>.json`: machine-readable report (`verdict`,
  `summary`, every result with structured `data`, the manual checklist);
* `~/hw_validate_<ts>/`: `verify_box.txt`, `emulator_smoke.txt`, the
  kiosk journal of the current run, `screenshots/*.bmp`. Fetch with
  `scp -r magic@<host>.local:~/hw_validate_<ts> .`

Exit 0 only if nothing FAILed; 1 otherwise; 2 = refused to run.

## Safety guarantees

* **User content is never changed.** Before anything is launched,
  `data/saves`, `data/states`, `data/screenshots`,
  `~/.config/retroarch/saves` and `~/.config/retroarch/states` are copied
  with `rsync -a` to `~/hw_validate_backup_<ts>/` (space-checked first,
  the copy itself verified). Afterwards they are restored with
  `rsync -a --delete` and verified with
  `rsync -anc --delete --itemize-changes` (must print nothing). The backup
  is deleted **only** after a verified restore; on any failure it is kept
  and its path printed. Games auto-save on exit, so without this every
  run would overwrite the owner's `.state.auto` files.
* **settings.json is restored byte-for-byte.** It is copied before the
  audio stage; each output change stops the kiosk, edits
  `audio.output`, and starts it. The original is copied back with the
  kiosk stopped, its md5 compared, and the kiosk restarted. A copy is also
  left in the artifacts dir.
* **Restores run from a trap** on `EXIT INT TERM HUP`. Long stages run as
  background children so a signal is handled at once: the child tree is
  terminated, any leftover `retroarch` is stopped, then settings and
  saves are restored and the report written (`run aborted by signal`).
  An unexpected early exit is reported as a FAIL, never a PASS.
* **Read-only without `--yes`.** `--dry-run` prints every state-changing
  command (`mut` wrapper) instead of running it.

The bats suite (`scripts/tests/test_hw_validate.bats`) exercises this
contract off-Pi: a fake smoke test clobbers saves/states and the run must
restore them byte-for-byte (mtimes included); an unrestorable directory
must keep the backup and FAIL; a SIGTERM mid-stage must still restore.

## What is automated

| Group | Check | Finding it guards |
|---|---|---|
| verify_box | every verify_box.sh `--with-services` item (platform detection, `[pi4] gpu_mem=76` / `[pi5]` config.txt split, display mode, canvas canary, content, cores, kiosk, memory posture, storage, service stack) | folded in as `verify_box: <section>` results |
| Audio | `pactl list short sinks`, default sink not `auto_null` | box silent while every other check passed |
| Audio | Pi 4B: a `Headphones` ALSA card must surface as a `mailbox`/`analog` sink (resolve_audio_sink.sh pattern) | jack output silently resolving to HDMI |
| Audio | both `vc4hdmi*/eld#0` parsed (`monitor_present`, `eld_valid`, `sad_count`) | which port has a TV that takes audio |
| Audio | `91-pulse-ignore-unused-hdmi.rules` absent | udev rule hid the TV's HDMI port |
| Audio | `magic-dingus-audio.service` active, PulseAudio in its cgroup, exactly one `pulseaudio` | kiosk restarts killing the sound server; duplicate daemons |
| Services | `magic-dingus-ota-recovery.service` enabled | power cut mid-OTA |
| Services | `import gunicorn`; web journal says `serving with gunicorn` | Werkzeug fallback in production |
| Services | kiosk `OOMScoreAdjust=-500`; `mdb_byparr` 800 / `mdb_qbittorrent` 300 (WARN = container not recreated yet) | OOM killer choosing the kiosk |
| Rendering | journal `Redraw gate: ON`, `UI batching: ON`, `CRT field rate active` (CRT mode) | perf work silently disabled / old binary |
| Rendering | latest `Redraw gate: drew N / skipped M` report; kiosk CPU% over 15 s from `/proc/<pid>/stat` (top's %CPU convention; WARN > 80 % on the static menu) | idle menu burning CPU |
| Posters | `[artwork]` over-budget episodes (WARN ≥ 3) and LRU evictions (WARN > 50 in 15 min) | eviction/re-fetch loop on a static screen |
| Video | Pi 4B: `v4l2h264dec` exists and was the H.264 decoder (kiosk DEBUG log); no `unusable plane layout` / `frame_map failed` / `skipping frame` this boot | software decode on the 1.5 GB board; frame-mapping regressions |
| Gating | Pi 4B menu offers no N64/Dreamcast playlist; smoke test launched no gated core | Pi 5-only systems reaching a Pi 4B |
| Games (`--yes`) | `emulator_smoke_test.py --games 1` (one game per available core, plus the restart-button path), report parsed; WARN for cores in playlists not exercised | per-core launch/return regressions |
| Games (`--yes`) | PS1 game running 15 s, then `sudo systemctl stop` the kiosk: stop < 20 s, journal `stopped on kiosk shutdown request (exited after SIGTERM)`, no `SIGKILLed`, no systemd timeout, a `.state*` file newer than the stop | progress lost on standby/restart; 5 s SIGKILL on every stop |
| Audio routing (`--yes`) | for each output (Pi 4B: headphone, auto, hdmi; Pi 5: auto, hdmi): restart with that `audio.output`, launch a game, read `ALSA device:` from `~/retroarch_launcher.log`, compare with the expected card (mirrors `pick_game_alsa_device`) + a MANUAL listen prompt | game audio on the wrong output |
| Post-game input (`--yes`) | after each audio-routing game, press Settings with **0 s settle**: Settings must open and no `Master Shuffle selected` may appear | 2026-10-03 post-game input race |
| Screenshots (`--yes`) | main menu, Settings, pairing screen, Movies Browse (if unlocked) via `data/screenshot_request`; the pairing QR region (35–65 % across, 15–55 % down, where `pairing_screen_renderer.cpp` draws it) must contain ≥ 10 % near-white and ≥ 10 % near-black pixels | QR rendered as a black square |

## What stays MANUAL

Printed at the end of every run (and in the JSON):

1. Listen: sound came from the expected output for each audio output tested.
2. Two different pads each get their own mapping in a 2-player game.
3. Pull power mid-OTA; the box recovers and boots to the menu.
4. TV-off update behaviour: update with the TV off, then turn it on.
5. Text edges look right (eyeball the screenshots and the TV).
6. Movie playback smooth with lip sync, including after a seek.
7. Frame pacing on the static CRT menu (no judder; the redraw report shows skips).
8. Movies Library screenshot (navigate there, `touch data/screenshot_request`).

## Off-Pi development

The pure logic lives in `scripts/hw_validate_lib.py` and is unit-tested by
`scripts/tests/test_hw_validate.py` (verify_box parser, ELD parser, smoke
report parser, synthetic-BMP QR check, CPU/redraw/artwork/decoder
classifiers, expected ALSA device, report building). The bash flow is
covered by `scripts/tests/test_hw_validate.bats` with stubbed `sudo` /
`systemctl` and a fake box tree. Both are picked up by `test-ota.yml`.
Keep `hw_validate.sh` shellcheck-clean.

Test hooks (environment): `MAGIC_DATA_DIR`, `MAGIC_BASE_DIR`,
`MAGIC_SETTINGS_FILE`, `HWV_MODEL_FILE`, `HWV_VERIFY_BOX`, `HWV_SMOKE`,
`HWV_KIOSK_LOG`, `HWV_REPORT_DIR`.

## If something goes wrong

* `backup KEPT at ~/hw_validate_backup_<ts>`: restore by hand, per
  directory, with the kiosk idle:
  `rsync -a --delete ~/hw_validate_backup_<ts>/data_saves/ /opt/magic_dingus_box/magic_dingus_box_cpp/data/saves/`
  (likewise `data_states`, `data_screenshots`, `ra_saves` →
  `~/.config/retroarch/saves`, `ra_states` → `~/.config/retroarch/states`;
  `settings.json.orig` → `/opt/magic_dingus_box/config/settings.json` with
  the kiosk stopped). Then delete the backup.
* A stage that cannot find a paired remote fails up front; pair a phone
  and re-run.
* `--yes` runs take roughly 15–25 minutes on a Pi 4B (one launch per core,
  plus four extra launches and five kiosk restarts).
