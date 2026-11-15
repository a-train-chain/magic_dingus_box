# Bluetooth game input validation — 2026-11-13

Validated and deployed code commit **`bd0ee666f401b309a5713859f7211dea9048113a`** on `magic@10.0.0.227` (`magicpi5`, Raspberry Pi 5 Model B Rev 1.1, aarch64 Debian 13/Trixie). Physical controller: Bluetooth Pro Controller, `057e:2009`, kernel `nintendo` driver; gamepad `event7`, separate IMU `event8`, no physical `js` node. The phone remote remains `event6/js0`.

## Builds and regression suites

| Check | Result |
| --- | --- |
| Native Mac CTest, all nine suites | Passed, 16.09 s |
| Full Trixie aarch64 kiosk build, `PISIM_JOBS=4 magic_dingus_box_cpp/dev/pisim/pisim.sh build` | Passed |
| Linux aarch64 CTest, all nine suites | Passed, 15.53 s |
| Release assertions: aarch64, `READY=1`, Media Browser, GPIO | Passed |

Built immutable committed snapshots under `/tmp/mdb-bluetooth-arm64`, separate from the active worktree. Final Linux command:

```sh
docker run --rm --init --platform linux/arm64 \
  -v /tmp/mdb-bluetooth-arm64:/repo -w /repo mdb-pisim:trixie \
  ctest --test-dir magic_dingus_box_cpp/build-pisim --output-on-failure
```

`--init` is necessary for the existing process-group lifecycle test: without a PID 1 reaper, an orphan zombie remains visible to `kill(pid, 0)`. The exact unchanged test failed without `--init` and passed with it. No production or test changes were made for that container issue.

## Real input pipeline and physical acceptance

The scratch Linux uinput harness links the actual production `InputManager::poll()` and libevdev. Before the fix, an unchanged held stick emitted only its initial step and failed the repeat assertion. The final source passed all seven case groups:

- Fallback X, fallback Y and remapped overlay axis: one step per dense short burst, five steps during an unchanged hold, first repeats at **400.3 / 400.8 / 401.1 ms**, then the expected 8 Hz cadence.
- Neutral and reversal stopped/restarted navigation correctly; the D-pad button-event path retained its existing repeat behavior.
- Capture transitions, cleanup/reinitialize and disconnect cleared held-stick state.

The harness stopped the kiosk only around input injection, destroyed synthetic devices, and restored the service. No synthetic devices remained.

The owner separately confirmed that short physical left-stick movements move the menu **one row at a time**, then tested Super Mario 64 and reported **“Everything is working great on Super Mario64.”** This is physical gameplay acceptance, separate from the synthetic tests.

| Game | Evidence |
| --- | --- |
| Super Mario 64, N64 playlist index 17 | KMS ready in 4,064 ms; automatic `Nintendo 64 (PS-style)` mapping; Start `10`, B `0`, A `8`; player 1 preferred reservation `057e:2009`. RetroArch explicitly reassigned the real controller to player 1 despite discovering the phone remote first. Owner confirmed gameplay. User quit normally; journal reports exit status 0 and successful kiosk recovery. |
| Chrono Trigger, SNES | KMS ready in 5,369 ms; seven-second smoke; automatic `Super Nintendo (PS-style)` mapping; same player 1 reservation confirmed in config and RetroArch log. Exact-PID SIGTERM returned to the kiosk in 3,003 ms. This verifies launch/return on a core supported by both boards; physical SNES gameplay was not separately tested. |

Both games' original SRAM and automatic state files were snapshotted before launch, restored afterward, and SHA256-verified again at final health check: **all four files match their original contents**, with no additional selected-game save files left behind. The helper handles hidden playback UI by revealing the existing playlist overlay before opening Settings; it changes no persisted preference.

## Deployment and final health

Deployed artifact SHA256:

```text
32345d7f106dc0683838d3951cac7ad96d7f3e46783c7c5f24aa27bb27807d88
```

Target `ldd` resolved every library. The deployed binary hash matched the artifact both after replacement and after game tests. Original binary SHA256 `6d5142a72f110f8e1ec50e7c222baeedb17d4b1e2351cc7952cbf85b8025ce0f` is preserved as `.prev` and separately at:

```text
/home/magic/bluetooth-input-validation-20261113/deploy-original-20261113-182527/magic_dingus_box_cpp.original
```

Compared 420 tracked production source/CMake/script files against the live installation; only intended discovery files differed. Then synchronized only the eight changed production source files from `d827232..bd0ee66`, checking every original hash before replacement and every final hash afterward. Original source files and a manifest are under the same unique backup's `source-originals/`. No blanket sync or deletion was used.

Final checks: kiosk service **active/running**, PID 43345; fresh playback status; no RetroArch process; no synthetic input devices; all eight deployed source hashes correct. `config/settings.json` remains byte-for-byte unchanged, SHA256 `3e1ee91549be37bc488c13a483d245190088d81fa9aeae67c621b3e590dbdf79`.

## Limits and retained evidence

The owner confirmed **no Pi 4 hardware is available**. No physical Pi 4 test was performed. The single generic aarch64 binary, portable regression suites and existing Pi 4 runtime-policy tests provide compatibility evidence; they do not substitute for real Pi 4 validation. N64/Dreamcast remain gated off on Pi 4. No dependencies or board compile switches were added.

Harness sources, build/test logs, source-sync/health manifests and game snapshots/results were copied outside plan scratch to `/tmp/mdb-bluetooth-validation-artifacts/`. Pi-side game backups and results remain under `/home/magic/bluetooth-input-validation-20261113/game-tests/`, including `n64-20261113-182928` and `snes-20261113-183354`. No push or release was performed.
