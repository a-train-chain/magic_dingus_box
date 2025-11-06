# Professional Readiness Roadmap (Optional, non-breaking)

This document lists advanced improvements to elevate the project to a production-grade product. All items are optional and can be implemented incrementally.

## Build and Dependency Hygiene
- Adopt pinned, reproducible builds with pip-tools (`requirements.in` → `requirements.txt`).
- Provide platform-specific wheels cache in CI to speed installs.
- Add `pyproject.toml` with project metadata and entry points (e.g., `magic` CLI).

## CI/CD
- GitHub Actions:
  - Lint (ruff), type-check (mypy), unit tests (pytest) on PRs and main.
  - Build distributable archive(s) on tagged releases (attach to GitHub Release).
- Optional: Nightly build that runs a smoke test on a headless framebuffer (xvfb).

## Packaging & Distribution
- Publish a versioned tarball with `dist/` (already present) plus checksums.
- Optional: Build a Debian package for Raspberry Pi (systemd units included).

## Security & Web Admin
- Proper auth for admin (Flask session or token + CSRF, rate limiting, CORS policy).
- Serve over HTTPS behind a reverse proxy (Caddy/Nginx) when remote access is required.
- Content validation: enforce allowed extensions, scan for double extensions, reject oversized files.

## Observability
- Structured logging (JSON) toggle via env; include request IDs in web admin.
- Log rotation config on systemd-journald; optional remote error tracking (Sentry) behind env flag.

## UX polish (device and web)
- i18n scaffolding for UI strings; language selector in web UI.
- Accessibility checks (contrast, keyboard navigation) for web UI.
- Settings backup/restore export (download/upload `settings.json`).

## Performance and Reliability
- Library scanning metrics and incremental indexing for large media libraries.
- Hash-based deduplication for uploads; optional checksum verification.
- Add watchdog for mpv socket availability and auto-restart policy tuning.

## Installer and Ops
- One-time installer script to create `magic` user/group, directories, and deploy hardened unit files.
- System health endpoint enriched with FPS, CPU temp, storage usage.
- Example reverse proxy config with TLS (Caddyfile/nginx snippets).

## Testing
- Expand tests for playlist parser edge-cases and web admin endpoints.
- CLI smoke tests: start admin server in test mode and hit health endpoint.

## Documentation
- Versioned docs: keep `docs/` current; archive older design notes in `docs/archive/`.
- Screenshots/gifs demonstrating display modes and bezel styles.

---

Adopting these steps incrementally will further harden security, improve reliability, and polish the end-user experience without changing current defaults.

