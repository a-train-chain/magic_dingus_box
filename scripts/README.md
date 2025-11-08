# Magic Dingus Box Scripts

This directory contains helper scripts for development, deployment, and maintenance.

## Raspberry Pi Scripts

### `update_pi.sh` - Auto Update & Restart
**Run on your Pi to pull latest changes and restart services**

```bash
cd ~/magic_dingus_box
./scripts/update_pi.sh
```

**What it does:**
- Pulls latest changes from GitHub
- Checks GPU memory allocation
- Deploys updated systemd service files
- Restarts magic-mpv and magic-ui services
- Shows service status and health checks

**Requirements:**
- Must be run on the Pi (not your dev machine)
- Must have git repository cloned
- Must have sudo privileges

**First time setup:**
```bash
chmod +x ~/magic_dingus_box/scripts/update_pi.sh
```

---

## Development Scripts

### `run_dev.sh` - Run UI in Development Mode
**Run on macOS/Linux for local testing**

```bash
./scripts/run_dev.sh
```

Starts the Magic Dingus Box UI with development settings.

### `mac_setup.sh` - Mac Development Setup
**Initial setup for macOS development**

```bash
./scripts/mac_setup.sh
```

Installs dependencies and sets up the development environment on macOS.

---

## Deployment Scripts

### `pi_install.sh` - Initial Pi Installation
**One-time setup on a fresh Raspberry Pi**

```bash
sudo ./scripts/pi_install.sh
```

Installs all dependencies and sets up systemd services.

### `setup_pi.sh` - Configure Pi Settings
**Configure display, audio, and system settings**

```bash
sudo ./scripts/setup_pi.sh
```

### `make_export.py` - Create Deployment Package
**Build a tarball for distribution**

```bash
python3 scripts/make_export.py
```

Creates a distributable package in the `dist/` directory.

---

## System Maintenance Scripts

### `os_ro_setup.sh` - Read-Only Filesystem Setup
**Configure Pi for read-only root filesystem (advanced)**

```bash
sudo ./scripts/os_ro_setup.sh
```

⚠️ **Warning:** This makes the root filesystem read-only for increased reliability but requires careful configuration.

---

## Quick Reference

| Task | Command |
|------|---------|
| Update Pi from GitHub | `./scripts/update_pi.sh` |
| Run UI locally (dev) | `./scripts/run_dev.sh` |
| Fresh Pi setup | `sudo ./scripts/pi_install.sh` |
| Create deployment package | `python3 scripts/make_export.py` |
| Mac dev environment | `./scripts/mac_setup.sh` |

## Notes

- All scripts should be run from the project root directory
- Scripts ending in `.sh` require execute permissions: `chmod +x scripts/*.sh`
- Pi-specific scripts require Raspberry Pi OS/Debian
- Development scripts work on macOS and Linux

