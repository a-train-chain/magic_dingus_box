# Magic Dingus Box — Live SD Card Cloning

This directory contains tooling to clone a running Pi's SD card over SSH
without ever physically removing the SD card. The result is a `.img.gz`
file on your Mac that you can flash to fresh SD cards using Raspberry Pi
Imager (or any similar tool).

The source Pi loses no permanent data: services come back exactly as they
were once the clone completes. Only ~1 minute of total kiosk downtime.

## Quick start

From your Mac, in the worktree:

```bash
./scripts/golden_image/clone_live_sd.sh
```

Defaults to cloning `magic@magicpi.local` to `~/golden_image_YYYY-MM-DD.img.gz`.

For a non-default Pi or output path:

```bash
./scripts/golden_image/clone_live_sd.sh \
    --pi magic@magicpi-abcd.local \
    --output ~/Desktop/my-golden.img.gz
```

To dry-run (walk through the prepare/restore steps without actually dd'ing):

```bash
./scripts/golden_image/clone_live_sd.sh --dry-run
```

## What happens during a clone

The script runs entirely inside one persistent SSH session (ssh
ControlMaster + ControlPersist). Six steps:

1. **Verify SSH connectivity** + sudo NOPASSWD on the Pi.
2. **Check SD card size + Mac free space** (need ~50% of card size for
   compressed output, or full size if `--no-compress`).
3. **Verify required local tools** (`ssh`, `dd`, `gzip`, optionally `pv`
   for a nice progress bar).
4. **Run `prepare_for_cloning.sh` on the Pi**, which:
   - Stops kiosk + Content Manager + Docker stack
   - Snapshots `device_info.json` + `/etc/hostname` + `/etc/hosts` to
     `/var/lib/magic-dingus-box/cloning_backup/` (these need to differ
     on each clone, so we remove them from disk for the dd, then put
     them back during restore)
   - Re-enables `magic-first-boot.service` (so the cloned image
     fires `first_boot.sh` on its first boot)
   - Drops a marker file at `cloning_backup/in_progress`
   - `sync; sync; drop_caches; sync`
5. **dd the SD card over SSH**, gzip-compressed in flight, written to
   the local `.img.gz` file. Progress shown via `pv` if installed.
6. **Run `restore_after_cloning.sh` on the Pi**, which:
   - Restores `device_info.json` + `/etc/hostname` + `/etc/hosts` from
     backup
   - Disables `magic-first-boot.service` (don't re-fire on source)
   - Restarts the Docker stack, Content Manager, and kiosk service
   - Removes the marker + backup files

A trap handler ensures step 6 fires even if the user `Ctrl-C`s during dd
or the network drops mid-stream — the source Pi never gets stuck in the
"in-progress" state.

## What happens when the cloned image boots on a new Pi

The new Pi inherits the source's filesystem layout but is missing the
identity files we removed in step 4. systemd starts up normally;
`magic-first-boot.service` (which was enabled by `prepare_for_cloning.sh`
on the source and persisted into the dd) fires before the kiosk service.
That runs `first_boot.sh`, which does:

| Step | What | Cloned Pi behavior |
|------|------|-------------------|
| 1 | Regenerate SSH host keys | (skipped — keys exist from source; see footnote) |
| 2 | Expand root FS to fill SD | Full SD now usable |
| 3 | Generate device identity | New UUID + new hostname `magicpi-XXXX` |
| 4 | Create required directories | (idempotent) |
| 5 | Fix ownership | (idempotent) |
| 6 | **Wipe Media Browser per-Pi state** | `services/.env`, `services/config/{radarr,prowlarr,qbittorrent,gluetun,flaresolverr}/*` removed |
| 7 | Self-disable | Won't run again |

After ~90 seconds, the kiosk is up in default mode (RetroArch + your
default playlists work). Media Browser is locked because there's no
`.env`. The operator activates it via the Content Manager UI on their
laptop (drops in a WireGuard config; `setup_services.sh` rebuilds the
entire stack from the codified fixtures in `magic_dingus_box_cpp/scripts/data/`).

> **Footnote on SSH host keys:** every cloned Pi inherits the source's
> SSH host keys. For a kiosk on a trusted LAN this is acceptable — the
> primary access path is the Content Manager (HTTP), not SSH. If you
> care about per-clone host keys, run `sudo ssh-keygen -A` and
> `sudo systemctl restart sshd` on each cloned Pi after first boot.

## Recovery if a clone goes sideways

If the Mac script crashes after running `prepare_for_cloning.sh` but
before `restore_after_cloning.sh` (network drop, kernel panic, force
quit), the source Pi will be in a degraded state: services stopped,
identity files missing. To recover, SSH to the Pi and run the restore
manually:

```bash
ssh magic@magicpi.local "sudo /opt/magic_dingus_box/scripts/golden_image/restore_after_cloning.sh"
```

The restore script is idempotent — running it without an in-progress
marker is a no-op. So it's always safe to run if you're not sure
whether the Pi needs restoration.

## When to use this vs. `prepare_golden_image.sh`

| Scenario | Use |
|---|---|
| Capture current Pi state with full library/data preserved on source | **`clone_live_sd.sh`** (live clone, source untouched) |
| Wipe source Pi's user data + create a "fresh defaults" image | `prepare_golden_image.sh` (destructive) → `create_image.sh` |

The live-clone tooling captures whatever state the Pi is in RIGHT NOW
(plus the per-Pi identity manipulation that lets the cloned image
self-configure). `prepare_golden_image.sh` actively wipes user state
on the source Pi (game saves, settings, downloaded movies in Radarr's
DB, etc.) — useful if you want to start fresh, but not if you have a
working library you want to keep using on the source while building
the golden image.

## Performance notes

| Network | ~32 GB SD clone time |
|---|---|
| Wired Ethernet (1 Gbps) | ~5-10 min |
| USB-Gadget Ethernet | ~30-50 min |
| WiFi 5 (802.11ac) | ~30-90 min |
| WiFi 4 (802.11n) | ~2-4 hours |

Compression is `gzip -1` (fast, not max). On typical Pi content the
output is ~50% of the source size. Use `--no-compress` to skip if
you'd rather have a raw `.img` file (some flashing tools require it).

## Output file flow

```
~/golden_image_2026-04-26.img.gz
        │
        ├──► Raspberry Pi Imager → "Use custom" → flash to SD
        │
        └──► Or via dd: gunzip < golden_image.img.gz | sudo dd of=/dev/diskN bs=4M
```
