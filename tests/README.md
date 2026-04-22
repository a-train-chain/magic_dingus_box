# Magic Dingus Box Test Suite

Two-tier test suite that gates the golden image. Local tier runs anywhere; Pi tier runs from your dev machine against a reachable Pi via SSH.

## Quick start

```bash
# All tests (local + pi)
./tests/run_all.sh

# Just the local tier (no Pi needed; fast)
./tests/run_all.sh --local-only

# Just the Pi tier (PI_HOST defaults to magic@10.55.0.1)
PI_HOST=magic@magicpi.local ./tests/run_all.sh --pi-only

# Filter by file substring
./tests/run_all.sh --filter bezel
```

## Dependencies

**Local (Mac dev machine):** `bats-core`, `shellcheck`, `yamllint`, `jq`, Python 3 with `pyyaml` and `jsonschema`.

```bash
brew install bats-core shellcheck yamllint jq
pip3 install pyyaml jsonschema
```

**On the Pi:** `jq` (already installed), `vcgencmd`, `systemctl`, `pactl` (all stock).

## How tests are organized

| Directory | Runs where | Purpose |
|---|---|---|
| `tests/local/` | Anywhere (incl. CI) | Schema validation, shell linting, format checks |
| `tests/pi/` | Mac → Pi via SSH | Hardware state, services, game launches |
| `tests/manual/` | Human at the TV | Visual / audio / input-feel verification |

Tests are auto-discovered by glob — drop a new `.bats` file in the right directory and the runner picks it up.

## Adding a new test

1. Pick the right tier (`local/` if it can run without a Pi; `pi/` if it needs SSH to one)
2. Create `tests/<tier>/<feature>_<topic>.bats` (e.g., `tests/pi/wifi_credentials_storage.bats`)
3. Source the helpers at the top: `load "$BATS_TEST_DIRNAME/../lib/helpers.bash"`
4. Use `require_pi`, `require_joystick`, etc. for prerequisites that should `skip` rather than `fail`
5. Run just your test: `./tests/run_all.sh --filter <topic>`
6. When the feature lands, make sure your test is green BEFORE merging

Example minimal Pi test:

```bash
#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

setup() { require_pi; }

@test "magic-dingus-box-cpp service is active" {
    run pi_ssh 'systemctl is-active magic-dingus-box-cpp.service'
    [ "$status" -eq 0 ]
    [ "$output" = "active" ]
}
```

## CI

`.github/workflows/test-local.yml` runs the local tier on every push and PR. Pi tier is manual-invoke only (no real Pi hardware in GitHub runners).

## Pre-image gate

`scripts/golden_image/prepare_golden_image.sh` runs `./tests/run_all.sh` first and aborts if anything fails. To bypass (audit-logged): `--skip-tests`.
