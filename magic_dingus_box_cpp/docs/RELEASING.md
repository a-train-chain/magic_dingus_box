# Releasing — stable and beta

Operator-only. Customers never see any of this (`OWNER_GUIDE.md` does not
mention betas on purpose). The OTA contract itself lives in
[`OTA_UPDATE_GUARANTEES.md`](../../OTA_UPDATE_GUARANTEES.md) "Update channels".

## The two channels

| Channel | Who | What `update.sh check` asks GitHub | Offered |
|---|---|---|---|
| `stable` (default) | every customer unit, every clone | `releases/latest` | the newest stable release |
| `beta` | the owner's own boxes only | `releases?per_page=20` | the highest version among stable **and** beta releases (drafts ignored) |

A box's channel is one word in `/opt/magic_dingus_box/config/update_channel`.
The file is absent on a stable box — absence (or anything other than the
exact word `beta`) means stable. `config/` is excluded from every OTA rsync
and from `deploy_cpp.sh`, so updates and rollbacks never change it.

Version order is SemVer precedence, everywhere (`update.sh` `version_cmp`):

    1.10.0  <  1.10.1-beta.1  <  1.10.1-beta.2  <  1.10.1-beta.10  <  1.10.1

Neither channel ever offers a downgrade.

## Before any tag: check the fleet you can reach

Run every reachable box's own acceptance test from the Mac — strictly
read-only, one table, non-zero exit if anything is not shippable:

```bash
scripts/fleet_check.sh --hosts ~/fleet.txt                   # one host per line, # comments
scripts/fleet_check.sh --hosts ~/fleet.txt --with-services   # + verify_services.sh (Media Browser boxes)
```

Do it **before** tagging (so a box that is already failing is not mistaken
for a regression of the new release) and again after your own boxes take
the beta or stable. Each row is host / board / version / channel / uptime /
pass-warn-fail / result / first failing check; `--log-dir DIR` keeps the
full `verify_box.sh` output per box. A box still on `beta` shows it in the
channel column — expected on your own boxes, never on a customer's.

Also make sure CI is green, including the blocking ShellCheck gate
(`tests/shellcheck_gate.sh`, test-local.yml `shellcheck` job) — release.yml
runs test-local.yml and will not publish past a finding. Changing
`update.sh`? Read
[`docs/superpowers/specs/2026-10-05-update-sh-split-design.md`](../../docs/superpowers/specs/2026-10-05-update-sh-split-design.md)
"Which update.sh actually runs" first.

## Cut a beta

From the commit you want to test (normally `main`, tests green):

```bash
git tag v1.10.1-beta.1
git push origin v1.10.1-beta.1
```

`release.yml` runs every suite, builds the arm64 binary and the source
tarball exactly as for a stable release (same release-blocking assertions),
and publishes the GitHub Release with **`prerelease: true`** because the tag
contains a `-`. The tarball's `VERSION` file says `1.10.1-beta.1`.

Tag grammar is strict: `vX.Y.Z` or `vX.Y.Z-beta.N`. Anything else
(`-rc1`, `-beta`, `-Beta.1`) fails the release workflow's "Extract version"
step on purpose — no box could install it.

### Changelog and VERSION

A beta does **not** touch the repo's `VERSION` or `CHANGELOG.md`. Cut it
from a commit whose changelog still has `## [Unreleased]`; release.yml
stamps the tag into the tarball's `VERSION`. There is never a
`## [1.10.1-beta.1]` heading (`tests/local/changelog_format.bats` accepts
plain `X.Y.Z` headings only), and `version_consistency.bats` is satisfied
by the `[Unreleased]` section. The changelog is written once, for the
stable release.

## Put your own boxes on beta

Either:

- **Content Manager** → Settings → Software Updates → **Advanced** → tick
  **Get early (beta) updates** (confirm the prompt), then **Check for
  Updates**; or
- **SSH**:

  ```bash
  /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/update.sh channel        # prints stable|beta
  /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/update.sh channel beta
  /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/update.sh check
  ```

**First time only:** the channel feature itself must already be on the box.
A box running 1.10.0 or older has no channel switch, and its updater rejects
`-beta.N` versions outright. Ship the channel feature in a normal stable
release first (or `deploy_cpp.sh` it to your own boxes), then switch.

`verify_box.sh` prints a **WARN** on a beta box. That is expected on your
own boxes and a stop sign on anything you are about to ship.

## Promote to stable

When the beta has proved itself, tag the stable release from the same
commit (or from the commit with the fixes):

```bash
# finalize CHANGELOG.md: rename [Unreleased] -> [1.10.1] - YYYY-MM-DD, set VERSION to 1.10.1
git tag v1.10.1
git push origin v1.10.1
```

It publishes as a normal release and becomes `releases/latest`, so every
stable box is offered it. Beta boxes are offered it too, because
`1.10.1 > 1.10.1-beta.N` — they leave the beta automatically and stay on the
beta channel for the next round.

Then switch boxes back if you like: `update.sh channel stable`. A box on
`1.10.1-beta.2` switched to stable while the newest stable is still
`1.10.0` sees **no** update (that would be a downgrade); it simply waits
for the next stable (`1.10.1` or later).

## Pull a bad beta

1. **Delete the GitHub prerelease** (Releases → the beta → Delete; the tag
   can stay or go). Beta boxes that have not installed it stop being
   offered it on their next check (the Content Manager caches a check for
   30 seconds). Stable boxes never saw it.
2. On any of your boxes that already installed it, roll back: Content
   Manager → **Rollback**, or `update.sh rollback` over SSH. The rollback
   keeps the channel (config/ is preserved), so the box is then offered the
   next-highest remaining release — an earlier beta if one is still
   published, otherwise nothing until the next release.
3. Fix forward with `-beta.N+1`. Never reuse a version number: a box
   already on `1.10.1-beta.2` will not take a different `1.10.1-beta.2`.

If a beta breaks the updater itself (`update.sh`), the box updates with
that broken updater from then on — repair it over SSH with `deploy_cpp.sh`.
This is the main reason betas go to the owner's boxes first.

## Golden images and shipped units

Clones always start on stable:

- `prepare_for_cloning.sh` **refuses** to clone a beta box (run
  `update.sh channel stable` on the source first);
- `first_boot.sh` Step 6i deletes the flag on every clone regardless;
- `prepare_golden_image.sh` removes it as well;
- `verify_box.sh` warns on any box still on beta.
