# VPN and port-forwarding reliability

Date: 2026-11-14. Status: proposed; awaiting owner approval.

## Intent and evidence

The owner wants the box to stay connected to Proton with working port
forwarding, and to recover automatically when a server stops providing it.
No design can guarantee uptime through an ISP, power, account, or provider
outage. Success means detecting failures promptly, trying viable alternatives,
keeping traffic protected, and reporting recovery honestly.

On magicpi5 today, Netherlands connections repeatedly failed DNS/TLS startup
checks or lost NAT-PMP leases with gateway connection-refused errors. A stale
2025-11-18 server catalog compounded the selection problem. Refreshing the
catalog alone did not fix the Netherlands pool. Changing to Canada did:
real HTTPS, forwarded port 60906, qBittorrent port readback and uploads,
an external TCP connection to that port, and all 15 service checks passed.
The same container, exit and port were healthy again over 30 minutes later.
This establishes the observed failure and recovery, not Proton's internal cause.

The current code has five relevant gaps:

1. Compose waits five failed minute checks; the cascade watcher then waits
   another five minutes before restarting. It has no bad-server memory.
2. The `.pf_seen` latch deliberately accepts a fresh session that has never
   received a port. That avoids a startup deadlock but can hide indefinite
   missing port forwarding.
3. The serial Docker event reader sleeps during confirmation and recovery.
   Its periodic task converges dependents only while Gluetun is healthy.
4. Port synchronization runs once a minute and reports a write as successful
   without checking the HTTP result and reading the preference back.
5. The pinned Gluetun image does not imply a current provider catalog; no
   catalog refresh is configured. Radarr responding is also insufficient
   evidence that the tunnel and forwarding are ready.

## Approaches

- **Configuration only:** add countries and reduce timeouts. Small, but it
  cannot reliably avoid failed servers or distinguish a port-sync fault from
  a tunnel fault. Repeated container restarts disrupt all five dependents.
- **Coordinated recovery (recommended):** retain Gluetun and its firewall;
  add one host-side recovery state machine with verified port synchronization,
  server selection, bounded retries, and catalog maintenance. More work, but
  directly addresses the observed gaps without replacing the VPN stack.
- A second VPN stack/provider would cost more resources and introduce
  routing and credential complexity; it is outside this change.

The agent recommends the second approach based on the owner's reliability
goal. The owner has not yet approved the overall design. The owner confirmed
the country policy on 2026-11-14: try another Canadian server first, then
fall back to the United States, with the VPN firewall active throughout.
No other country is allowed unless the owner changes that preference.

## Scope and constraints

Native Proton WireGuard with port forwarding enabled gets automatic server
selection. Custom providers and intentionally disabled forwarding keep their
existing behavior; unsupported combinations must be identified explicitly.
Country preferences are per box: existing installations inherit their current
configured country and have no cross-country fallback unless configured.
Canada then United States is approved for magicpi5 only; it must not silently
replace other owners' settings in a shared image/update.
Keep the current pinned Gluetun version unless implementation discovers a
documented blocker requiring a separately reviewed pin change.

Use existing Python 3, systemd, Docker and HTTP tooling. No additional
container, package, Proton account password, or render-loop work. Must work
on Pi 4B and Pi 5 within the Pi 4 resource budget. Local movie playback and
the kiosk remain independent of recovery. No automatic key rotation or
editing of downloaded content, playlists, watch state or SSD mounts.

## One recovery owner

Replace the cascade watcher's VPN restart decision with one bounded polling
state machine, rather than adding a competing watchdog. Preserve its tested
dependent-relink/convergence behavior and playback-pause awareness. Docker
events may wake reconciliation, but lost events and restarts of the watcher
must be recoverable from current state alone. Never sleep for several minutes
inside an event handler.

Separate observations from mutations. The worker has a single-instance lock
and takes the existing read-open compose lock before any tunnel mutation or
container operation. Every subprocess, HTTP call and lock wait has a bound.
If the lock is busy/unusable, defer; never proceed unlocked. Recheck container
generation, operator intent, settings revision and playback marker after
acquiring it. Do not hold it while waiting for recovery health.

Intentional setup/reset/OTA/country changes must inhibit automatic recovery
through the existing maintenance intent plus explicit integration with those
entry points. Persist managed-stack start/stop intent through supported
systemd and Content Manager start/stop/reset/setup/restart paths, and clone
preparation. An explicit stop remains stopped. Record a direct Docker stop as
an operator override when observed. After missed events, an ambiguous exited
container stays stopped until supported start/resume. Missing/corrupt intent
also cannot authorize resurrection. Recover an exited container only with
enabled intent AND affirmative failure evidence, such as Docker's OOMKilled
flag or a recorded autonomous failure transition. Nonzero exit status alone
is insufficient: an intentional stop that times out can end in exit 137.
Treat an unreachable Docker daemon as an observation failure, not permission
to mutate. This conservative edge policy is an agent design choice to avoid
undoing operator actions; healthy running containers still get normal recovery.

## Health contract and timings

Check port state and qBit alignment every 10 seconds; check actual DNS/TCP/TLS
egress every 30 seconds with bounded requests. Use an independent HTTPS
fallback when the primary endpoint fails, so one probe site's outage cannot
force server rotation. A cached public-IP response never proves egress.

Publish separate current observations:

- tunnel traffic verified;
- forwarded port present for the current tunnel/container generation;
- qBit readback matches that port, with UPnP/random-port disabled;
- dependent namespace/service reachability;
- last successful verification, last recovery reason and next retry.

`Ready` requires fresh traffic, a valid nonzero port and verified qBit
alignment. On boot allow up to 120 seconds to obtain the first port, then
enter recovery if it is still absent. Never treat “never had a port” as
permanently ready. Initial dependents may still start once the protected
tunnel passes traffic: separating Docker startup eligibility from full
readiness avoids the old qBit/startup dependency deadlock.

Builder-selected initial thresholds (subject to measured test adjustment):

- A single failed observation updates diagnostics but does not reset anything.
- If only qBit's port is wrong, correct it promptly, check the HTTP result,
  then read back both qBit and Gluetun state. Reject a stale result if the
  generation or port changed during the operation. Retry a local sync error;
  it must not cause a VPN server change.
- Missing forwarding for 90 continuous seconds, or three losses within ten
  minutes, makes the current server a recovery candidate. Brief Gluetun
  renewal retries are allowed to finish first.
- Confirm loss of tunnel traffic for 60 seconds before a controlled action;
  first recheck whether Gluetun's own recovery has already changed the server.
- After choosing a server, allow up to 120 seconds for traffic plus forwarding.
  Aim to start useful recovery within two minutes and restore readiness within
  roughly three minutes when a healthy alternative exists. These are testable
  targets, not promises about a provider outage.

Missing forwarding does not turn off VPN protection. If the encrypted tunnel
still works, outgoing transfers may continue during repair; the state is
`Recovering port forwarding`, not `Ready`. Do not add a second torrent
pause/resume owner that could fight the SSD or playback guards.

## Recovery and server selection

Let Gluetun own WireGuard and NAT-PMP protocol retries. Prefer a controlled
tunnel/server change through its authenticated control API while preserving
the container and network namespace. The pinned v3.41.3 source supports
`PUT /v1/vpn/settings` and `PUT /v1/vpn/status`. Its port-forward handler is
GET-only despite PUT appearing in its auth route list: do not invent a
port-repair call from newer documentation. Confirm exact API behavior in
contract tests before depending on it.

For server changes select eligible WireGuard/P2P/forwarding entries from the
validated catalog. Try at most two distinct eligible servers in the preferred
country before trying the approved fallback country. Exclude recently failed
endpoints for 30 minutes, with a 24-hour cap on history. A short successful
connection must not erase a flapping server's penalty. Stop changing servers
as soon as one is stable; never interrupt a healthy fallback merely to return
to the preferred country. Keep desired preferences separate from effective
runtime selection; automatic recovery must not rewrite the user's `.env`.

Persist bounded failure history and last successful selection. Reconcile it
against the current catalog and settings after reboot; stale/corrupt state
must not strand the VPN. Do not apply this source box's state to cloned units.
If all eligible servers are penalized, retry the oldest eligible candidate
after backoff instead of banning every server forever. Back off progressively
(2, 5, then 10 minutes, capped) after a failed round; continue observing and
retry indefinitely at the cap. Avoid full-stack churn when internet, clock,
credentials, or the provider are unavailable. Explain persistent failure
without claiming to diagnose an invalid key from a generic timeout.

Escalate to one controlled Gluetun container restart only when the control
API is unresponsive, a container is broken, or a newly refreshed catalog must
be loaded during an already necessary recovery. Existing dependent relinking
runs only when the namespace/container changed. Bound disruptive restarts to
one per ten minutes after an unsuccessful attempt; polling continues. Do not
restart the kiosk. Preserve the pause marker and stop/resume ownership rules.

## Control API and port synchronization

Keep port 8000 unpublished. Install a per-box random credential with narrowly
scoped access to the required VPN settings/status routes. Keep read-only
public routes as today; never make mutation routes public to Byparr or other
shared-namespace processes. Store credentials with restrictive permissions;
never print settings responses, keys or auth headers. Cover provisioning,
OTA, rollback and cloning/reset of the new local credential.

Extract a bounded, reusable port-sync operation used by recovery and the
existing timer. Serialize port writes. Retain the timer's SSD guard on its
existing cadence, ahead of every port-related early return; neither a failed
VPN nor failed authentication may suppress independent VPN observations.
No credential changes or extra torrent starts/stops as a side effect.

## Catalog maintenance and diagnostics

Fetch the official Gluetun provider catalog daily with jitter and conditional
HTTP requests; this fetch is to Gluetun's published static data, not repeated
authenticated requests to Proton's API. A bounded host-side HTTPS fetch is
permitted during recovery too, so a broken VPN cannot prevent fixing a stale
server list. It fetches public metadata only, never application traffic.

Validate size, JSON schema/version for the pinned image, timestamps, endpoint
addresses/keys and nonempty eligible pools before atomically updating only the
Proton data. Preserve other providers, permissions and a known-good backup.
Never replace good data with empty, older, incompatible or malformed data.
Fetch failures keep the current list and report its age. A healthy tunnel is
not restarted just to load a new catalog. On recovery, explicitly reconcile
the controller's candidate list with the catalog actually loaded in Gluetun;
rejecting an unknown new hostname must not trigger an endless rotation loop.

Extend existing Box Health/Media Browser diagnostics with readiness, current
country/server, forwarding/sync state, catalog age, and recovery history.
`Waiting for internet`, `Recovering VPN`, `Recovering port forwarding`,
`Synchronizing port`, and `Ready` must reflect fresh observations. Keep local
cached movie playback available; do not equate “Radarr answers” with readiness.
This change does not require redesigning the kiosk or adding notifications.

## Verification and delivery

Test policy with a fake clock and recorded observations: first-lease failure,
port loss with working traffic, flapping, wrong qBit port, HTTP/auth failure,
generation change mid-sync, API refusal, all candidates failing, WAN outage,
clock jumps, stale/corrupt state, catalog failure, watcher restart, missing
events, intentional stop/reset, lock contention, and playback/SSD guards.

Exercise the exact pinned API and firewall in an isolated test environment
before failure injection on the owner's box. Prove that VPN-down blocks
internet traffic and unauthenticated mutation routes are denied. Verify
new port readback and external inbound TCP reachability. Test catalog changes
and an unavailable preferred server with a viable fallback. Include rollback
and provisioning/cloning paths. Run relevant Python/Bats suites and required
repository regressions; if C++ suites run, use Media Browser ON (9) and OFF
(8), one scratch build at a time, then remove the build.

After implementation/review, take backups and deploy to magicpi5 using the
Bluetooth-preserving local integration workflow and dry-run. Approval of this
design plus “build” includes brief controlled VPN reconnect tests, with the
kiosk kept running. It does not authorize deleting media, SSD unplug tests,
pushes, PRs, shared merges, or release publication. Perform the normal 15
service smoke checks and a longer steady-state soak with recorded results;
leave existing ongoing health reporting active, not an ad hoc forever job.
Pi 4B hardware validation remains a release gate.

## Sources checked

- Repository: `services/docker-compose.yml` under `magic_dingus_box_cpp`,
  `scripts/gluetun_cascade_restart.sh`, `scripts/qbit_port_sync.sh`,
  `scripts/ensure_gluetun_auth.sh`, `web/vpn_settings.py`, their tests and units.
- [Proton's manual forwarding guide](https://protonvpn.com/support/port-forwarding-manual-setup)
  describes renewable mappings and selecting another P2P server on failure.
- [Gluetun server maintenance](https://github.com/qdm12/gluetun-wiki/blob/main/setup/servers.md)
  documents stale built-in catalogs; current updater requires Proton credentials.
- Pinned v3.41.3 [VPN handler](https://github.com/qdm12/gluetun/blob/v3.41.3/internal/server/vpn.go),
  [port handler](https://github.com/qdm12/gluetun/blob/v3.41.3/internal/server/portforward.go),
  and [server selection](https://github.com/qdm12/gluetun/blob/v3.41.3/internal/configuration/settings/serverselection.go).

Implementation details such as module boundaries, storage JSON layout and
probe fallback hostname are left to the builder, subject to the contracts above.

Builder review: Astra reviewed the recovery/control/locking design and found
no remaining blocking planning questions after the operator-intent clarification.
Owner country preference is confirmed; overall design approval remains pending. Sol 6.1 is the
intended implementation model for most tasks; reserve Astra for the most
difficult recovery/concurrency work and final integration review.
