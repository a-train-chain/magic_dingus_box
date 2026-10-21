# Pi 4 playback and image readiness implementation plan

> For agentic workers: use superpowers:subagent-driven-development; test each behavior before changing it.

Goal: fix concrete content compatibility and clone-safety gaps found while validating the real Pi4, preserving one runtime-selected build for both boards.
Architecture: keep existing upload presets and provider setup; strengthen validation and generated encoding formats, download scoring, and clone preflight. No C++ pipeline retuning without measured evidence.
Tech stack: Python/Flask/FFmpeg, Bash, pytest/Bats.
Spec: user's request to repair VPN, minimize skipping, and audit shared-image readiness; AGENTS.md dual-board contract.

Constraints: runtime board detection only; no new production dependencies; preserve media, saves, credentials and Pi5 behavior except shared compatibility fixes. Do not claim fresh-card/Pi5 validation from Pi4 alone.

- [x] Upload compatibility: reproduce acceptance of 10-bit/non-420/>30fps H.264; reject incompatible bypass and generate <=30fps 8-bit420 masters. Bound Pi4 encoder threads while preserving Pi5 policy. Run focused regression tests and a real conversion.
- [x] Download scoring: prove HEVC1080/HDR can pass after positive bonuses; make forbidden formats remain below threshold under allowed bonuses. Keep mirrored config/install/verification expectations aligned; test combinations.
- [x] Clone safety: preserve rollback archive off-card and verify hashes before removal. Add a preflight regression for rollback archive paths that currently evade the backup tripwire; do not try to scan arbitrary nested archive formats.
- [x] Review each change, run local regression suite, deploy only verified changed files to Pi4, verify live behavior.
- [ ] VPN (live validation deferred at user request): establish handshake, gateway, external connectivity and forwarding independently; preserve per-device identity, use current provider policy, request fresh credentials if evidence requires them.
- [x] Playback: inventory every local video; exercise each through kiosk with bounded samples, plus sustained highest-load sample and transcode contention. Record limits of current QoS telemetry.
- [x] Shared image: audit fresh-boot configuration and source content completeness; verify existing Pi5 read-only. Report fresh-card and physical-input checks still required.


## Validation evidence and release limits

- Pi4: all 59 local videos probed as H.264 640x480/30fps, 8-bit 4:2:0 (53 limited-range, 6 full-range). All 59 advanced through bounded real-kiosk playback samples and transitions; no non-advancing samples. These are short samples, not full-duration certification of every file.
- Real Pi4 concurrent upload: 15-second 1080p60/10-bit source converted through the updated Flask upload route to 1280x720, H.264 level 4.1, yuv420p, 30fps in 26.5 seconds. Temporary isolated data directory; production CSRF policy unchanged. Kiosk continued playing, no logged QoS/stall/error, NRestarts=0, throttled=0x0; about 1.08 GiB available during conversion. Existing telemetry does not count every displayed/dropped frame.
- Upload suite: 44 focused passes; full web suite 663 passed, 4 skipped (Mac lacks gunicorn/flask_sock). Local shell/regression suite: 343 passed. Full-range upload/re-upload regressions pass with real FFmpeg. Independent review caught and prompted that correction; final independent re-review passed both full-range regressions with no outstanding findings.
- Updated upload module and convergence/verification/clone-preflight scripts deployed to Pi4. Web service restarted successfully. No C++ rebuild needed for these changes. Download-policy convergence will run with future service provisioning; not applied to the stopped VPN stack.
- Pi5 read-only verify_box: 38 passes, 0 failures, 2 warnings (beta channel; optional Dreamcast BIOS missing with HLE fallback). This is an existing installation, not a fresh-image test.
- Prior session: all seven supported emulator cores passed Pi4 launch/return tests and kiosk restart recovery; current C++ build deployed, board-specific boot config applied and rebooted.
- VPN: original configuration restored, diagnostic credential copies removed from SD after verified Mac backup. User declined fresh credentials and further testing. Stack stopped for this session; services remain enabled and may start again after reboot. A distinct valid ProtonVPN WireGuard configuration with NAT-PMP enabled is still required before claiming functional VPN/media downloads. Provisioning remains available through Content Manager.
- Rollback archive moved off SD after SHA256 verification; clone preflight now blocks matching rollback directories. Backups are under ~/.codex/backups/mdb-pi4-20261021 on the operator Mac, not release content.

### Before publishing a golden image

1. Use an updated full-content Pi5 donor: this Pi4 lacks the N64/Dreamcast content that first boot prunes for Pi4. The sync_source_box helper is not a substitute for deploying the complete current source/release.
2. Put the donor on the stable release channel. Run the existing clone preparation/verification workflow; never distribute per-device credentials or pairing state.
3. Flash the exact same final image to fresh cards in both Pi4 and Pi5. Confirm first boot, unique identity, partition growth, locked Media Browser, offline/no-SSD behavior, then board-appropriate game/video paths.
4. Physically confirm screen, audio, buttons, rotary knob, power handling and game controls. Automated status checks do not substitute for these observations.
5. Provision each box's own Wi-Fi and optional VPN through the documented setup. VPN live validation and movie/SSD playback were intentionally not completed here.

- Highest-bitrate local file: 59 consecutive position samples advanced from 0.22 to 58.47 seconds, no logged playback errors or throttling. The harness initially sent input during startup; input was retried after the menu became ready. Temporary validation playlist removed and normal kiosk restarted afterward. All four deployed production files matched local SHA256.
