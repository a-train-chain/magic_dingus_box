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

## Everything runnable without a Pi

```bash
./tests/run_offline.sh                 # all offline suites, one summary
./tests/run_offline.sh --with-cpp      # ...plus the C++ Catch2/CTest suites
./tests/run_offline.sh --suite web-pytest --suite ota-bats   # a subset
./tests/run_offline.sh --quiet         # only failing suites' output
PYTHON=.venv/bin/python ./tests/run_offline.sh   # interpreter for the Python suites
```

`run_all.sh --local-only` covers only `tests/local/*.bats`. `run_offline.sh`
also runs `tests/local/test_*.py`, the updater's suites in
`magic_dingus_box_cpp/scripts/tests/` (bats + unittest — `update.sh`, unit
converge, memory tuning, cascade, cooldowns, missing-search), `scripts/tests/`
when it exists, and the web admin pytest suite, then prints a per-suite
pass/fail/skip table and exits non-zero if any suite failed. A suite it
cannot run because a tool is missing (`bats`, or `flask`/`pyyaml`/`pytest`
in `$PYTHON`) is named as NOT RUN: a warning on a dev machine, a failure
when `CI` is set. The C++ suites are opt-in (`--with-cpp`) because they need a
CMake build (`BUILD_KIOSK=OFF`, into `magic_dingus_box_cpp/build-test`;
`CPP_BUILD_DIR`, `MEDIA_BROWSER`, `MAKE_JOBS` override) — mind the disk.

The web suite's production-server tests (`test_serve.py`: gunicorn gthread +
the Phone Remote WebSocket) and real-conversion upload tests need
`pip install gunicorn==23.0.0 'flask-sock>=0.7.0'` and `ffmpeg`/`ffprobe`.
Without them they skip locally; with `CI` set they FAIL
(`magic_dingus_box/web/tests/_deps.py`), so CI can never silently stop
testing the server the boxes run.

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

`.github/workflows/test-local.yml` runs the local tier on every push and PR. Pi tier is manual-invoke only (no real Pi hardware in GitHub runners). `release.yml` calls both `test-local.yml` and `test-ota.yml` (the updater's suites) and publishes only when both pass.

Its `shellcheck` job is a blocking lint gate over **every** tracked shell script: `tests/shellcheck_gate.sh` (all `*.sh`/`*.bash` plus extensionless sh/bash-shebang files) at `-S info`, with the pinned ShellCheck 0.11.0. Run it locally before pushing — `brew install shellcheck` is 0.11.0 today; a newer local version may report extra findings CI does not see yet. Repo-wide exclusions (each justified) live in `/.shellcheckrc`; a deliberate one-off pattern gets an inline `# shellcheck disable=SCxxxx  # reason`. The CI step also carries a short, temporary `--exclude` list — shrink it, never grow it.

## Pre-image gate

`scripts/golden_image/prepare_golden_image.sh` runs `./tests/run_all.sh` first and aborts if anything fails. To bypass (audit-logged): `--skip-tests`.
