# New Release Smoke Test

Run after deploying a new release to a previously-imaged Pi. Faster than the full pre-image checklist; focused on "did the update break anything obvious".

**Release version:** _________________
**Pi being tested:** _________________
**Date:** _________________

## Quick smoke (5–10 min)

- [ ] `./tests/run_all.sh --pi-only` is green
- [ ] Kiosk UI loads after the update
- [ ] Settings menu opens and the version shown matches the new release
- [ ] At least one game per cluster launches without error:
  - [ ] NES (lightest)
  - [ ] PS1 (heaviest)
- [ ] Bezel still renders in Modern TV mode (if applicable)
- [ ] Pre-update saved games still load (no save corruption)
- [ ] Settings retained from before update (display mode, audio output)
- [ ] No new error/warning entries in journal:
  ```
  ssh PI 'sudo journalctl -u magic-dingus-box-cpp.service --since "10 minutes ago" | grep -iE "error|fail|critical"'
  ```

If all green: release accepted on this unit. If any failures, document below before rolling back.

## Notes / Issues found

(Free text here)
