# OTA Update Pipeline

End-to-end documentation for the BDR Coverage Planner over-the-air
updater (Phases 1-9, complete). Read `AGENTS.md` first for the
short-form rules; this doc is the long-form reference.

## Goals

- Operator-driven (never auto-installs without confirmation).
- Survives field conditions: flaky networks, mid-install power loss,
  ctor crashes on the new build.
- Field-recoverable: a broken update auto-rolls back to the previous
  `.deb` without operator intervention.
- Production-grade with the smallest plausible surface area —
  see "What we deliberately don't do" below.

## Architecture overview

```
        ┌──────────────────────────┐
        │  GitHub Releases (CI)    │
        │  publishes .deb +.sha256 │
        │  to `latest` and v-<sha> │
        └────────────┬─────────────┘
                     │ HTTPS (poll)
                     ▼
   ┌──────────────────────────────────────┐
   │  OCU (bdr_coverage_planner)          │
   │  ────────────────────────            │
   │  • UpdateChecker polls GitHub        │
   │  • UpdateBanner (top of stage stack) │
   │  • UpdateModal (Install Now / Later) │
   │  • Install Now → handoff             │
   │  • [Phase 9] startup marker dispatch │
   │  • [Phase 9] 60 s health watchdog    │
   └────────────┬─────────────────────────┘
                │ QProcess::startDetached
                │ + lockfile handshake + qApp->quit()
                ▼
   ┌──────────────────────────────────────┐
   │  Runner (bdr-update-runner)          │
   │  ─────────────────────────           │
   │  • Frameless centered Qt window      │
   │  • UpdateDownloader (resilient)      │
   │  • SHA256 verify                     │
   │  • sudo -n bdr-apply-update install  │
   │  • writes marker, execv's OCU back   │
   │  • --rollback mode skips download    │
   └────────────┬─────────────────────────┘
                │ sudo NOPASSWD
                ▼
   ┌──────────────────────────────────────┐
   │  /usr/bin/bdr-apply-update (root)    │
   │  ─────────────────────────────       │
   │  install <deb>: dpkg -i              │
   │  recover     : dpkg --configure -a   │
   └──────────────────────────────────────┘
```

## Marker file: the bridge between processes

Single JSON file at `<CacheLocation>/update_state.json`, written
atomically via `QSaveFile`. Schema in `cpp/include/update/update_state.hpp`.

```json
{
  "schema": 1,
  "state": "<one of 7 stages>",
  "current_deb_path": "/home/.../updates/x.deb",
  "previous_deb_path": "/home/.../updates/y.deb"
}
```

### Stages

| Wire string                  | When written                                      | Who reads it          |
|------------------------------|---------------------------------------------------|-----------------------|
| `none`                       | (never written; marker absent)                    | OCU at startup        |
| `downloading`                | (reserved; not written today)                     | —                     |
| `dpkg_running`               | Runner, just before invoking `dpkg -i`            | OCU at startup        |
| `installed_pending_probe`    | Runner, after `dpkg -i` succeeds                  | OCU at startup        |
| `probing_health`             | OCU, on first sight of `installed_pending_probe`  | OCU at NEXT startup   |
| `done`                       | OCU, after `bootHealthy` fires within 60 s        | OCU at next startup   |
| `rolled_back`                | Runner (rollback mode), or OCU (handoff fallback) | OCU at startup        |

### Why `installed_pending_probe` AND `probing_health` are both needed

This pair is the crash-loop detection seam. Without it, the OCU on the
first launch after a successful install can't distinguish itself from
a relaunch after a ctor crash — both would see the same marker.

The two-state sequence solves this:

1. Runner finishes `dpkg -i`, writes `installed_pending_probe`, execvs
   the OCU.
2. OCU starts. Sees `installed_pending_probe` → atomically rewrites to
   `probing_health` → starts 60 s `QTimer` connected to `bootHealthy`.
3a. Healthy: `bootHealthy` fires → OCU writes `done`. Next launch is
    a normal startup.
3b. Crash: OCU dies before writing `done`. Marker stays as
    `probing_health`. Next launch sees `probing_health` on a *fresh*
    process (so the previous launch must have crashed) → triggers
    rollback handoff.

## OCU startup dispatch

`cpp/src/main.cpp`'s `determineStartupAction()` reads the marker and
returns one of four actions:

| Marker stage              | StartupAction                  | What main.cpp does                                         |
|---------------------------|--------------------------------|------------------------------------------------------------|
| `none` / `done` / others  | `Normal`                       | Boot AppShellWindow, no special wiring.                    |
| `dpkg_running`            | `Normal`                       | Sync `sudo bdr-apply-update recover` → clear marker → continue. |
| `installed_pending_probe` | `NormalWithProbe`              | Rewrite to `probing_health`. Boot AppShellWindow + 60 s watchdog. |
| `probing_health`          | `HandoffToRollbackRunner`      | Spawn runner with `--rollback <prev_deb>`, poll lockfile, exit. |
| `rolled_back`             | `NormalWithBanner`             | Boot AppShellWindow + `showRolledBackBanner()`.             |

When `HandoffToRollbackRunner` fails (no `prev_deb`, runner missing,
lockfile not acquired in 2 s), the OCU writes a `rolled_back` marker
itself and falls through to `NormalWithBanner` so the operator at
least sees that something went wrong.

## Runner UX

Two modes, same window structure:

| Aspect                  | Forward install                               | Rollback (`--rollback`)                            |
|-------------------------|-----------------------------------------------|----------------------------------------------------|
| Title                   | "Updating Roofus OCU"                         | "Restoring previous version"                       |
| Subtitle                | "Version <tag-or-sha>"                        | "The new version did not start cleanly. Restoring previous build." |
| Phases                  | Download → SHA verify → dpkg → restart        | dpkg → restart                                     |
| Status during dpkg      | "Installing update package — please do not power off." | "Rolling back to previous version — please do not power off." |
| Status on success       | "Installation complete — restarting…"         | "Rollback complete — restarting…"                  |
| Marker on success       | `installed_pending_probe`                     | `rolled_back`                                      |
| Marker on dpkg failure  | (kept as `dpkg_running`) + Try Again UI       | Best-effort `recover`, then `rolled_back` anyway   |

Forward-install Try Again is cause-aware: a download failure re-runs
the full download/verify; an install failure re-runs only `dpkg -i`
(the `.deb` is already verified on disk).

## Wrapper exit codes (`bdr-apply-update`)

| Exit code | Meaning                                       |
|-----------|-----------------------------------------------|
| 0         | Success.                                      |
| 64        | Usage / bad arguments.                        |
| 65        | Invalid `.deb` path (not a regular file, or wrong suffix). |
| 66        | `dpkg` returned non-zero.                     |
| 67        | Wrapper invoked without root (sudoers misconfigured). |

The runner translates these into operator-readable strings in the
failure UI.

## Lockfile contract

`<CacheLocation>/update_runner.lock` (`flock(LOCK_EX|LOCK_NB)`).

- Runner acquires on startup. Refuses to start if already held (rc=3).
- OCU's `handoffToUpdateRunner` polls `isRunnerLockfileHeld()` after
  `QProcess::startDetached`; only `qApp->quit()`s once the lock is
  observed held. This eliminates the "lost window" race where the OCU
  exits before the runner draws its first frame.
- The lock releases automatically on `execv` (kernel close-on-exec) or
  on process termination.

## Field-test recipe

### One-time prep on a fresh laptop

1. Install the current `.deb`:
   ```bash
   sudo dpkg -i bdr-coverage-planner_*.deb
   ```
   The postinst will land the sudoers drop-in at
   `/etc/sudoers.d/bdr-coverage-planner` after `visudo -c` validation.
2. Confirm the operator account is in the `sudo` group:
   ```bash
   id | grep -q '\bsudo\b' && echo "ok" || echo "FIX: add to sudo group"
   ```
3. Confirm NOPASSWD works:
   ```bash
   sudo -n /usr/bin/bdr-apply-update recover
   echo "rc=$?"
   ```
   Expected: rc=0, no password prompt. If it prompts, the postinst's
   `visudo -c` either rejected the file (check syslog) or the
   operator account isn't in `sudo`.

### Happy-path test

1. Push a no-op commit to `main`. Wait ~10 min for the GitHub Actions
   workflow to finish.
2. Launch the OCU. Within ~60 s the `UpdateChecker` should pick up
   the new SHA and the banner should appear at the top of the stage
   stack.
3. Click **View Details** → modal opens. Verify:
   - Battery status reads correctly (or "AC" if plugged in).
   - "Install Now" is enabled (no active mission/transfer/upload).
4. Click **Install Now**. Expected:
   - OCU window disappears.
   - Frameless runner window appears, centered on the primary screen.
   - Progress bar fills as `.deb` downloads.
   - "SHA verified" → "Installing update package…".
   - "Installation complete — restarting in 2 s…".
   - OCU comes back up on the new SHA.
5. Inspect the marker:
   ```bash
   cat "$HOME/.cache/PilotControl/BDR Coverage Planner/update_state.json"
   ```
   Expected: `{"state": "done", ...}`.
6. Inspect the log for the per-process tags:
   ```bash
   tail -50 "$HOME/.cache/PilotControl/BDR Coverage Planner/update.log"
   ```
   Expected: alternating `[ocu]` and `[runner]` tags telling a
   coherent story.

### Rollback test (the real value)

1. On a dev branch, introduce a deliberate ctor crash near the top of
   `AppShellWindow::AppShellWindow`:
   ```cpp
   std::abort();  // FIELD_TEST: revert before merging
   ```
2. Push to `main`. Wait for CI. The release will publish a `.deb`
   that crashes on launch.
3. On the test laptop, click **Install Now**. The runner installs the
   broken `.deb` and execs the OCU back. The OCU crashes immediately
   (you'll see no UI).
4. Relaunch the OCU manually:
   ```bash
   /usr/bin/bdr_coverage_planner_launcher
   ```
   Expected:
   - Marker still says `probing_health` from the previous launch.
   - `main.cpp` startup dispatch classifies this as a crash-loop:
     ```
     main: startup: probing_health marker on a fresh launch — previous
       probe never completed, rolling back to <prev.deb>
     ```
   - Runner spawns in `--rollback` mode with the "Restoring previous
     version" UI.
   - dpkg installs the previous `.deb`.
   - OCU comes back on the OLD SHA.
   - Rollback advisory banner is visible at the top.
5. Click **Dismiss** on the banner. Inspect the marker — it should
   be absent.
6. Revert the `std::abort()` and push again to leave the field laptop
   in a clean state.

## What we deliberately don't do

- **No background install.** Phase 6 modal `Install Now` button is
  the single trigger. We don't auto-install at any operator-invisible
  moment.
- **No mid-install cancel.** Once the runner spawns and acquires the
  lockfile, the operator can't back out until the runner reaches a
  terminal state (success / failure UI). This is intentional: dpkg
  partway through is worse than dpkg complete.
- **No rollback on download/verify failure.** Only on
  successful-install-but-broken-OCU. A failed download leaves the
  current install untouched; the runner shows Try Again / Cancel.
- **No multi-version retention.** Cache keeps exactly two `.deb`s:
  the just-installed and one generation back. Anything older is
  swept by `purgeOlderCachedDebs`.
- **No re-check of battery at install time.** Q3=C: the modal-time
  battery gate is the only check. Power loss mid-install is absorbed
  by the `dpkg_running` marker + `recover` path on next launch
  (Phase 9 watchdog).
- **No attempts counter.** State-machine semantics
  (`installed_pending_probe` → `probing_health`) carry the same
  information without a numeric field that could drift.

## Failure modes and what handles them

| Failure                                  | Mitigation                                          |
|------------------------------------------|-----------------------------------------------------|
| Network drops mid-download               | UpdateDownloader retries 3× with exponential backoff. |
| GitHub rate-limits the checker           | ETag caching + 304-aware client.                    |
| Download stalls with TCP open            | Stall timeout (30 s no progress) → retry.           |
| Total download exceeds ceiling           | Total-time ceiling (10 min) → terminal failure.    |
| SHA256 mismatch                          | Terminal failure with "checksum mismatch".          |
| `dpkg -i` fails                          | Marker stays `dpkg_running` → next launch's recover. |
| Power loss during `dpkg -i`              | Same — `recover` runs `dpkg --configure -a`.        |
| Two operators click Install Now twice    | Lockfile rejects second runner (rc=3).              |
| OCU exits before runner shows window     | Lockfile-poll handshake delays OCU exit until runner ready. |
| New OCU's ctor crashes                   | Marker stays `probing_health` → next launch rolls back. |
| New OCU's event loop wedged              | 60 s watchdog timer → rollback handoff.             |
| Rollback `dpkg` also fails               | Best-effort `recover`, write `rolled_back`, banner with severe message. |
| No previous `.deb` available             | `rolled_back` written with empty `current_deb_path`; banner shown; operator must reinstall manually. |
| sudoers drop-in corrupted                | postinst's `visudo -c` rejects the file, leaves sudo intact. OCU's `recover` invocation fails with a clear log line; no crash. |

## File index

```
cpp/include/update/
  update_types.hpp        # VersionInfo, gating constants
  update_log.hpp          # cross-process atomic logging
  update_lockfile.hpp     # flock(2) wrapper
  update_state.hpp        # marker schema (Phase 9)
  update_settings.hpp     # snooze + last-seen-SHA persistence
  update_checker.hpp      # GitHub poller
  update_downloader.hpp   # resilient HTTPS + SHA verify

cpp/include/components/
  update_banner.hpp       # "System Update Available" banner
  update_modal.hpp        # Install Now / Remind Me Later modal
  rollback_banner.hpp     # Phase 9 advisory banner

cpp/include/runner/
  update_runner_window.hpp # frameless installer window

cpp/src/update/            # implementations of the above
cpp/src/components/        # implementations of the above
cpp/src/runner/            # runner main + window
cpp/src/main.cpp           # OCU entry + Phase 9 marker dispatch + watchdog
cpp/src/app_shell.cpp      # bootHealthy emit + showRolledBackBanner

cpp/scripts/
  bdr-apply-update                  # privileged dpkg wrapper
  bdr-coverage-planner.sudoers      # NOPASSWD drop-in source

cpp/CMakeLists.txt          # bdr_update_core, bdr-update-runner targets
cpp/create_deb.sh           # packaging: postinst, prerm, sudoers stage+validate
.github/workflows/release.yml  # CI: build .deb + .sha256, publish
```
