# Changelog

All notable changes per release. The **top** `## ` block is the release notes
that ship in the next OTA build — keep it short (≤5 bullets, ≤80 chars each)
because the in-app modal renders only those bullets.

Bullets must start with `-`. Lines that don't start with `-` after the heading
are dropped by the workflow's release-notes extractor. Add a new top section
before pushing to `main` that triggers a release.

---

## Unreleased - 2026-09-15

- Sync laptop and robot ~/pilot_ws to cliff-on-autonomy via banner
- Stage 6 ROI card: swath width (0.3-2 m) and speed (0.4-0.6 m/s) sliders
- Fix scan launch arg that stopped the robot planning the scan
- Scan page now says why Start Scan is locked, in plain language
- Cancel and Complete Mission show progress instead of freezing
- Launching the scan no longer leaves you stuck on Edge Review

## 2026-09-13 — OTA 082cde0

- Upload locks until the RDATA_EXT copy is done; list by building/operator
- Preflight RGB uses the robot's single See3CAM (left_rgb)
- Tilt-calibration modal uses the Stage 3 zinc chrome
- Stage 6 Complete Mission shows finalize progress and skip-copy
- OTA can be targeted per robot id, or pinned off on one laptop

## v1.0.0 - 2026-05-09

- OTA pipeline complete: GitHub Releases polling, frameless install runner
- Phase 9 watchdog detects boot-probe failure and rolls back automatically
- Pre-install gating on battery + active mission, snooze 4h
- bdr-apply-update privileged wrapper installed via NOPASSWD sudoers drop-in
- Vendored odrive_can interface libs bundled into deb (desktop launch fix)

## v0.1.0-ota - 2026-05-08

- Initial OTA scaffolding: build-stamped commit SHA + GitHub Actions release
- App now reports embedded SHA on startup for diagnostics

## Pre-OTA history

Earlier work predates the OTA pipeline and is not surfaced in update modals.
See `git log` for full history.
