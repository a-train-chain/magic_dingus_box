# VPN Reliability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Automatically restore verified Proton VPN traffic and port forwarding with bounded Canada → US failover on magicpi5.

**Architecture:** One host Python controller replaces the native-Proton cascade restart decision, while Gluetun retains protocol/firewall ownership. Pure policy consumes bounded observations; adapters handle authenticated control, verified qBit port writes, and validated catalogs. Existing lifecycle entry points publish intent and inhibition; diagnostics consume a sanitized status snapshot.

**Tech Stack:** Python 3 standard library, existing Bash/Docker/systemd/HTTP tools, unittest/pytest/Bats; pinned Gluetun v3.41.3.

**Spec:** `docs/superpowers/specs/2026-11-14-vpn-reliability-design.md` (owner approved design, implementation, review and controlled magicpi5 tests).

## Global Constraints

- No push, PR, shared-branch merge, release, media deletion, or SSD unplug. Local commits and local pi5-test integration/deployment are authorized.
- No additional runtime container, package, Proton account password, or render-loop work. Both Pi 4B and Pi 5; Pi 4 resource envelope.
- Keep Gluetun v3.41.3 and the current firewall; control port 8000 stays unpublished. Never print credentials, complete settings responses or headers.
- Native Proton WireGuard + forwarding only for automatic server failover; other configurations retain current behavior. Other boxes keep their current country, with no cross-country fallback by default.
- magicpi5 policy: Canada first, then United States; at most two distinct preferred-country candidates before fallback. Desired settings and effective selection are separate; never rewrite .env automatically.
- Observe port/qBit every 10 s, traffic every 30 s; boot first-port grace 120 s; sustained port loss 90 s or 3 losses/600 s; traffic loss 60 s; candidate grace 120 s.
- Failure exclusion 1800 s, history cap 86400 s, failed-round backoff 120/300/600 s (cap), disruptive container restart at most once/600 s after failure. Healthy fallback stays put.
- Every external command/HTTP/lock wait bounded. Compose lock is `/run/lock/mdb-compose.lock`, opened read-only. Busy/unusable locks defer; no unlocked mutation. No lock held while waiting for health.
- Preserve playback-pause, SSD guard and torrent ownership. A missing forwarded port does not add a torrent pause/resume owner.
- Preserve explicit operator stop. Ambiguous exits and missing/corrupt intent cannot authorize resurrection; exit 137 alone is not evidence of a crash.
- Sol 6.1 implements/reviews most tasks; Astra handles Task 4 and final whole-branch review. Keep this plan's SDD ledger and evidence (owner's explicit ledger retention overrides workspace deletion).
- One scratch C++ build at a time if needed, Media Browser ON (9 suites) and OFF (8), remove the build afterwards. Do not duplicate dependency caches.

## Review Focus

1. A late HTTP success belongs to a replaced container or rotated port: reject stale writes/readiness (Tasks 1, 3, 4).
2. An operator stop ends in SIGKILL/137, or an event is lost across reboot: never infer restart permission from exit code (Tasks 4, 5).
3. Newly downloaded catalog and Gluetun's in-memory catalog differ: no endless unknown-hostname retry or healthy-tunnel interruption (Tasks 2, 4).
4. Recovery overlaps movie/game pause, SSD guard, setup or rollback: preserve intent/ownership and avoid lock inversion (Tasks 3–5).
5. Corrupt state, wall-clock jumps or a probe-site outage: bounded recovery remains live without unsafe resurrection or restart storms (Tasks 2, 4, 6).

## File map and execution contract

New package: `magic_dingus_box_cpp/scripts/vpn_recovery/` with `common.py`
(paths/config/atomic storage/locks), `control.py` (Gluetun transport),
`catalog.py` (validated provider data/candidates), `port_sync.py` (qBit
readback), `policy.py` (pure decisions), `runtime.py` (observations/actions),
`intent.py` (operator state), `cli.py` (entry points). Thin launcher:
`magic_dingus_box_cpp/scripts/vpn_recoveryctl.py`. Package scripts run from
the deployed tree; avoid a second installed copy of the package.

Persistent sanitized state: `/var/lib/magic-dingus/vpn_recovery/`.
Desired per-box policy: `/opt/magic_dingus_box/config/vpn_recovery.json`.
Control credential: `services/config/gluetun/auth/recovery.key` (0600),
with scoped role in adjacent `config.toml` (restrictive permissions).
Runtime locks/state use `/run/mdb-vpn-recovery/`. Constructor-injected paths
and transports are the test seams; production defaults never run in tests.

Tests live under `magic_dingus_box_cpp/scripts/tests/test_vpn_recovery_*.py`
and `tests/local/` for delivery/shell integration. Use stdlib unittest for
new package tests; existing web tests use their existing pytest environment.

Types are defined by their owning task below; later agents must read actual
earlier interfaces rather than create duplicates. If a precise interface
needs refinement, document it in the report and ledger before the next task.
User approved proceeding through the plan without another approval stop.

---

### Task 1: Protected Gluetun control and common I/O boundaries

**Model:** Sol 6.1, high.
**Files:** Create package `__init__.py`, `common.py`, `control.py`; modify
`scripts/ensure_gluetun_auth.sh`, `tests/local/gluetun_auth.bats`; create
`scripts/tests/test_vpn_recovery_control.py` and `test_vpn_recovery_common.py`.

**Interfaces:** `Paths` dataclass (base/services/config/state/runtime),
`load_config(paths: Paths) -> dict`, `atomic_json(path, value, mode=0o600)`,
`bounded_lock(path, timeout_s)` context manager (raises on unavailable),
`ensure_control_credentials(paths) -> bool` (changed),
`ControlClient(paths, runner).get(path, container_id) -> dict`,
`.put(path, payload, container_id) -> dict|str` (bounded, raises sanitized error).
Config returns provider/type/forwarding/preferred_countries/fallback_countries;
fallback defaults empty. Unknown fields/invalid types are rejected; preserve
current .env parsing behavior without executing shell content.

- [x] Add failing tests: `test_public_reads_private_mutations`, `test_key_mode_and_idempotency`, `test_http_error_is_not_success`, `test_timeout_is_bounded`, `test_generation_change_aborts_request`, `test_lock_busy_or_missing_never_runs_mutation`, `test_redaction`, `test_policy_defaults_preserve_country`, `test_invalid_config_fails_closed`. Assert no key/settings body in exceptions or logs and all existing public GET routes remain.
- [x] Run `python3 -m unittest discover -s magic_dingus_box_cpp/scripts/tests -p 'test_vpn_recovery_c*.py'` and targeted `bats tests/local/gluetun_auth.bats`; confirm the new cases fail before implementation.
- [x] Implement the listed interfaces. Control requests run from the host inside the inspected Gluetun network namespace using existing `nsenter` and Python stdlib (secrets through stdin, not argv); recheck container ID/PID/start identity. Allow only needed settings/status and public read routes; no general proxy. Use partial settings PUT; never log GET settings bodies. Credentials are random per box; preserve on rerun. Existing helper delegates installation to package when available and retains legacy compatibility otherwise. Do not activate/restart the live Pi.
- [x] Run those tests and existing auth/compose-pin tests; self-review against exact v3.41.3 source routes, including GET-only port handler. Record API assumptions for Task 6's pinned-container contract test.
- [x] Commit the bounded adapter and tests; write task report with exact interfaces, commands/results, limitations and commit.

### Task 2: Validated catalogs and server candidate selection

**Model:** Sol 6.1, high.
**Files:** Create `vpn_recovery/catalog.py`, `scripts/tests/test_vpn_recovery_catalog.py`, small public-data fixtures under `scripts/tests/fixtures/vpn_recovery/`.
**Interfaces:** `Candidate` dataclass (hostname, country, endpoint IP, public key);
`CatalogStore(paths, fetcher).load() -> catalog`, `.refresh(now, force=False) -> result`;
`eligible_candidates(catalog, countries) -> list[Candidate]`;
`select_candidate(candidates, history, preferred_countries, fallback_countries, now) -> Candidate|None`.
History and scheduling serialization remain plain bounded JSON consumed by Task 4.

- [x] Write failing tests for schema version 4, malformed keys/IPs, duplicate endpoints, non-forwarding/other protocols, empty preferred pools, older/future timestamps, size limit, 304, fetch timeout, partial write, stale writer and preserved other providers/ownership; test two distinct preferred choices before fallback, exclusions for 1800 s, expiry/history cap 86400 s and all-excluded behavior.
- [x] Run `python3 -m unittest discover -s magic_dingus_box_cpp/scripts/tests -p 'test_vpn_recovery_catalog.py'` and confirm failures.
- [x] Implement the public static provider fetch from `https://raw.githubusercontent.com/qdm12/gluetun-servers/main/pkg/servers/protonvpn.json`, limit 8 MiB, HTTPS timeout 15 s, timestamp future tolerance 24 h, and schema/endpoint validation. Merge only Proton into pinned single-file servers.json under the compose lock, with atomic known-good backup; preserve permissions/other providers. Fetch outside the lock, revalidate inside. Use ETag/Last-Modified and a daily due time with bounded jitter; no restart on update. Track downloaded versus loaded catalog identity for Task 4. Reject malformed state safely; no direct Proton account API call.
- [x] Run tests, including recorded provider fixture from the current public catalog. Ensure deterministic policy tests require no network. Document how newly downloaded hostnames remain unusable until Gluetun loads them.
- [x] Commit and report exact candidate/history/result shapes to the next task.

### Task 3: Verified qBittorrent port synchronization

**Model:** Sol 6.1, high.
**Files:** Create `vpn_recovery/port_sync.py`, `scripts/tests/test_vpn_recovery_port_sync.py`; modify `scripts/qbit_port_sync.sh` and related Bats tests, add `tests/local/vpn_port_sync.bats` as needed.
**Interfaces:** `PortSynchronizer(paths, control, qbit_transport).sync(expected_generation=None) -> dict` with `status`, `forwarded_port`, `qbit_port`, `generation`, `checked_at`, `error` (sanitized). Status is `synced`, `no_port`, `changed`, `unavailable`, `failed` or `deferred`. `.observe()` performs bounded reads without mutation.

- [x] Add failing tests: correct current port is no-op; wrong port writes then reads back; non-2xx/403/timeout/invalid JSON/mismatched readback never says synced; a changed generation/port before or after write invalidates result; UPnP/random port corrected; malformed ports rejected; concurrent writers defer; credentials never enter logs. Existing drive-guard tests must still prove it runs before no-port/already-synced paths and owns only its stopped torrents.
- [x] Run `python3 -m unittest discover -s magic_dingus_box_cpp/scripts/tests -p 'test_vpn_recovery_port_sync.py'` and the new Bats tests to see failures.
- [x] Implement stdlib cookie-authenticated qBit client with 5 s per HTTP request and bounded total sync. Serialize port writers; re-read current generation and Gluetun port after locking, verify HTTP status and qBit preferences readback, then recheck generation/port before success. Existing shell timer retains authentication/SSD guard cadence and uses the shared port operation afterward. Preserve legacy operation when the package is unavailable for rollback; no second full port-write implementation in the shipped new path. VPN observation is independent of qBit authentication.
- [x] Run targeted unit/Bats and drive-guard/password tests. Assert no start/stop torrent API calls from the new port synchronizer.
- [x] Commit and report the interface and test evidence.

### Task 4: One recovery controller, policy and operational intent

**Model:** Astra, high (hardest state/concurrency task).
**Files:** Create `vpn_recovery/policy.py`, `runtime.py`, `intent.py`, `cli.py`, `scripts/vpn_recoveryctl.py`, and `scripts/tests/test_vpn_recovery_policy.py`, `test_vpn_recovery_runtime.py`, `test_vpn_recovery_intent.py`. Modify `scripts/gluetun_cascade_restart.sh`, `systemd/gluetun-cascade-restart.service`, `services/docker-compose.yml`, `scripts/recreate_gluetun.sh` and their tests.

**Interfaces:** `decide(observation: dict, state: dict, config: dict, now_mono: float, now_wall: float) -> dict` returns `action`, `reason`, `next_state`, `next_due`; one mutation action per decision: `none`, `sync_port`, `rotate_server`, `restart_container`, `relink_dependents`, `converge_dependents`. `Controller(paths, adapters).tick()` observes, decides, revalidates and performs one bounded action. `set_intent(paths, enabled, reason)`, `inhibit(paths, token, owner_pid, reason)`, `release_inhibit(paths, token)`, `may_recover(...)` are shared with Task 5. CLI subcommands: `run`, `status`, `sync-port`, `catalog-refresh`, `intent enable|stop`, `inhibit begin|end`, `run-inhibited -- command...`.

- [x] Add fake-clock transition tests for every threshold in Global Constraints, including first lease never arriving, isolated loss versus 3 flaps, distinct country trials, healthy sticky fallback, candidate grace, progressive backoff, all-failed recycling, zero qBit-triggered rotations, stale traffic and generation, WAN outage, corrupt state, clock jumps and startup/watcher restart. Assert no action on one transient failure or stopped/ambiguous intent; nonzero exit/137 alone is insufficient.
- [x] Add orchestration tests with fake adapters for lost events, soft tunnel change preserving container, API refusal, stale catalog host, at most one disruptive restart/600 s, no nested compose locks, busy lock, settings revision changing while blocked, concurrent maintenance, playback pause/SSD guard, observed direct Docker stop and own-operation stop events. Run new tests to confirm failure.
- [x] Implement policy and runner using Tasks 1–3. Use monotonic durations in-process and bounded wall-time history across boots. Observe live generation using container ID/start plus WireGuard interface identity (Pi supports `ip -o link show tun0`; verify the exact pinned restart contract). Reconcile observed autonomous tunnel changes before mutating. Prefer authenticated partial server-selection PUT/controlled VPN restart; verify actual resulting selection and traffic/port, not just HTTP success. Match catalog candidates to loaded generation; refresh/load once during necessary recovery with rate limit.
- [x] Keep one controller process: native supported configuration routes the existing cascade unit/launcher into the new runner; legacy unsupported path remains. Extract sourceable bounded relink/converge helpers with lock ownership explicit; controller must not hold compose lock while calling a helper that acquires it. Replace the old forwarding latch healthcheck with tunnel-startup eligibility; full readiness comes from controller status. Preserve dependent pause enforcement and host endpoint checks. Do not let a parallel old watcher also restart Gluetun.
- [x] Persist managed intent and operation tokens with owner PID/start identity and generation. Missing/corrupt intent or ambiguous stopped container does not resurrect. Captured explicit stop overrides; affirmative failure (e.g. OOMKilled) plus enabled intent may recover. Expired/dead maintenance owners must reconcile to actual managed unit/config state, not inhibit forever. Fresh running installations may adopt existing running state without starting a stopped stack. Validate intent/settings immediately after acquiring compose lock. Wrap manual country recreation with intent/inhibit and remove unlocked fallback there.
- [x] Run all new unit tests plus cascade/recreate/convergence/compose-lock tests. Update old behavior tests to assert the new native path and preserve unsupported-provider behavior. Commit and report exact state/status/CLI contracts, environment migration needs and any decisions.

### Task 5: Delivery, lifecycle integration and truthful diagnostics

**Model:** Sol 6.1, high.
**Files:** Modify `scripts/setup_memory_tuning.sh`, `scripts/setup_services.sh`, `scripts/converge_system_units.sh`, `scripts/restart_stale_cascade_watcher.sh`, `systemd/magic-dingus-services.service`, `scripts/update.sh`, `scripts/verify_box.sh`; `scripts/golden_image/{prepare_for_cloning,restore_after_cloning,first_boot}.sh`; `magic_dingus_box/web/{admin_media_browser_ops,admin_media_browser,vpn_settings,diagnostics}.py`, `web/static/{manager_media_browser,vpn_settings}.js` where current cards need text; relevant unittest/pytest/Bats tests. Create small shared installer/status reader only if existing hooks cannot consume the CLI directly.

**Interfaces:** Consume Task 4 CLI/intents/status. Add sanitized `vpn_recovery` object to existing service-health response (freshness, state/reason, country/hostname, port/qBit match, last success/recovery, catalog age, next retry); stale (>45 s) status cannot say Ready. Existing endpoints remain compatible. Box Health calls CLI status in read-only mode.

- [x] Add failing integration tests covering install from old tree, rerun idempotence, module-only update reload, absent package rollback, fresh first boot, source clone prepare/restore and per-box key regeneration. Test systemd stop/start, web reset/setup/restart/country change, OTA failure/rollback and overlapping maintenance; no action during inhibit, no resurrect after explicit stop. Every new unit must enter convergence policy; prefer existing controller with internal refresh scheduling to avoid new timers.
- [x] Add diagnostic tests: old/no status and stale Ready are unknown; fresh states display accurate labels; custom/PF-off providers are not false failures; no secrets leak in health/diagnostic bundles; local cached movies stay available.
- [x] Run targeted test names to establish red. Reuse the existing test environment, or create one small scratch venv if absent; never install dependencies into system Python.
- [x] Integrate through the shared delivery hook so OLD update.sh delivers NEW behavior. Install auth before activation and coordinate the one necessary auth-loading restart. Preserve enable/stop intent and explicitly initialize already-running native boxes; don't start stopped/unconfigured boxes. Recovery package runs from deployed tree, so hash module content for watcher reload (not just wrapper mtime). All supported management paths establish inhibition before mutations and clear it safely after; no leaked token on process death. Clone artifacts contain no source recovery key/history/intent; source restoration retains source state safely.
- [x] Extend existing diagnostics without a layout redesign or extra notifications. Add per-box fallback policy support to existing VPN settings flow (default none; primary choice preserved, explicitly configured allowed fallback list); do not silently put Canada/US on other units. Config writes use shared revision/inhibition and atomic permissions-preserving operations.
- [x] Run targeted tests plus existing provisioning/convergence/clone/web VPN tests. Commit and report how to set magicpi5's approved Canada/US policy and how to roll back.

### Task 6: Failure-contract harness and full predeployment verification

**Model:** Sol 6.1, high.
**Files:** Create `scripts/tests/vpn_recovery_contract.py` (explicit opt-in integration harness), extend runtime policy tests for uncovered transitions, and create `docs/superpowers/validation/2026-11-14-vpn-reliability.md`; update `CLAUDE.md` VPN operational notes. No live Pi mutation from the agent; controller executes hardware commands after branch review.

**Interfaces:** Harness accepts explicit `--container NAME` and temporary fixture/auth paths; requires a recognizable disposable test-container name and refuses `mdb_gluetun` unless a separately explicit owner-test mode is supplied. It emits sanitized JSON assertions, never reads/prints production private keys, never spins up a second tunnel with the production key.

- [x] Add failing harness tests: reject production container/default target; dummy-key fixture has no real provider credentials; timeout/cleanup paths; no public control port; contract mismatch is a failure, not a skip. Replay a failure-to-success sequence through real controller adapters backed by a deterministic fake transport.
- [x] Implement a disposable pinned-Gluetun contract recipe using a dummy WireGuard key and unroutable test endpoint, the existing Pi image cache, no production ports or binds. Verify authenticated GET/partial PUT/status methods, unauthorized mutation denial, tunnel identity transitions, and firewall blocking outbound internet while the dummy VPN is down. Test recovery/failover state decisions with deterministic adapters; don't confuse fake tunnel success with a live VPN test.
- [x] Run all package tests, affected Bash/web suites and script syntax checks. Capture baseline-versus-new failures honestly. Test classifier's current-server observation against pinned source and available commands; resolve any contract mismatch with reviewed changes before deploying.
- [x] Write exact owner-box runbook: baseline/backups, dry-run deploy, auth loading, Canada/US setting, port-loss/first-lease/selected-server-failure tests, rollback and cleanup, external forwarded-port test, 15-service checks, and at least 30-minute steady-state observation. Owner approval covers brief VPN reconnects; kiosk remains running and SSD checks are deferred. Do not delete original recovery backups or user data.
- [x] Commit harness, regression tests and runbook; report full test evidence and remaining hardware steps. Fresh Astra whole-branch review follows before any local integration or deployment.

## Controller completion sequence (after all task gates)

1. Astra whole-branch review and required fix/re-review wave; retain ledger.
2. Merge the reviewed feature into local `pi5-test` only (already authorized by approved design's integration workflow). Confirm Bluetooth files and inspect exact rsync dry-run, including `--delete` effects; no pushes/PRs/shared merges.
3. Run disposable pinned-contract checks before production failure injection. Never use the same production WireGuard key in a second container. Remove only our disposable test container/files afterward.
4. Back up Pi code/config/auth/intent and baseline kiosk PID, data inventory, watch state, services, and current VPN. Deploy scoped Python/shell/web/systemd changes without rebuilding or restarting an unchanged C++ kiosk; use established rsync exclusions/dry-run and the common delivery hook. If C++ changes become necessary, use the approved normal deploy build and both suite modes first.
5. Apply magicpi5's approved Canada/US policy, run the runbook's bounded tests with guaranteed restoration, verify the firewall and externally reachable forwarded port, qBit alignment, all 15 smoke checks, kiosk continuity and content/state preservation. Observe at least 30 minutes; any new failure changes the next action, never hide it with a shortened soak.
6. Commit validation results locally, update recovery memory and report achieved behavior, test evidence and remaining Pi 4 release gate. No ad hoc recurring monitor/automation. Leave supported service health reporting active.

## Self-review

Spec sections map to Tasks 1 (control/protection), 2 (catalog/selection),
3 (verified port), 4 (single owner/policy/intent), 5 (lifecycle/diagnostics),
6 (contract/verification) and controller hardware completion. Review-focus
conditions are explicitly tested by their owning tasks. Interfaces are owned
once; common dict schemas must be reported between tasks. Existing C++ media
behavior is untouched; unsupported VPN configurations keep legacy behavior.
