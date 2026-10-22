# Changelog

What's New in the OCU update box. Read by roofing contractors, so:

- Every PR that changes the app adds its bullets at the top of
  `## Unreleased`. The "Release notes" PR check fails without one (label the
  PR `no-release-notes` when the operator will notice nothing).
- Say what the operator will see or can now do, in plain words. No technical
  terms (the check rejects words like ROS, S3, SHA, API), at most 80
  characters, one line, starting with `- `.
- Each release ships only the bullets added since the previous release
  (`.github/scripts/release_notes.py`), newest first, at most 5.
- Nothing to rotate: older bullets stay below and are never shipped again.

---

## Unreleased

- Upload progress now shows the whole building as a percentage
- Damaged scans on the drive are flagged: plug the drive into ROOFUS to fix
- Uploads keep going when one scan has a problem and list it at the end
- Uploads no longer pause on their own while files are still going up
- Uploaded scans are checked again, and missing ones can be uploaded again

## 2026-10-21 — OTA c72f8a5

- Robot map is now clearly visible on the light-mode canvas
- Satellite map turns to match the cloud, and parks when a pick disagrees
- A suspect pair is ringed; the fit card says when it wasn't checked
- OCU update banner now appears within ~2 min of a release, not 5
- Robot software check runs the moment you reach the Dashboard

## 2026-09-17 — OTA e9b2b52

- CI embeds the ArcGIS key so satellite maps work after OTA
- Launch warns at 25s if the robot never prints a line
- Failed robot cleanup now asks before launching anyway
- Pre-launch clears a wedged ROS 2 daemon
- Stop modal waits 3s so a sweep-end blip is not a prompt

## 2026-09-15 — OTA 7db7705

- Sync laptop/robot ~/pilot_ws; switch rebuilds only changed pkgs
- Stage 6 ROI: swath width (0.3-2 m) + speed (0.4-0.6 m/s) sliders
- Start Scan now unlocks, and says why in plain language when it doesn't
- Cancel and Complete Mission show progress; can save a partial sweep
- Map: two-finger scroll pans, Ctrl+scroll zooms, zoom-out bounded

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
