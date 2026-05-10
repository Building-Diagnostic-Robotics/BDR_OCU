# AGENTS.md — BDR_CP

This file gives AI agents working in this repo persistent context. Read it
before making suggestions or changes.

## What this project is

BDR Coverage Planner — the Operator Control Unit (OCU) and coverage planning
GUI for "Roofus," an autonomous mobile robot that performs building roof
scanning. Built with C++17, Qt Widgets, and ROS2 (rclcpp). All code lives in
the `f2c_cpp` namespace. For deeper architecture details see `cpp/CLAUDE.md`.

Build from `cpp/`:

```bash
./build.sh                      # Release
./build/bdr_coverage_planner    # Run
```

## Development bypasses awaiting re-wiring

Several stages in the planner have been **intentionally bypassed** so UI
development and testing can happen offline, without a robot attached. These
are **temporary** and must be re-wired (or guarded behind a dev flag) before
release. The full, tickable checklist lives in `docs/DEV_BYPASSES.md`.

Quick inventory of current bypass sites:

- `cpp/src/startup_screen.cpp` — `kEnableLaunchDashboardPassthrough = true`
 forces Stage 2 Continue always-enabled regardless of preflight result.
- `cpp/src/planner_screen.cpp` — `kBypassPlannerStageGates = true` lets
  the operator click into Scan Splitting / Scan stages without a saved
  map, completed plan, or published waypoints. Intentionally open during
  field testing; close before customer delivery.
- `cpp/src/app_shell.cpp` — `DashboardScreen::viewRecordingsRequested`
  signal is emitted but intentionally not connected yet. Dialog `.cpp`
  files are present but excluded from `GUI_SOURCES` until this lands —
  see `cpp/CMakeLists.txt`.
- `cpp/src/dashboard_screen.cpp` — Top-row cards are all live now.
  System Status rolls up
  {preflight, battery, MQTT-freshness reachability proxy} into
  INITIALIZING/READY/WARNING/NOT READY. Battery card mirrors the
  live MQTT subscriber. Total Scans + Next Calibration share a
  single combined SSH probe with Last Calibration that returns
  `<cal_mtime> <total_scans> <scans_since_cal>`; the calibration
  card blinks (`QGraphicsOpacityEffect`) and becomes clickable once
  `kCalibrationDueAfterScans = 3` scans have passed since the last
  tilt calibration.   The lower System Information row is fully wired,
  including Uptime (elapsed since `main()` stamped
  `kOcuStartEpochMsProperty` on `QApplication`, refreshed
  every second while Stage 3 is visible).
- `cpp/src/planner_screen.cpp` — Stage 4 is fully wired: progress +
  quality from live odometry and reprojection, per-segment
  completion from real controller `segment_complete` /
  `segment_saved` payloads, per-segment plot colors via a
  status-driven `PlotWidget` overlay (Figma-spec), and the active
  segment spinner is animated via a 30 ms `QTimer` rotating the
  `scan_segment_active.svg` icon. No remaining Stage 4 dev
  short-circuits worth tracking. See `docs/DEV_BYPASSES.md`.

### Rules for agents touching these sites

- **Do NOT delete or "clean up" these bypasses** as a drive-by change.
  They are the current dev workflow.
- When a user explicitly asks to re-wire one, follow the checklist in
  `docs/DEV_BYPASSES.md` and tick the item there in the same change.
- When adding a new temporary short-circuit, tag the line with a
  `// BDR_REWIRE:` comment and add a matching entry to
  `docs/DEV_BYPASSES.md`.
- Before any release or "cleanup" task, run:

  ```bash
  rg -n 'BDR_REWIRE|Dev bypass|dev bypass|kEnable.*Passthrough' cpp/
  ```

  and surface each remaining match to the user.

## Staged flow (current reality)

`AppShellWindow` owns a `QStackedWidget` with 5 stages, each a self-contained
`QWidget`:

| Stage | Class | File |
|-------|-------|------|
| 1 | `SetupScreen` | `cpp/src/setup_screen.cpp` |
| 2 | `StartupScreen` | `cpp/src/startup_screen.cpp` |
| 3 | `DashboardScreen` | `cpp/src/dashboard_screen.cpp` |
| 4 | `ExplorationScreen` | `cpp/src/exploration_screen.cpp` |
| 5 | `PlannerScreen` | `cpp/src/planner_screen.cpp` |

The monolithic legacy window **`CoverageGUI`** has been **removed** from the
tree (`coverage_gui.hpp/.cpp` deleted). Live UI is `AppShellWindow` + staged
screens; map/video/widgets live in `plot_widget.*`, `video_stream_widget.*`,
`coverage_geometry.hpp`, `coverage_stats.hpp`. Recordings/cloud-tab sources
(`data_transfer_dialog`, `cloud_upload_dialog`, `scan_session_tracker`,
`teleop_widget`, `network_monitor`) remain **on disk** but are **not** linked
into `bdr_coverage_planner` — see the comment above `GUI_SOURCES` in
`cpp/CMakeLists.txt` when rewiring `DashboardScreen::viewRecordingsRequested`.

## OTA update pipeline (Phases 1-9, complete and production-wired)

Operator-driven over-the-air updater. The OCU polls GitHub Releases
(`latest` tag), shows a non-intrusive banner when a new commit SHA
appears, and on operator confirmation hands off to an external
`bdr-update-runner` binary that downloads, verifies SHA256, dpkg-installs
the new `.deb`, and `execv`s the OCU back. A 60 s health-probe watchdog
in the OCU's `main()` automatically rolls back to the previous `.deb` if
the new OCU fails to fire `AppShellWindow::bootHealthy` in time.

### Key entry points

- `cpp/src/main.cpp` — startup-time marker dispatch + watchdog wiring.
- `cpp/include/update/update_state.hpp` — JSON marker schema (the bridge
  between OCU and runner).
- `cpp/src/update/update_checker.cpp` — GitHub Releases poller.
- `cpp/src/update/update_downloader.cpp` — resilient download with
  retries, ETag caching, stall detection, total-time ceiling.
- `cpp/src/components/{update_banner,update_modal,rollback_banner}.cpp`
  — the three operator-facing surfaces.
- `cpp/src/runner/` — the external installer binary (build target
  `bdr-update-runner`).
- `cpp/scripts/bdr-apply-update` — privileged dpkg wrapper, invoked via
  NOPASSWD sudo. Subcommands: `install <deb>`, `recover`.
- `cpp/scripts/bdr-coverage-planner.sudoers` — sudoers drop-in,
  validated by `visudo -c` in the postinst before being moved into
  `/etc/sudoers.d/`.
- `.github/workflows/release.yml` — CI publishes `.deb` + `.sha256`
  sidecar to both the rolling `latest` tag and an immutable `v-<sha>`
  tag on every push to `main`.

### Deployment artifacts

The `.deb` ships:

- `/usr/bin/bdr_coverage_planner` (OCU)
- `/usr/bin/bdr_coverage_planner_launcher` (env-setup wrapper)
- `/usr/bin/bdr-update-runner` (external installer)
- `/usr/bin/bdr-apply-update` (privileged dpkg wrapper)
- `/usr/share/bdr-coverage-planner/sudoers/bdr-coverage-planner` (staged
  sudoers source; postinst moves it to `/etc/sudoers.d/` after
  `visudo -c` validation)

### Marker file states

`<CacheLocation>/update_state.json` (atomic writes via `QSaveFile`) carries
exactly one of seven states. The full state-transition diagram lives in
`docs/OTA.md`.

### Rules for agents touching the OTA path

- **Do NOT remove or "simplify"** any of the following — they are
  load-bearing:
  - `bdr-update-runner` binary or its CMake target.
  - `bdr_update_core` static library (shared between OCU and runner).
  - `bdr-apply-update` wrapper or the sudoers drop-in.
  - The marker schema (`update_state.{hpp,cpp}`) including
    `InstalledPendingProbe` (the crash-loop-detection seam).
  - `AppShellWindow::bootHealthy()` signal — the watchdog's healthy
    completion gate.
  - The lockfile dance in `update_lockfile.{hpp,cpp}` and the OCU's
    `handoffToUpdateRunner` polling loop.
- The `applicationName` MUST be `"BDR Coverage Planner"` (with spaces)
  in **both** `cpp/src/main.cpp` and `cpp/src/runner/main.cpp`. They
  share `QStandardPaths::CacheLocation` for the marker, log, and cache.
  Diverging the strings silently breaks the entire handoff.
- Settings code that uses `QSettings(kSettingsOrgName, kSettingsAppName)`
  passes those names explicitly and is unaffected by the QApplication
  name above. Don't conflate the two.
- Phase 9 watchdog only attaches in the
  `StartupAction::NormalWithProbe` branch. Do not add unconditional
  `done`-marker writes in `AppShellWindow` — that would mask real
  ctor-crash failures from triggering rollback.

### Docs

- `docs/OTA.md` — state-transition diagram, runner UX, wrapper exit
  codes, field-test recipe.

## Docs worth reading

- `cpp/CLAUDE.md` — authoritative architecture overview and build notes.
- `docs/DEV_BYPASSES.md` — the re-wiring checklist (see above).
- `docs/OTA.md` — OTA state machine, runner UX, field-test recipe.
- `docs/TILT_CALIBRATION_PLAN.md` — tilt calibration design + TODO list.
