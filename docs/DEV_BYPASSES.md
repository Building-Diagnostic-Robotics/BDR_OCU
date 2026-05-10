# Dev Bypasses — Rewire Before Release

This document tracks every **intentional short-circuit** in the BDR Coverage
Planner. These exist so UI development and testing can happen offline without
a live robot connection. Each item below must be rewired (or guarded behind a
`BDR_DEV_MODE` build flag / env var) before shipping.

> **Maintenance rule:** when you add a new temporary bypass, add a matching
> `// BDR_REWIRE:` tag at the code site AND a checklist entry here. When you
> rewire one, tick the box in the same commit.

---

## Stage 1 — Setup / Login (`cpp/src/setup_screen.cpp`)

- [x] **Real authentication.** `SetupScreen::submit()` now calls
      `loginToRobot()` (`cpp/src/robot_login.cpp`), which SSHes to the
      robot and runs `pilot_control_auth login --pin-stdin --json`. On
      failure, raw errors are recorded via `update::log::warn` and a
      mapped operator-friendly message is shown in `lbl_error_` (see
      `mapLoginErrorForOperator` in `setup_screen.cpp`). The button is
      disabled and relabeled `LOGGING IN…` during the SSH round-trip.
      `setup/robot_id` is persisted to QSettings only after a successful
      login. The session token returned by the robot is parsed and
      verified inside `loginToRobot` but discarded — see
      `TODO(BDR_REWIRE)` in `robot_login.hpp` for the follow-up to plumb
      it into the planner mission-upload path.
- [x] **Robot-aware connection monitor.** `SetupScreen::checkConnection()`
      now pings the host of the first robot in `robots.json`, cached at
      construction. The indicator reflects radio-link reachability — a
      single per-laptop concern, independent of whatever Robot ID the
      operator types. If the registry is missing or empty the indicator
      stays in DISCONNECTED rather than silently pinging a hardcoded
      address. The `robot_ip` QSettings key is preserved as an
      undocumented per-machine override.
- [x] **Registry bootstrap.** `SetupScreen` constructs and loads a
      `RobotRegistry` directly via `RobotRegistry::load()`, which probes
      `~/.config/PilotControl/BDRCoveragePlanner/robots.json` first and
      falls back to alongside-binary locations. `submit()` reloads the
      registry on each login click (matches the existing pattern in
      `AppShellWindow::loadExplorationRfConfigForActiveRobot`). No
      injection seam needed for now since the registry file is ~hundreds
      of bytes and load latency is negligible.

## Stage 2 — Pre-flight Diagnostics (`cpp/src/startup_screen.cpp`)

- [ ] **Continue gate.** Remove
      `constexpr bool kEnableLaunchDashboardPassthrough = true;` (top of
      file) or guard it behind `#ifdef BDR_DEV_MODE`. The "enabled" check
      around line 1071 must reduce to
      `preflight_completed_ && !has_fail` once the passthrough is gone.

## Stage 3 — Dashboard (`cpp/src/dashboard_screen.cpp`, `cpp/src/app_shell.cpp`)

- [ ] **Recordings action.** `DashboardScreen::viewRecordingsRequested` is
      emitted (`dashboard_screen.cpp:398`) but not connected in
      `AppShellWindow::ensureStage3()` (`app_shell.cpp:751-764`). Wire it
      to open `DataTransferDialog` (robot → laptop rsync) and, optionally,
      `CloudUploadDialog` (laptop → S3). Both classes already compile and
      are only reachable from the legacy `CoverageGUI` today.
- [ ] **Info-card values.** Firmware version, Last Calibration timestamp,
      and Battery level are static strings. Source:
  - Firmware — TBD (likely an SSH query; define in `robots.json` or a
    dedicated service).
  - Last Calibration — timestamp of latest
    `/R_DATA/tilt_calibration/tilt_correction_matrices_*.npz` via SSH.
    See `docs/TILT_CALIBRATION_PLAN.md §5.1`.
  - Battery — existing ROS2 telemetry or SNMP probe (check
    `ExplorationScreen` telemetry sources for reuse).
- [ ] **Status card live values.** The Status / Scans / Cameras /
      Calibration value labels need to be connected to real data streams
      rather than the compiled-in defaults.

## Stage 5 — Planner (`cpp/src/planner_screen.cpp`)

- [ ] **Stage 3 / Stage 4 navigation gates.** A single dev-bypass constant
      `kBypassPlannerStageGates = true` (top of `planner_screen.cpp`,
      anonymous namespace) currently lets the operator click into the
      Scan Splitting and Scan stages even when there is no saved map,
      no completed coverage plan, and no published waypoints. Touched
      sites all reference the constant: `onNextClicked` (×2),
      `updateStageSteps` (`plan_ready` / `scan_ready`),
      `updateFooter` (`plan_ready` / `scan_ready`), and the two
      stage-step chip click handlers. Flip the constant to `false` to
      restore the proper preconditions in one line. The internal
      *operation* guards (`onPublishSelectedClicked`,
      `onStartSelectedClicked`) are intentionally left in place — they
      stop you from publishing/starting nothing, even when navigation
      is open.
- [x] **Autonomy handoff.** Scan Splitting (Stage 3 of 4 within the planner
      workflow) now owns the autonomy handoff. `PlannerScreen::onPublishSelectedClicked`
      and `onStartSelectedClicked` emit `publishScanSegmentsRequested` and
      `startScanSegmentsRequested`, which `AppShellWindow` forwards to the
      `/f2c_waypoints` `Float64MultiArray` publisher. A `[0.0]` payload
      signals "start navigation" to `mpc_accel_autonomous_controller` (see
      `pilot_ws/src/pilot_control/scripts/mpc_accel_autonomous_controller.py`).
- [ ] **Manual progression dialog.** Manual mode on the Scan Splitting page
      currently renders a "coming soon" status message. Target behavior:
      once scanning starts in Manual mode, after each segment completes the
      OCU must prompt the operator (modal dialog) for permission to begin
      the next segment. Requires a per-segment completion signal from the
      controller (TBD — coordinate with `pilot_control`).
- [x] **Stage 4 "Scan" execution screen.** Built as a new sub-page inside
      `PlannerScreen` (`PlannerStep::Scan`). Layout: left rail = Current
      Segment / Overall Progress / Telemetry cards, center = live map +
      Start/Pause + Emergency Stop control bar, right rail = Segment Status
      list + Scan Statistics card. See **Stage 4 dev short-circuits** below
      for what is still mocked.

### Stage 4 dev short-circuits (`cpp/src/planner_screen.cpp`)

- [ ] **Scan progress source.** `PlannerScreen::onScanTick` (1 Hz timer)
      synthesizes per-segment `progress_pct`, `quality_pct`, and the
      derived `scan_total_points_m` from segment lengths. Replace with
      real per-segment progress + quality telemetry once the controller
      publishes them. See `// BDR_REWIRE:` tags in `onScanTick`.
- [ ] **Per-segment completion topic.** Active segment advancement is
      driven by the synthetic timer above; a real "segment N complete"
      signal from `mpc_accel_autonomous_controller` is needed to drive
      `SessionCache::ScanSegment::completed` and
      `scan_active_segment_index`.
- [ ] **Emergency Stop wiring.** `PlannerScreen::scanStartRequested`,
      `scanPauseRequested`, `scanResumeRequested`,
      `emergencyStopRequested`, and `completeMissionRequested` are
      currently logged via `qInfo` in `AppShellWindow::ensureStage5()`
      and not routed anywhere. Wire to the controller (Emergency Stop
      should additionally disarm motors and abort the mission cleanly,
      not just pause).
- [ ] **Per-segment plot colors.** The center map on Stage 4 reuses the
      existing `PlotWidget` planned-path rendering (single color) plus
      the live robot pose marker. The Figma calls for completed
      segments in solid green, active segment in green-with-dashed-ahead,
      and pending segments dashed grey. Add a
      `PlotWidget::setScanSegmentsOverlay` API that accepts a list of
      `(path, status, color)` and draws them with status-dependent
      pens, then call it from
      `PlannerScreen::pushScanSegmentsToPlot`.
- [ ] **Scan-stage spinner.** `mission_planner_scan_segment_active.svg`
      is a *static* arc icon — the design intends an animated spinner.
      Replace with a `QPropertyAnimation`-rotated `QPixmap` (or a
      smaller variant of `BanterLoaderWidget`) when polish time is
      available.

## Cross-cutting cleanup (track here, not release-blocking)

- [ ] **Legacy `CoverageGUI`.** ~6,874 LOC in `cpp/src/coverage_gui.cpp`
      compiled but unused by `main.cpp`. Either delete, or guard behind a
      CMake option (`BDR_CP_LEGACY_GUI=OFF` by default).
- [ ] **Out-of-date docs.** `architecture_overview.md` and
      `revamped_architecture_blueprint.md` still describe a 3-stage flow and
      don't mention `ExplorationScreen` / `PlannerScreen`. Either update to
      match `cpp/CLAUDE.md` or fold them into a single source of truth.
- [ ] **Git.** `BDR_CP/` has no `.git` directory yet. Initialize (or nest
      under an existing repo) so PR / CI tooling can apply here.

---

## Pre-release verification

Run before tagging any release build:

```bash
rg -n 'BDR_REWIRE|Dev bypass|dev bypass|kEnable.*Passthrough' cpp/
```

Zero matches outside historical-context comments = bypasses fully cleared.

It is **strongly recommended** to unify these flags behind either:

- env vars read by a small `dev_flags.hpp` helper (e.g.
  `BDR_DEV_SKIP_LOGIN`, `BDR_DEV_SKIP_PREFLIGHT`, `BDR_DEV_MOCK_ROBOT`),
  or
- a CMake `BDR_DEV_MODE` option compiled only in Debug / RelWithDebInfo,

so flipping them off for a release is a one-line, auditable change rather
than a hunt across files.
