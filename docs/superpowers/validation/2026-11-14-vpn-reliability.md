# VPN reliability: predeployment and owner-box validation

Implementation evidence belongs in `.superpowers/sdd/2026-11-14-vpn-reliability/task-6-report.md`. This is the execution runbook. **Live results are pending until the controller records them here.** The disposable recipe and deterministic replay prove different things; replay success does not prove a live tunnel, NAT-PMP lease, or inbound reachability.

## Gates and boundaries

1. Complete the fresh Astra whole-branch review and fix/re-review loop. Integrate locally into `pi5-test` only, preserving its Bluetooth work. No push, shared merge, release, new runtime package, or C++ rebuild/restart is required for these Python/shell/web/systemd changes.
2. Run the Linux identity and pinned-image gates below from an ephemeral **code/test-only** checkout. The exact cached image is `qmcgaw/gluetun:v3.41.3@sha256:fa19cc76b2af13d57a8d3dc3066f2ada061b1c761b8aecf989b3877c0486e027`. Never substitute a floating tag, pull a replacement, or attach production auth/env/config to the disposable container.
3. Fresh backup and baseline precede production changes. Then inspect a scoped rsync dry-run and apply the common delivery hook. A failed gate stops deployment or further fault injection; it is never reported as a skip/success.
4. The kiosk stays running. No media deletion, SSD unplug, stale watch-position restoration, production-key second tunnel, host firewall/route changes, or ad hoc recurring monitor. Use only short, bounded reviewed VPN interruptions. Pi 4 hardware remains a release gate.

## Local and Linux tests

From the reviewed repository root:

```sh
python3 -m unittest discover -s magic_dingus_box_cpp/scripts/tests -p 'test_vpn_recovery_*.py'
python3 -m unittest discover -s magic_dingus_box_cpp/scripts/tests -p test_gluetun_cascade.py
/opt/homebrew/bin/bats tests/local/compose_image_pins.bats tests/local/gluetun_auth.bats \
  tests/local/vpn_port_sync.bats tests/local/vpn_recovery_delivery.bats \
  tests/local/recreate_gluetun.bats tests/local/compose_lock_coverage.bats \
  tests/local/verify_box_vpn_tunnel.bats
```

The five untouched Mac Bash 3.2 playback ownership failures (`EXISTING[@]`, line317) are already reproduced against baseline; do not relabel them as regressions or repeatedly rerun them on the Mac. The no-flock Mac skip is also known. Root should stage the existing small Homebrew Bats shell sources under the Pi's temporary test checkout, run the affected Bats suites there with Bash 5 and real flock, then remove only that staged copy. This installs no runtime package. Run playback ownership tests once on Linux so the Mac limitation is not the final evidence for that boundary.

On Linux root, with the existing production `magic` account's NOPASSWD sudo ability:

```sh
MDB_TEST_ROOT_BOUNDARY=1 MDB_TEST_OTA_USER=magic python3 -m unittest discover \
  -s magic_dingus_box_cpp/scripts/tests -p test_vpn_recovery_cli.py \
  -k test_linux_root_owned_intent
```

Run the ordinary full suite as the unprivileged account; the command above selects only the Linux root-boundary fixture. Several ordinary CLI tests deliberately mock an unprivileged identity and cannot run meaningfully as real root.

This fixture creates temporary root-owned 0600 intent/lock, genuinely drops the OTA child to the unprivileged account, uses real sudo state helpers, checks uid/euid/gid/HOME/file ownership, and cleans its temporary tree. Docker observations alone are stubbed; production intent and containers are untouched. Missing Linux/root/sudo prerequisites must be recorded as an unfulfilled deployment gate. The ordinary package suite also exercises real isolated HTTP qBit localhost/127.0.0.1 Netscape-cookie handoff, including curl session expiry 0; it contacts no production qBit.

## Disposable pinned-image contract gate

Root runs this **before production fault injection**, after review, from the ephemeral full test checkout. Choose fresh paths; the fixture directory must not already exist:

```sh
contract_token=$(python3 -c 'import secrets;print(secrets.token_hex(8))')
contract_name="mdb-vpn-contract-${contract_token}"
contract_fixture="/tmp/${contract_name}"
sudo python3 magic_dingus_box_cpp/scripts/tests/vpn_recovery_contract.py \
  --opt-in-disposable-contract --container "$contract_name" \
  --fixture-dir "$contract_fixture" \
  --auth-dir "$contract_fixture/services/config/gluetun/auth"
```

Exit 0 and JSON `ok:true`, all assertions true, `cleanup:"removed"` are required. Save only this sanitized JSON. The recipe refuses default/production names, any existing requested container, existing fixture paths, auth outside its own fixture, and uncached/mismatched image metadata. There is intentionally no production-target override mode.

The fixture generates a fresh scoped control key and contains a **fixed public dummy WireGuard private key**, dummy peer keys, and a public recorded catalog whose timestamp is recent and nonfuture. It reads no production env/key/config. It uses one disposable bridge container, no published ports, and one bind to its own temporary Gluetun directory. Before execing the pinned entrypoint, the bootstrap installs table 200 blackhole routes and priority 1 destination rules for every fixture endpoint **inside that container namespace only**. This keeps global endpoint addresses acceptable to the unchanged production validator while making the dummy endpoints unroutable. No host rule, route, Docker network, or production container is changed.

Required assertions include:

- Authenticated settings/status GET; unauthenticated private GET and both PUT routes denied; public forwarding **GET only** returns zero. There is no invented portforward mutation/restart API.
- Native Proton hostname+endpoint partial PUT is applied, with the dummy private key unchanged. Identical settings return `settings left unchanged` and preserve tun0; explicit authenticated stopped/running changes the tunnel ifindex while Docker ID/start/PID remain stable.
- Exactly one observed global UDP/eth0 VPN firewall endpoint matches the selected target. INFO `Using protonvpn servers from file ... more recent` attests the fixture file through the real adapter and actual default INFO logging.
- Endpoint blackholes exist before and after the transitions. With VPN explicitly stopped, OUTPUT policy is DROP, a separate numeric TCP probe fails, and the real DNS/TCP/TLS traffic adapter fails. The harness verifies GNU wget availability, BusyBox timeout availability, and actual inner-child termination so missing tools cannot masquerade as firewall success.
- A disposable Docker stop produces the actual `TimeNano Actor.ID` event fields consumed by the controller.

Catalog identity binds the complete validated provider, including timestamp, all server fields, duplicates and ordered IP lists. Only the outer server-array order is canonicalized for hashing: the pinned image sorts that array while flushing `servers.json`. The original snapshot and candidate selection order remain intact. INFO file-wins evidence and matching disk identity are both still required.

The recipe has a 180s aggregate command deadline. Each direct command is bounded to 10s or remaining time; protected HTTP remains bounded by the adapter (8s/remaining); startup/tunnel transition waits are at most 25s each within the aggregate budget. The container's traffic child uses BusyBox `timeout 3` with GNU wget `--tries=1 --timeout=1`; its host-side caller has a 1.3s cap. Cleanup gets a separate 3s ownership inspect plus 7s remove bound. SIGINT/SIGTERM trigger the same cleanup path; SIGKILL/power loss cannot be trapped.

Removal uses the inspected immutable container ID after matching this run's random ownership label and, when available, the create response ID. Name collisions/replacements never grant removal of a foreign container. Normal success/failure removes only the created fixture directory. Failed/uncertain cleanup fails the gate and retains the fixture for inspection. If interrupted by SIGKILL, inspect **only this run's** exact disposable name/label, remove its verified ID and fixture, and record the abnormal cleanup; never use a broad `docker rm`, prune, or wildcard delete. Contract mismatches require source/implementation review and a rerun, not relaxing production validation.

## Fresh baseline and secure backup

Target remains `magic@10.0.0.227` (magicpi5), but confirm identity again. Earlier read-only numbers are orientation only: kiosk PID 308949 / NRestarts 0, Canada, port 60906=qBit, 13 playlists/32 media/203 ROMs. Never use these values as the fresh baseline.

Root's retained `/private/tmp/mdb-vpn-reliability-20261114/baseline.py` samples only allowlisted metadata/hashes/counts over SSH stdin. Take a fresh sample immediately before deployment and retain a new post-test sample. Include:

- kiosk PID, start, NRestarts, active unit and web responsiveness; `.env` hash, desired policy, recovery intent revision/inhibitors, state/status freshness; current Docker ID/start/PID/tun0 ifindex, verified traffic/public exit, selected endpoint, forwarded port and qBit readback;
- content/playlist/ROM inventories, SSD library/download inventory hashes/counts, watch-state identity/count hashes (not changing playback position), settings, Bluetooth pairing/config identity;
- services/unit/drop-in inventory, playback pause marker/owner journals, drive guard ownership, firewall and unpublished 8000. Do not print complete inspect/settings, credentials, headers or auth files.

Create a **new timestamped root 0700** `/var/backups/mdb-vpn-reliability-<UTC>` backup before deployment. Preserve modes/owners of old code/scripts/web/systemd/compose, root `services/.env`, Gluetun auth/catalog/config, desired policy and persistent recovery intent/state; retain preexisting unit/drop-ins and the baseline. Do not archive media/download trees merely to back up code. Avoid reading live SQLite into a faux coherent backup; keep data untouched and use its established backup mechanism if needed. Secure backup contents stay on the Pi and out of diagnostics/chat.

`/var/backups/mdb-vpn-recovery-20261114` is the **original Netherlands recovery backup**, not the current Canada rollback point. Retain it and every original recovery backup. Do not restore stale watch progress/settings/data after the user continues playback.

## Scoped deployment and auth activation

The parent controller's code-only staging generator is `/private/tmp/mdb-vpn-reliability-20261114/prepare_deploy.py`. It maps changed web, C++scripts, systemd and golden-image sources, rejects protected paths/deletions, and maps changed compose to both the source copy and active root `services/docker-compose.yml`. Inspect its generated manifest against the reviewed diff, with Bluetooth work explicitly preserved.

Use the established rsync exclusions and `-rltci` flags: **no `-p/-o/-g`**, since a private 0700 stage root must not change existing application-directory permissions. Run `--dry-run` first, inspect every target and any proposed deletion. These changes need additions/modifications only, so no `--delete` is needed; if a different delivery path proposes it, review its exact effects before proceeding. Never overwrite per-box env/config/auth/state/content through source sync.

The source staging generator does **not** refresh `/usr/local/bin`. After the fresh backup and inspected sync, run this attended scoped handoff (do not substitute the common hook alone):

```sh
sudo python3 /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/activate_vpn_recovery.py --opt-in-scoped-activation
```

It first stops an active watcher, provisions/loads auth, and runs the common hook with the watcher quiesced. It atomically refreshes the two changed installed entrypoints: `gluetun_cascade_restart.sh` → `/usr/local/bin/gluetun_cascade_restart.sh`, and `qbit_port_sync.sh` → `/usr/local/bin/qbit-port-sync.sh`, using the same mappings as `update.sh:refresh_out_of_tree_files`. No persistent package copy is installed. It resumes only a previously active configured watcher and requires a **fresh** running fingerprint, matching MainPID, and that PID's actual `vpn_recoveryctl.py ... run` command. Expected sanitized output is `{"watcher":"native","pid":<PID>,"fingerprint":"<SHA256>"}`; this proves native activation, not VPN Ready. Originally inactive/unconfigured watchers return `{"watcher":"inactive"}` without starting the stack; explicit stopped intent is preserved. Any failure stops deployment: inspect the watcher before manually resuming. Never run old and new watchers together.

This helper is for this reviewed, attended scoped path only. It is deliberately not called by `setup_memory_tuning.sh` or OTA: the OLD updater must retain its installed-helper refresh **after** verified kiosk startup. Normal deploy keeps its existing `refresh_out_of_tree_files` call. Refresh the web service via the existing deployment path. Do not rebuild/restart the unchanged C++ kiosk. Verify the kiosk's PID/NRestarts remain at baseline after the hook and web restart. Inspect sanitized `status --summary`, unit state and auth file modes; never cat recovery.key or complete settings. Confirm control 8000 remains unpublished.

On **magicpi5 only**, Content Manager → Media Browser → Advanced: desired primary **Canada**, explicitly allowed fallback **United States**, Apply. Save both policy files under the existing maintenance workflow. Omitted fallback elsewhere stays empty. An operator-stopped stack must remain stopped after Apply; use an explicit supported start if it should run. This is desired policy, separate from effective server selection; automatic recovery must not rewrite `.env`.

Catalog compatibility: reject/defer every provider timestamp above `int(time.time())`, preserving the existing file and known-good backup. A corrected clock permits the next bounded failed-refresh retry (at least 600 seconds); never rewrite upstream timestamps or reload a healthy tunnel to compensate. Catalog load accounting distinguishes a no-op Compose start from a changed container start. A newly unattested autonomous start may obtain one necessary recovery load; the resulting start is charged even without log proof, and the global 600-second disruptive restart limit still applies.

## Bounded Ready gate

Use a finite read-only waiter between each live action, for example on the Pi:

```sh
sudo timeout 420 python3 - <<'PY'
import sys,time
sys.path.insert(0,'/opt/magic_dingus_box/magic_dingus_box_cpp/scripts')
from vpn_recovery.common import Paths
from vpn_recovery.cli import read_status
until=time.monotonic()+410
while time.monotonic()<until:
    s=read_status(Paths())
    if s.get('ready') and s.get('traffic_verified') and s.get('qbit_synced') and s.get('dependents_ok'):
        print('Ready: fresh traffic, port, qBit readback and dependents verified')
        break
    time.sleep(5)
else:
    raise SystemExit('Ready deadline exceeded')
PY
```

A successful HTTP/compose request is not this gate. Also check current expected generation/endpoint and desired Canada/US policy through sanitized status. If the gate fails, end the active injection, restore only its owned fault rules/settings, and investigate before further tests.

## Live fault matrix (controller executes after final review)

Root selects/reviews the concrete generation-fenced injection commands against the current pinned state. Before each injection, record the fresh baseline and immutable Docker ID. Prepare an independent bounded restoration watchdog **before adding any fault**, plus `try/finally`/EXIT/INT/TERM restoration. Identify owned rules by a fresh unique comment/token and exact full rule; touch only the inspected production container's namespace. No host-wide firewall edits, global chain flushes, broad route resets or unbounded faults. A replacement namespace naturally discards its rules; cleanup must never blindly transfer the old fault to a new container.

| Case | Finite action and restoration | Required observation |
|---|---|---|
| Wrong qBit port only | Use existing cookie-authenticated preference client under compose and generation/intent fence to set a different valid listen port; leave UPnP/random off. Restore baseline only if still that generation; otherwise let verified sync reconcile. Bound the test to 90s. | Port writer corrects within 10s cadence with readback; no VPN rotation/restart or torrent pause/resume ownership. |
| Sustained lease loss, working traffic | Temporarily reject only the actual NAT-PMP gateway UDP 5351 in the inspected VPN namespace, with a unique owned rule and a 240s removal watchdog. Confirm Gluetun actually reports zero; a held cached lease is **not** a simulated loss. Remove fault on successful candidate transition or at the deadline. | DNS/TCP/TLS stays good; established loss under 90s does not rotate, confirmed 90s does; new lease aligns qBit. Record raw timing without credentials. If pin never reports zero during this bounded injection, record test unproven and review another injection; do not invent PUT `/v1/portforward`. |
| First lease never arrives | With a 360s restoration watchdog armed, use supported lifecycle inhibition/restart and install a temporary NAT-PMP block in the **new, inspected** namespace before its first lease. Verify zero from first observation; a lease that arrived before the block invalidates this attempt. Never share its key with another container. | No premature retry at 119s; action at/after 120s first-port grace, bounded candidates/restart. Remove only owned block, then require Ready. |
| Selected Canada server failure → Canada/US | Save current effective endpoint; temporarily reject its UDP 51820 only, with a 360s removal watchdog. For fallback proof, use a second explicitly reviewed owned rule against the next distinct Canadian trial when selected, within the same finite test window. Clear both on US success or expiry. | 60s confirmed traffic loss; at most two distinct Canada hostnames before US, generation changes, endpoint and fresh traffic/lease/readback agree. Healthy US remains selected for later polls; `.env` stays Canada. If a replacement container discards rules, record that limit instead of extending injection indefinitely. |
| Probe-site outage / WAN outage | Replay covers policy deterministically. A live probe-host block, if run, must be confined to the VPN namespace and separately bounded to 90s, with both probe providers handled explicitly. Do not disable the Pi/host's network or interfere with SSH. | Independent provider avoids one-site outage recovery; genuinely absent WAN suppresses disruptive failover. Fake replay alone is not a live WAN proof. |
| Explicit stop / maintenance / playback | Use supported lifecycle stop/start and maintenance controls, never infer permission from exit 137. Bound stopped observation to 30s before explicit start. Exercise existing playback pause/unpause only while the owner's current activity allows it; keep a baseline of owner journals. | No autonomous resurrection while stopped/inhibited; paused heavyweight services stay paused during recovery, qBit eligible; no new torrent pause owner. SSD unplug remains deferred. |

Do not inject faults simultaneously. After every row, prove removal of its exact owned rules, require Ready, verify the unchanged kiosk PID, compare preservation baseline and record action/recovery times. Stop testing on data/settings/pairing drift, a kiosk restart, lost SSH, persistent cleanup failure, or failed Ready gate. An ended injection that did not produce the intended observation is an incomplete test, not a pass.

## External port, services and steady state

After faults are removed, take the newly **verified** VPN public exit and forwarded TCP port from bounded current observations. Confirm qBit listens on exactly that port. From the Mac, first verify its public egress differs from the Pi VPN exit; then make a 5s TCP `socket.create_connection((exit_ip,port),timeout=5)` attempt. Do not print credentials/use a third-party port-checking service. A successful local qBit preferences read is not external reachability; record actual success/failure and test vantage.

On the Pi, run the normal 15 service smoke checks and full box assessment, with an outer bound and saved sanitized output:

```sh
timeout 600 /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/verify_services.sh
timeout 600 /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/verify_box.sh --with-services
```

Retain the exact check count, failures and warnings; don't equate active systemd units with application readiness. Confirm native Box Health and Content Manager report the fresh snapshot, old legacy logs cannot override it, cached local playback remains available, fallback selector is usable, and no credential appears in diagnostics.

Observe **at least 30 uninterrupted minutes after final cleanup and Ready** with finite sampling (e.g. 36 read-only samples at 50s intervals, or equivalent). Keep the existing supported controller/health reporting active; create no new recurring job. Record start/end UTC, fresh Ready/traffic/port/qBit/dependents, desired/effective countries, actual recovery/restart counts and min/max port, kiosk PID/NRestarts, system resource symptoms, and final content/watch identity inventory. A new fault resets the next action: investigate and restore before starting a new full soak; never shorten the observation to fit a completion claim.

## Rollback and cleanup

If rollback is required, stop the **watcher only** first; use the supported maintenance owner for code replacement and preserve explicit stack-stop intent. Restore this deployment's fresh secured code/config/unit backup, then run the restored `converge_units_after_restore`/common delivery path so no unit references a removed package. Use the lifetime code-only privileged owner bridge for OTA rollback; do not elevate the original unprivileged updater. Restore the current policy/auth coherently, not the original Netherlands snapshot by accident. Preserve new user data, watch progress, all recovery backups and untouched media.

A restored services tree may require a supported Gluetun reload to load its matching auth; it must still honor operator stop, compose locking and pause ownership. Require the same Ready/services/external-port/preservation checks for a running rollback. Keep the kiosk PID continuous unless separately reviewed C++ work makes that impossible.

Remove only disposable container/fixture and ephemeral staged tests/Bats created by this run. Do not remove `.superpowers/sdd` evidence, original backups, supported watcher/unit/status files, media or user state. Commit the live result ledger locally and update recovery memory after validation, with remaining Pi 4 gate explicit.

## Owner result ledger (magicpi5, 2026-11-15; final controller `004c2bf`)

| Gate | UTC / evidence / result |
|---|---|
| Astra review and local pi5-test integration | Whole-branch review plus scoped fix review approved through code commit `2d68205`; local `pi5-test` fast-forward integration complete; no deleted paths or C++ source changes. Live-validation fixes after that each got an independent scoped review (see below). |
| Linux root/unprivileged identity + affected Bash 5 tests | **Pass.** Root-boundary identity 1/1, playback ownership 18/18, affected Bats 61/61 on the Pi (Codex). Package suite re-run on the Pi as `magic` after every later fix (195 → 249 tests, 1 opt-in skip each time). Do **not** run `test_gluetun_cascade.py` on a box before `10f2719` (it wrote the real pause marker). |
| Cached pinned disposable contract and exact cleanup | **Pass.** 28/28 assertions, cleanup removed (Codex, `656d290`); re-run after the socket-runner/nsenter change and after the attestation change: 28/28 each, no leftover container or fixture. Network capability gate 10/10 after the `dport_mask` helper fix. |
| Fresh secure backup/baseline and scoped dry-run | **Pass.** `/var/backups/mdb-vpn-reliability-20261115T090842Z` (Codex), plus root-0700 `before-*` subdirectories for every later module swap (`before-flap-fix`, `-attest-fix`, `-round-fix`, `-attempted-fix`, `-selection-reset`); each swap hash-verified, mode/owner preserved, watcher quiesced under `run-inhibited` and resumed with a verified fingerprint. |
| Delivery, auth, desired Canada + US, kiosk continuity | **Pass.** Activation + Content Manager Canada/US policy (Codex). Kiosk PID 308949, NRestarts 0 for the whole session; no C++ rebuild. |
| qBit-only / sustained port loss / first lease / selected-server fallback | wrong qBit port **pass** (3.31 s, Codex); sustained port loss **pass** (observed, loss→action 100.6 s, rotation + qBit readback, Ready 22 s later); first lease **pass** (observed, 130.5 s); fallback: **demonstrated, not formally proven** — run 4 (after the flap fix) went CA→CA→US with loss→action 68 s / ~66 s, but `us-119` never leased inside the window; run 5 (final code) went CA→CA in 60 s, then Gluetun wedged on `ca-59` (its own sub-second startup-check restart loop, no fault of ours) and the `us-119` PUT hung for 2 min in Gluetun's handler, so the controller escalated to its one container restart and recovered on Canada `ca-51` in ~2 min; the helper itself crashed that run. No single run produced the helper's "observed" verdict. |
| Explicit stop / maintenance / playback preservation | Explicit stop **pass** (30.02 s/11 samples, no resurrection, Codex). Playback pause exercise not run (owner playback active); SSD unplug deferred. |
| External TCP port and 15 service checks | **Pass.** Mac (different egress) → VPN exit `139.28.218.23:34160` connected in 197 ms; qBit listens in the VPN namespace only. `verify_services.sh` 15/15 (262 live results); `verify_box.sh --with-services` 38 pass / 0 fail / 4 warn (3 aged-out kiosk boot log lines, no Dreamcast BIOS), SHIPPABLE, native VPN Ready check PASS. |
| Full 30min steady-state interval and final inventories | soak-1 **pass**: 184/184 samples Ready over 1861 s, one generation, port steady, kiosk unchanged, 0 Gluetun VPN restarts, controller 3.68 % of a core, RSS 20–35 MB, stale round closed live at 1794 s. Final soak (soak-2, Canada `ca-27`) **pass**: 184/184 Ready over 1861 s, one generation, port 34160, 0 Gluetun restarts, controller 3.65 % of a core, RSS 17.4–17.8 MB, round closed at 1774 s. Final inventories vs. the pre-test baseline: media, playlists, ROMs, SSD library/downloads, protected settings/pairing files, watch history, firewall DROP and unpublished control port unchanged; only the watcher PID differs. |
| Rollback rehearsal/results; Pi 4 release gate | Not exercised. Per-module backups retained on the Pi. **Pi 4B hardware validation remains a release gate.** |

## Local verification checkpoint — 2026-11-14

All six task gates and the fresh Astra whole-branch/scoped fix review are complete. I1–I4 and M1/M3 are closed; the broad `Controller.tick()` readability refactor (M2) is explicitly deferred. Reviewed code head: `2d68205`. No implementation changes have been deployed to magicpi5.

Root verified the current checkout: 195 package tests in9.973s (194 passed,1 explicit Linux-root identity skip);28 cascade tests passed;77 affected web tests passed with29 known Mac missing-evdev/uinput warnings;46 affected Bats cases (45 passed,1 known missing-flock skip);1 Node UI test passed. The first simultaneous package/cascade/web/Bats run hit the setup fixture's8-second startup deadline in one test while cascade duration rose from about9s to29s. Both setup cases then passed alone in3.727s, and the full isolated package run passed. No code was changed to mask the timing failure; retain this concurrency limitation when running process-heavy validation on the Mac.

Hardware access became unavailable during a read-only check: the Mac moved from the home LAN to en0 `172.20.10.14`, gateway `172.20.10.1`; SSH/web at`10.0.0.227` and mDNS are unreachable. The owner was asked to reconnect or leave Pi checks for later. Last successful read-only sample (epoch1794693858) showed the same Canada container/start, fresh TLS traffic, forwarded port60906=qBit, and kioskPID308949/NRestarts0. This is baseline evidence, not validation of the new code.

Linux identity/flock and pinned-image contracts, fresh backup/dry-run, activation/deployment, real fault/port/country recovery, external TCP, actual browser/preservation checks and the full30-minute soak remain **pending** until the Mac can reach the Pi. No source credentials, content, services, or network configuration were changed on the Pi during implementation. The earlier manual Netherlands→Canada repair remains separate. Do not substitute the old preparation snapshot for the required fresh baseline; real settings are under `/opt/magic_dingus_box/config/`.

## Live validation, continued — 2026-11-15 (after the Codex hand-off)

Resumed at ~01:50 PST from the SDD ledger with magicpi5 Ready on `1a06e20`. The live fault rows found six controller defects that the deterministic suites could not; each was reproduced in a failing test first, fixed, independently reviewed, re-run on the Pi's Linux suite and deployed with the guarded module swap above.

| Live observation | Fix |
|---|---|
| Fallback row #1: Gluetun restart-loops a blocked endpoint every ~6 s inside the same container (tun0 ifindex 4→30 in 150 s); every new tun0 reset the loss clocks and re-armed the 120 s candidate grace, so the controller sat in "Waiting for candidate" forever. | `0c766f8`: only a new container start resets clocks/grace; ≤30 s unknown samples keep loss evidence; a missing tun0 within 30 s of one seen waits a tick instead of spending the container restart; rotation PUT fenced on container identity; an unsent deferred rotation retries after 20 s. Also restores the contract replay test `3ddabdd` left failing. |
| Fault helper preflight: the startup INFO line had scrolled past `--tail 1000` (1199 lines after one restart loop). | `af50e24`: attest from the 121 s startup window (`--since`/`--until`; Docker 26 returns nothing for `--tail` with `--until`). |
| `round_attempts` from a recovered incident persisted, so the next unrelated failure would skip Canada. | `2487f14`: a round closes after 30 min continuous Ready (closed live at 1794 s in soak-1); shorter success keeps it. |
| After a container restart reverted to Canada, healthy `node-ca-43` (port 43857) was rotated away because `attempted` still named the US host PUT before the restart; and `0c766f8`'s issued-rotation reset stamped a future `started`, wiping policy state. | `13129fd`: drop `attempted` on container change; `_complete` uses the tick's clock. One sanitized journal line per issued action. |
| First-lease row: "3 losses in 600 s" fired 6 s after an attended PUT, counting losses from two previous servers. | `3d827bf`: losses are per server (cleared on issued rotation); any intent-revision change (maintenance/lifecycle only) re-evaluates the current selection with first-lease grace. |
| During `verify_services` the 1.3 s in-tunnel probe (650–750 ms idle) failed for 60 s while Docker's 3 s in-tunnel healthcheck passed; the healthy server was recorded failed (the rotation happened not to be issued). | `70d5652` + `004c2bf`: for a tunnel generation our own probe already verified, a Docker healthcheck success ≤45 s old, after that proof and not followed by a failure, counts as working traffic. Candidates never borrow it. |

Other changes in the same pass: `33644c8`/`7e4a944` serve the controller's fixed `docker inspect`/`docker ps` identity queries from the Engine socket in-process (stdout verified byte-identical to the CLI on the Pi) and read tun0 via `nsenter`; controller CPU on the Pi 5 fell from 10.7 % to 3.7–4.0 % of a core, containerd from 4.6 % to 1.5 %, dockerd from 1.5 % to 0.6 % (import RSS +0.16 MB). `10f2719`: the cascade suite no longer touches the real pause marker (a run on the Pi left a fixture marker that was removed by hand; no container was affected). `e0ebfaa`: first boot no longer wipes the watcher's tmpfs runtime dir. Fault helper (scratch then; in the repo since 2026-11-15 as `tests/vpn_fault/check_network_fault.py`, never deployed): accepts iproute2's exact `dport_mask`, records the controller's action before voiding an interval on a mid-rotation unknown sample, and lets the first-lease traffic check settle for up to 15 s.

Gluetun's own restarts stopped as soon as each fault was removed (0 in the following 30 min). Evidence (sanitized JSONL, logs, baselines): `/private/tmp/mdb-vpn-reliability-20261114/` on the Mac; per-row evidence directories under `/run/mdb-vpn-network-*` on the Pi until reboot.

Final state (04:08 PST): Ready on Canada `node-ca-51` (desired country), forwarded port 52371 = qBit, kiosk PID 308949 / NRestarts 0, deployed controller = `004c2bf` (fingerprint `d266169b…`). Gluetun v3.41.3 can wedge its VPN loop after a sub-second startup-check restart loop (settings PUT blocked 2 min, shutdown timed out); the controller's api_failed → single restart path is what recovers it, so keep that escalation.


## Live validation — 2026-11-15 afternoon (fallback optimizations, PR #24)

Deployed to magicpi5 as module swaps under `run-inhibited` (backups
`/var/backups/mdb-vpn-reliability-20261115T090842Z/before-diversity-deadcandidate`
and `before-abandoned-escalation`): network-block candidate diversity, the
dead-candidate rule, and the abandoned-rotation escalation. Faults came from
the in-repo helper `tests/vpn_fault/check_network_fault.py`.

| Run (box time) | Mode | Result | What it showed |
|---|---|---|---|
| 12:17 | fallback | unproven (namespace changed) | Diversity: from `node-ca-51` (185.111.110.x) the next candidate skipped `ca-50`/`ca-43` (same block) for `node-ca-59` (84.20.16.x). Dead-candidate rule: `ca-59` judged **69 s** after the rotation (was 120 s+). Fallback reached `node-us-119`, which again never leased a port; after its 120 s first-lease grace the controller correctly rotated to `node-us-390`. That settings PUT hung **2 min** in Gluetun v3.41.3 (`200 PUT /v1/vpn/settings … in 2m0.5s`, then "Shutdown failed: … http server: goroutine shutdown timed out"). The controller had marked `api_failed` but waited out the rest of the grace before its one restart, which led to the abandoned-rotation escalation (`7ca0798`). The restart replaced the pinned namespace, so the helper could not prove the US leg. Its 550 s watchdog then crashed on `/proc/<old pid>/stat` and left a failed unit (fixed in `4231578`; no rules remained). |
| 12:58 | fallback | unproven (`unexpected_endpoint_do_not_chase`) | After the 12:23 container restart, Gluetun ran its `.env` selection (any Canada server). When `node-ca-14` was blocked, Gluetun's own healthcheck hopped to `node-ca-51` in ~45 s, before the controller acted. That is good recovery, but it bypasses the path under test: the controller's two-Canada-then-US logic only engages on a controller-pinned selection. |
| 13:00 | port-loss | **pass** (observed) | Rotation 90.07 s after the port loss (threshold 90), `ca-51` → `ca-59`, qBit synced, Ready. This also pinned `ca-59` for the next run. |
| 13:33 | fallback | unproven (namespace changed) | Started from the pinned `node-ca-59`. Diversity again: the next Canada candidate was `node-ca-24` (176.113.74.x), not a 84.20.16.x neighbour. Dead-candidate rule judged `ca-24` after **80 s**, and the two-Canada round moved to the fallback country: `node-us-119`, which again never leased a port. After its first-lease grace (131 s) the controller rotated to `node-us-390`; that settings PUT hung in Gluetun again, and the new abandoned-rotation escalation restarted the container **66 s** after the rotation (was the rest of the grace). Box Ready on `node-ca-37` afterwards, kiosk NRestarts 0, 0 failed units; the fixed helper cleaned up without crashing. The restart replaced the pinned namespace, so the US leg is still not proven. |
| 15:02 | port-loss | **pass** (observed) | First run on the merged PR #24 controller (deployed 14:58 as a module swap of `catalog.py` + `runtime.py`, backup `before-repeat-failures`; watcher PID 1722206; box tree = main `19c23c6`; `verify_box --with-services` 42/0/1 SHIPPABLE beforehand). Rotation **90.09 s** after the port loss, `node-ca-37` → `node-ca-59`, port 61774, qBit synced, cleanup exact, kiosk PID/NRestarts unchanged. |
| 15:53 | fallback | unproven (`namespace_identity_unavailable`) | From the pinned `node-ca-59` after the round closed at 15:33. `ca-59` → `node-ca-24` (176.113.74.x, another block) at 15:55:25; dead-candidate rule → `node-us-119` **82 s** later; no port lease again → `node-us-390` at 15:59:05 (138 s); that settings PUT hung, and the abandoned-rotation escalation restarted Gluetun **60 s** after the rotation (16:00:05). Ready on `node-ca-24` by 16:01 (port 55125, qBit synced), kiosk untouched, cleanup exact. Found: `us-119`'s count went 1 → 3 in this one incident. The failed PUT left it as `attempted`, so the rotation away and the escalation restart both recorded it. Fixed: a record inside the previous one's 30-min exclusion no longer counts again. Its stored 3 happens to match reality (it failed on three separate runs today), so the next run tries it last. |
| 16:51 | port-loss | **pass** (observed) | `node-ca-24` → `node-ca-60`, rotation **100.03 s** after the port loss (threshold 90, 10 s observation cadence); port 58116 forwarded 7 s after the settings PUT. Pinned `ca-60` for the next run. |
| 17:24 | fallback | unproven (`namespace_identity_unavailable`) | Repeat rule worked: `us-119` (count 3) was skipped. `ca-60` → `ca-27` (17:26:25) → `node-us-390` (17:27:47) → no lease → `node-us-69` at 17:29:55 → PUT hang → escalation restart 17:30:55 → Ready on `node-ca-14` by 17:31. Gluetun's log shows why no US server leases (see Finding below). |
| 18:58 | fallback | **PASS** (observed) | First run with Gluetun's own health restart off (PR #28: compose entrypoint wrapper, applied 17:54 via `recreate_gluetun.sh`; Gluetun logged "Restart VPN on healthcheck failure: no"). Unpinned `.env` selection, no port-loss pin needed. `node-ca-44` blocked → controller acted **60.09 s** later → `node-ca-37` (blocked) → dead-candidate rule acted **67.49 s** later → `node-us-391` (the projected fallback): settings PUT 370 ms, **port 53357 forwarded 7 s later**, qBit synced, dependents OK, Ready ~2.5 min after the first fault. Gluetun: 0 "operation was canceled", 3 VPN starts in the whole run (one per switch), same process throughout (no container restart). Kiosk PID 1907452 / NRestarts 0, 0 failed units, cleanup exact. The box stays on the healthy fallback by design. |

**Finding (17:24 run, confirmed against 15:53; fixed by PR #28 and proven by the 18:58 run): the US servers were never the problem.** Right after the controller's settings PUT to `us-390`, Gluetun went into a sub-second restart loop: 236 VPN restarts in 72 s, each "startup check: … lookup cloudflare.com: operation was canceled". Then its VPN loop stopped, port forwarding never started, and the next PUT hung ~2 min. The 15:53 switch to `us-119` shows the same signature (19 restarts, all "operation was canceled", no port forwarding). The switch away from a server whose traffic still works (port-loss, 16:54 → `ca-60`) is clean: port forwarded 7 s after the PUT. So the race is: **a settings PUT that lands while Gluetun is restart-looping on a dead server (its own 6 s healthcheck cycle) wedges Gluetun v3.41.3**. It happened on all four fallback runs today; recovery always came from the abandoned-rotation escalation restart, back on the `.env` country (Canada). The allowed fallback country did not engage in any of them. The repeat-failure counts on `us-119`/`us-390` are artifacts of this; the rule is harmless (back of the pool, never excluded) and stays for genuinely flaky servers. Open: a rotation path that does not race Gluetun's restart loop (the controller deliberately sends one settings PUT, with no stop/start gap).

Lessons (also in CLAUDE.md and `tests/vpn_fault/README.md`):
- Gluetun self-hops within its country under the `.env` selection. A fallback proof needs a controller-pinned server: run port-loss first.
- A rotation records two attempts in the round (the server left and the one chosen), so wait for the round to close (30 min continuous Ready, restarted by any watcher restart) before the next fallback run.
- (Superseded by the Finding above.) `node-us-119` failed to lease on every fallback run that reached the US, and PR #24 added a repeat-failure count so a server like it goes to the back of its pool. The no-lease turned out to be Gluetun's wedge, not the server.
- Proven at 18:58 after the fix: with Gluetun's own health restart off (one restart authority), the controller's two-Canada-then-US path lands on a US server that becomes Ready with traffic and port in sync.
