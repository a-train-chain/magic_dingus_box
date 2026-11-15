# VPN live-fault tests

Attended, opt-in tools that prove the native VPN recovery controller
(`magic_dingus_box_cpp/scripts/vpn_recovery/`) on a real box. They inject
faults that deliberately interrupt the VPN, **so run them only with the
owner's go-ahead**, and never while anyone is downloading something they
care about.

Nothing here is deployed: `deploy_cpp.sh` and the OTA copy only
`magic_dingus_box_cpp/`, `magic_dingus_box/web/`, `scripts/golden_image/`,
`services/` and `systemd/`. The unit tests are mock-only (no Docker,
network or namespaces). They run in CI (`test-local.yml` "Run VPN
fault-helper tests") and in `tests/run_offline.sh --suite vpn-fault`.

| File | What |
|---|---|
| `check_network_fault.py` | Injects one bounded fault inside Gluetun's network namespace and records whether the controller recovers the way its policy says. |
| `test_network_fault.py` | Mock-only unit tests for the helper's ownership, cleanup and timing rules. |
| `soak.py` | Read-only 30–60 min sampler of the controller's status, CPU and generations after a deploy. |

## Safety rails in `check_network_fault.py`

- **Who and where:** Linux root only. Live modes also refuse any host not
  named `magicpi5` (the owner's box). `--opt-in-network-fault` and an
  explicit `--mode` are both required.
- **What it touches:** only rules and routes it owns (tagged table 20001)
  inside the namespace of the exact Gluetun process it pinned, plus a
  root-private manifest under `/run/mdb-vpn-network-<id>/`. It never
  changes a file in the installation, credentials, the desired VPN policy,
  torrent state or the kiosk service.
- **How long:** a transient systemd timer removes the fault even if the
  helper dies:
  - `port-loss`: 240 s
  - `fallback`: 600 s
  - other modes: 360 s

  `--cleanup <manifest>` removes it by hand.
- **Prerequisites:** a Ready box, an attested Gluetun catalog, verified
  traffic both in the tunnel and on the host WAN, and (for `fallback`) the
  policy Canada → United States and a **controller-pinned** server. After
  any Gluetun container start, the selection is the `.env` country, and
  Gluetun's own healthcheck hops to another server within ~45 s of a
  fault, before the controller acts: good recovery, but it bypasses the
  path under test. Run `port-loss` first: its rotation pins a server.
  That rotation records two Canada attempts in the controller's round
  (the server it left and the one it chose), so wait for the round to
  close (30 min of continuous Ready, restarted by any watcher restart)
  before `fallback`, or its preflight refuses with
  `history_does_not_allow_distinct_second_Canada`. Don't restart Gluetun
  in between, or the pin is lost.

## Modes

| Mode | Fault | A pass means |
|---|---|---|
| `capability` | none: checks that the kernel and iproute2 can express the owned rules | the box can run the other modes |
| `port-loss` | blocks NAT-PMP to the gateway while traffic keeps working | the controller rotates after ≥ 90 s of lost port, and the new server is Ready with qBittorrent's port in sync |
| `first-lease` | a new same-country server that never gets a first port lease | the controller waits out the 120 s first-lease grace, then moves on |
| `fallback` | blocks the current Canada server, then the second Canada server it picks | the controller tries two Canada servers, then lands on an allowed US server that becomes Ready with traffic and port in sync. Any US server counts: the projected first one may never lease, and moving past it is correct. |

## Running one

From the Mac, with the box quiet (no game, no movie, nothing downloading
you care about). Stage a root-owned copy and check its hash:

```bash
scp tests/vpn_fault/check_network_fault.py magic@10.0.0.227:/tmp/cnf.py
ssh magic@10.0.0.227 'sudo install -d -m 0700 /run/mdb-vpn-attended && \
  sudo install -o root -g root -m 0600 /tmp/cnf.py /run/mdb-vpn-attended/check_network_fault.py && \
  rm -f /tmp/cnf.py && sudo sha256sum /run/mdb-vpn-attended/check_network_fault.py'
```

Then run one mode (the output is one JSON object per event; the last line
is the result):

```bash
ssh magic@10.0.0.227 'sudo python3 /run/mdb-vpn-attended/check_network_fault.py --opt-in-network-fault --mode fallback'
```

The other modes:

```bash
ssh magic@10.0.0.227 'sudo python3 /run/mdb-vpn-attended/check_network_fault.py --opt-in-network-fault --mode port-loss --gateway 10.2.0.1'
```

```bash
ssh magic@10.0.0.227 'sudo python3 /run/mdb-vpn-attended/check_network_fault.py --opt-in-network-fault --mode first-lease --gateway 10.2.0.1'
```

`--gateway` is Proton's NAT-PMP gateway (`10.2.0.1`), never inferred.

**Reading the result:**
- **Pass:** `"ok": true, "validity": "observed"`. The result also carries
  `controller_loss_to_action_s` and, for `fallback`, `fallback_reached`
  and `projected_fallback`.
- **Unproven:** `ok: false` with `validity: unproven` means the fault was
  removed before the controller showed the behaviour. It is not proof of a
  bug. Read `reason` and the evidence directory named in the result
  (`/run/mdb-vpn-network-*`, kept until reboot).

## Results so far

The record is `docs/superpowers/validation/2026-11-14-vpn-reliability.md`;
the release status is in `2026-11-15-release-1.12.0-readiness.md` (row 17.2).
