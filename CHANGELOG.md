# Changelog

All notable changes per release. The **top** `## ` block is the release notes
that ship in the next OTA build — keep it short (≤5 bullets, ≤80 chars each)
because the in-app modal renders only those bullets.

Bullets must start with `-`. Lines that don't start with `-` after the heading
are dropped by the workflow's release-notes extractor. Add a new top section
before pushing to `main` that triggers a release.

---

## v0.1.0-ota - 2026-05-08

- Initial OTA scaffolding: build-stamped commit SHA + GitHub Actions release
- App now reports embedded SHA on startup for diagnostics

## Pre-OTA history

Earlier work predates the OTA pipeline and is not surfaced in update modals.
See `git log` for full history.
