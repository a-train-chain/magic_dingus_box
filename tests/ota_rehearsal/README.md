# OTA rehearsals

Two ways to prove that a box on an OLD release can take the NEW one over the
air — and come back — before anything is published.

| | `run.sh` (containers) | `hw_rehearsal.sh` (one real Pi) |
|---|---|---|
| Runs | arm64 Debian Trixie containers on the Mac | from the Mac, against `PI_HOST` |
| GitHub | fake, in the container | real (read-only) for the downgrade, fake **on the box** for the update |
| Catches | update.sh / web admin logic, data preservation, rollback | everything the containers cannot: systemd, PulseAudio, DRM, real units and drop-ins, real disk, the field-state gap |
| Cost | ~15 min, nothing at risk | ~40 min, the box is down to OLD and back |

Both build the NEW release with the **same shell the release workflow runs**
(`run.sh artifacts`, lifted out of `.github/workflows/release.yml`), so what
is rehearsed is what would ship. Nothing is ever published or tagged.

## Container rehearsal

```bash
tests/ota_rehearsal/run.sh                  # artifacts, image, all scenarios
tests/ota_rehearsal/run.sh scenario old_web # one scenario
```

See the header of `run.sh` and `box/scenario.sh` for the scenarios.

## Hardware rehearsal

```bash
# what would happen — no ssh, no rsync, no builds; every command printed
PI_HOST=magic@192.168.1.50 tests/ota_rehearsal/hw_rehearsal.sh --dry-run

# the real thing (prints hostname + model first; --yes is required)
PI_HOST=magic@192.168.1.50 tests/ota_rehearsal/hw_rehearsal.sh --old v1.9.14 --new HEAD --yes
```

`PI_HOST` is never defaulted: two boxes are usually reachable. Logs, the
snapshot copy and every command that ran land in
`~/mdb-ota-hw-rehearsal/<timestamp>_<host>/` (`OTA_HW_LOGROOT` overrides):
`run.log`, `results.txt` (PASS/FAIL/NOTE lines), `remote_commands.sh`,
`state/*.json`, `job_downgrade.log`, `verify_box_*.txt`, `audio_*.txt`.

**Before you start:** the box runs THIS checkout (deploy it first — the run
ends by redeploying it and diffing against the snapshot), the TV is on (a
kiosk with no display exits 69 and the OTA's start check treats that as a
one-time rollback), the kiosk sits on the main playlist menu, nothing is
downloading (an active torrent changes `/mnt/ssd` and fails the content
fingerprint), Docker is running on the Mac.

### Steps

1. **Preflight** — ssh reachable, passwordless sudo, box `VERSION` == this
   checkout, kiosk idle (`kiosk_status.json`: `screen == "playlist"`,
   `retroarch == null`), ≥ 3 GiB free on the SD card, no
   `~/.magic_dingus_box_backup.ota_in_progress` marker, no leftovers from an
   earlier run, port 443 free, the OLD source asset downloadable from
   GitHub. Prints hostname + model; stops here without `--yes`.
2. **Build** the NEW release artifacts (`run.sh artifacts`; refuses a
   tarball with AppleDouble `._*` files or git-lfs pointers) **and** the
   pisim kiosk binary the final restore pushes — both before the box is
   touched, so a build failure costs nothing.
3. **Snapshot**, on the box (`~/mdb_ota_hw_snapshot`) and copied to the Mac:
   `config/`, `services/.env` + `services/config` (sudo), `data/` minus
   media and roms, a tar of system state (`magic-*`/`gluetun-*`/`qbit-*`/
   `kiosk-*` units, every `*.d` and `*.wants` dir under
   `/etc/systemd/system`, `/usr/local/bin`, `/etc/udev/rules.d`, `/etc/hosts`,
   `~/.config/pulse`), `VERSION`, the list of active units, and three JSON
   states (`hw_helpers.py`): an app-data md5 manifest, a counts+bytes
   fingerprint of `data/{playlists,media,roms,saves,states,thumbnails}` and
   `/mnt/ssd`, and a system-state listing. All copies skip sockets/devices
   (`--no-specials --no-devices`: qBittorrent's ipc socket broke plain rsync).
4. **Downgrade** to OLD with the box's CURRENT `update.sh install <old>
   <github url>`, against real GitHub. Runs detached on the box (setsid +
   nohup), polled from the Mac — a dropped ssh link cannot kill it mid-rsync.
5. **Field-state strip** — see below. Then restarts web + kiosk and
   *records* the audio state (OLD v1.9.14 with the TV on HDMI1 is silent:
   recorded, not failed). Refuses to continue if the strip did not take.
6. **Fake GitHub on the box**: NEW artifacts + `box/fake_github.py` to
   `~/fakegh`; a 3-day CA + leaf for the GitHub hosts in `/run/fake-github`;
   the CA into the trust store; `/etc/hosts` backed up, then ONE tagged line
   appended; `control.json` latest = NEW; server started with `sudo setsid`.
   Proven by the request log, not just by a 200.
7. **Install NEW through the OLD web admin**, as its UI does: `GET
   /admin/update/check` → `GET /admin/csrf-token` → `POST
   /admin/update/install {version, download_url}` with `X-CSRF-Token` → poll
   `/admin/update/status/<job>`; when the web restart cuts the poll, fall
   back to `VERSION` + `/admin/update/version` (manager.js does the same).
   Requests go from the box itself with `Host: localhost`.
8. **Verify NEW**: `VERSION`; every pre-run active unit active, none failed;
   `NRestarts=0` for kiosk/web/audio; exactly one `pulseaudio`, inside
   `magic-dingus-audio.service`'s cgroup; when a TV reports audio (valid
   ELD) the default sink is not `auto_null`; the web admin serves;
   `verify_box.sh --with-services` SHIPPABLE; app-data manifest and content
   fingerprint identical to the snapshot; OTA marker gone.
9. **Rollback through the NEW web admin** (`POST /admin/update/rollback`;
   the connection may drop — `VERSION` is polled), then verify OLD is
   coherent: one `pulseaudio`, no stale audio unit running, web serving OLD,
   app data + content identical, marker gone.
10. **Teardown + restore**: (a) stop the server, `/etc/hosts` back
    byte-for-byte, CA out + `update-ca-certificates --fresh`, `rm
    /run/fake-github`; (b) the **deploy guard** dry-runs deploy_cpp.sh's
    three `--delete` rsyncs with its exact flags and filter list (parsed
    from the script) and ABORTS if any would delete under `data/` or
    `config/`; then `deploy_cpp.sh` (no `--build`) + `pisim.sh push`; files
    the strip touched that deploy did not rewrite go back from the snapshot
    tar (removed packages are reinstalled); (c) system-state listing,
    app-data manifest and content fingerprint vs the snapshot,
    `verify_box.sh --with-services`; (d) the on-box snapshot (it holds
    `.env` and the VPN key) and `~/fakegh` are deleted. The Mac copy is kept
    and its path printed.

Exit status is non-zero on ANY verification failure; verification failures
do not stop the run (the rollback and restore still happen), step failures do.

### Safety guarantees

- Nothing happens without `--yes`, after the target's hostname and model are
  printed. `PI_HOST` is never defaulted.
- Nothing is published: the real GitHub is only read (one OLD download); the
  NEW release exists only on the box's fake, which refuses every non-GET.
- **The fake GitHub is always removed** — by step 10a, or by the exit trap on
  any failure / Ctrl-C / ssh HUP (`/etc/hosts` restored byte-for-byte from
  `~/fakegh/hosts.orig`, with the tagged line as a fallback; CA removed;
  server stopped). A copy of `hosts.orig` is also kept on the Mac.
- **Nothing else is restored automatically on error.** The exit trap prints
  exactly which steps remain and the commands for them, including one
  command that finishes them: `--restore-only <logdir> --yes`.
- The server is matched with an **anchored** pattern
  (`^python3 /home/magic/fakegh/fake_[g]ithub\.py`): on 2026-10-04 an
  unanchored `pkill -f <path>` killed the very ssh session that ran it,
  because that session's command line contained the path. Remote scripts
  also travel on ssh's stdin, so no ssh command line ever contains them,
  and they run with stdin = `/dev/null`.
- The final deploy cannot remove operator data: the deploy guard runs first.
- All filenames are handled null-safely: manifests are built in Python on the
  box (JSON keys), never through shell word lists (ROM and save names have
  spaces).

### Field-state strip (what is generic, what is per-release)

After the downgrade the box runs the OLD tree but keeps every piece of
system state later deploys/OTAs put **outside** the tree — state a real OLD
field box never had (OTA never reinstalls unit files). Updating such a box
would rehearse the dev bench, not the customer. The strip plan is merged
from:

- **Generic (derived from git, every release):** unit files NEW's systemd
  dirs have and OLD's lack (`unit`: disable --now + remove); unit files both
  ship but NEW changed (`old-unit`: OLD's copy installed, if the unit is
  installed); `/etc/systemd/system/<unit>.d/<x>.conf` paths NEW's scripts
  mention and OLD's do not (`dropin`).
- **Per release:** `field_state_<OLD X.Y.Z>.txt` — required, reviewed, and
  the source of truth (the drop-in derivation is a grep heuristic). It adds
  what git cannot see: `package` (apt remove, e.g. `python3-gunicorn`),
  `pulse-client-conf` (the marker-tagged `autospawn = no` file), `file`,
  and `run <tree path>` (re-run an OLD-tree installer so files it owns carry
  OLD content, e.g. v1.9.14's `setup_memory_tuning.sh`). Rehearsing from a
  new OLD baseline means writing its file; the bats suite fails if one does
  not parse.

Every path the strip changes is journaled (`strip_journal.txt`, from a
before/after system listing); after the final deploy, journaled paths that
still differ from the snapshot are put back from its tar.

### Recovering from an aborted run

The exit trap prints the remaining steps. In short:

```bash
ssh "$PI_HOST" ls -la /home/magic/.magic_dingus_box_backup.ota_in_progress
#   present = an install/rollback was cut off; boot recovery (or
#   update.sh recover) handles it before you restore
PI_HOST=... tests/ota_rehearsal/hw_rehearsal.sh --restore-only ~/mdb-ota-hw-rehearsal/<run> --yes
```

`--restore-only` re-runs the (idempotent) fake-GitHub teardown, the deploy
guard, deploy + push, the strip-journal restore, all post-restore checks,
and deletes the on-box snapshot. A new run refuses to start while leftovers
of an old one (`~/mdb_ota_hw_snapshot`, `~/fakegh`, `/run/fake-github`, the
CA, the tagged hosts line, a running server) are present.

### Files

| File | Role |
|---|---|
| `hw_rehearsal.sh` | the driver (Mac) |
| `hw_rehearsal_lib.sh` | pure bash helpers (pkill pattern, hosts line, quoting) |
| `hw_helpers.py` | pure Python helpers, run on the Mac AND on the box over ssh: manifests, fingerprints, listings, comparisons, tarball check, deploy-guard parsing, field-plan merge |
| `field_state_<X.Y.Z>.txt` | per-OLD-release strip list |
| `box/fake_github.py` | the fake GitHub (shared with the container rehearsal) |
| `../local/ota_hw_rehearsal.bats` | off-Pi tests: helpers, deploy_cpp.sh parsing, a full `--dry-run` with ssh/rsync/curl/docker stubbed to fail |
