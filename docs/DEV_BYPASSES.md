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
      `CloudUploadDialog` (laptop → S3). Implementations live under
      `cpp/src/` but are **not** linked into `bdr_coverage_planner` until
      this is wired — re-add them to `GUI_SOURCES` in `cpp/CMakeLists.txt`
      (see comment above that block).
- [x] **Info-card values.** All four info-card values are now live.
  - [x] Firmware — wired to the OCU build version
    (`f2c_cpp::version::kAppSemver`, derived from
    `project(... VERSION X.Y.Z)` in `cpp/CMakeLists.txt`). Bumping that
    one line bumps the displayed version everywhere.
  - [x] Last Calibration — `DashboardScreen` runs an SSH `stat -c %Y`
    probe against `/R_DATA/tilt_calibration/tilt_correction_matrices_*.npz`,
    formats the mtime as relative time on the card with the absolute
    timestamp on hover. Probe runs on show + every 5 minutes while
    visible; uses the host/ssh_user resolved from `RobotRegistry`
    (matches the `SetupScreen` pattern). On SSH failure the card shows
    `unreachable` with the stderr in the tooltip; a missing
    calibration file shows `never`.
  - [x] Battery — `DashboardScreen` spawns `mosquitto_sub` via
    `QProcess` against the host from `RobotRegistry`, port `1883`,
    topic `pilot/battery/state` (overridable via `battery_mqtt_topic`
    / `battery_mqtt_port` `QSettings` keys). Card displays
    `<percent>  <STATE>` (`OK` / `LOW ≤25%` / `CRITICAL ≤12%` /
    `STALE`) with voltage + current in the tooltip. 2 Hz status timer
    detects staleness; subscriber respawns 5 s after death. Threshold
    constants and JSON schema match the legacy `CoverageGUI` monitor
    so operators see identical state labels across both planners.
    Robot side: see `pilot_ws/src/pilot_control/docs/battery_mqtt_setup.md`.
  - [x] Uptime — `main.cpp` stamps `QApplication` with
    `kOcuStartEpochMsProperty` (epoch ms) right after `QApplication`
    construction. `DashboardScreen::refreshUptimeDisplay` formats
    elapsed time (`Xd Yh Zm` / `Xh Ym` / `Xm Ys` / `Xs`) and refreshes
    on the same 1 Hz timer as the System Status card while Stage 3 is
    visible; tooltip shows absolute local start time.
- [x] **Status card live values.** All four top-row cards are now
      driven by live signals.
  - [x] System Status — `DashboardScreen::refreshSystemStatusCard`
    rolls up {preflight, battery, robot-reachability} into
    INITIALIZING / READY / WARNING / NOT READY. Reachability proxy =
    fresh MQTT battery payload (zero extra probe overhead, ~5 s
    detection latency). Battery threshold for NOT READY is `<20%`
    (separate from the legacy MQTT card thresholds of 12 / 25).
    Preflight WARN is plumbed end-to-end: `StartupScreen` now tracks
    `has_any_warn` alongside `has_any_fail` and surfaces a three-state
    `overall_status_` ({READY, WARN, FAIL}); `AppShellWindow::goToStage3`
    forwards via `DashboardScreen::setPreflightResult`. Three-state
    OR-logic: NOT READY beats WARNING beats READY.
  - [x] Battery (top card) — same live MQTT subscriber feeding the
    lower System Information row. Displays just the percent; card
    title already says "Battery", so state is conveyed by color
    (#009966 nominal, #E17100 warn, #E74C3C critical).
  - [x] Total Scans — `DashboardScreen` reuses the same SSH session
    that probes Last Calibration to also `find /R_DATA -maxdepth 2
    -type d -name 'Section_*' | wc -l`. Single round-trip returns
    `<cal_mtime> <total_scans> <scans_since_cal>`.
  - [x] Next Calibration — soft policy `kCalibrationDueAfterScans = 3`
    in `dashboard_screen.cpp` (single edit point). Renders
    `"N scans"` while under the limit, flips to `"Due now"` plus a
    pulsing `QGraphicsOpacityEffect` blink once exceeded. Card is
    clickable and routes straight to `onCalibrateTiltRequested()` so
    the operator never has to hunt for the quick-action button.

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
- [x] **Manual progression dialog.** Manual mode on the Scan Splitting
      page is wired (`PlannerScreen::onProgressionModeChanged`,
      `planner_screen.cpp:5131-5142`). The "coming soon" placeholder
      copy has been replaced with operator-facing instruction text.
      Per-segment confirmation prompts still depend on a real
      "segment N complete" signal from the controller — tracked under
      *Per-segment completion topic* below.
- [x] **Stage 4 "Scan" execution screen.** Built as a new sub-page inside
      `PlannerScreen` (`PlannerStep::Scan`). Layout: left rail = Current
      Segment / Overall Progress / Telemetry cards, center = live map +
      Start/Pause + Emergency Stop control bar, right rail = Segment Status
      list + Scan Statistics card. See **Stage 4 dev short-circuits** below
      for what is still mocked.

### Stage 4 dev short-circuits (`cpp/src/planner_screen.cpp`)

- [x] **Scan progress source.** `PlannerScreen::setLiveRobotTelemetry`
      (`planner_screen.cpp:1672-1753`) projects the live robot pose onto
      the active segment polyline and updates
      `seg.progress_pct = max(prev, fraction_along * 100)`. A path-hint
      cache (`scan_active_segment_path_hint_`) avoids re-scanning the
      whole polyline on every odometry tick. `onScanTick`
      (`planner_screen.cpp:10017`) is now just an elapsed-time
      accumulator + UI refresh — no synthesis. Quality is computed by
      `maybeScheduleScanQualityUpdate` (line 9937) via
      `computeReprojectionQualityPercent(path, trail)`, gated on the
      robot having actually passed the first waypoint of the active
      segment, EWMA-smoothed (`0.7*old + 0.3*sample`). The Scan Statistics
      **Points** row (formerly `distance × 0.28` millions heuristic) is
      removed from the UI until a trustworthy LiDAR point-rate signal exists.
- [x] **Per-segment completion topic.** Active-segment advancement is
      driven by the controller's two-phase scan-execution status topic,
      not a timer. `AppShellWindow::onPlannerScanExecutionStatus`
      (`app_shell.cpp:3181-3198`) parses `segment_complete` (last
      waypoint reached) and `segment_saved` (DC end-and-save returned,
      actuator retracted) and forwards to `notifyScanSegmentCompleted`
      / `notifyScanSegmentSaved` on `PlannerScreen`.
- [x] **Emergency Stop wiring.** Wired in
      `AppShellWindow::onPlannerEmergencyStopRequested`
      (`app_shell.cpp:1662-1677`): engages by disarming via
      `sendExplorationAxisStateRequest(IDLE)`, pausing via
      `planner_dc_pause_client_`, and clearing autonomy via
      `publishPlannerAutonomyEnable(false)`; clears by reversing the
      same three steps. `onPlannerCompleteMissionRequested`
      (`app_shell.cpp:1679+`) orchestrates motors-idle → finalize →
      teardown → return to Stage 3.
- [x] **Per-segment plot colors.** Implemented as a status-driven
      overlay layered on top of the existing `PlotWidget` segment
      render. New API: `PlotWidget::setScanSegmentsOverlay(statuses,
      active_progress_pct)` (`plot_widget.hpp` /
      `plot_widget.cpp`). When `statuses.size() ==
      scan_segments_.size()` the paint pass switches palettes:
      Completed = solid green `#10B981`, Active = solid green up to a
      polyline split point interpolated from
      `scan_active_progress_pct_` + dashed grey `#9F9FA9` ahead,
      Pending = dashed grey, Unselected = faded dashed grey. Empty
      `statuses` reverts to the planning-stage palette.
      `PlannerScreen::refreshScanSegmentOverlay`
      (`planner_screen.cpp`) re-derives the statuses from
      `cache.scan_segments[].completed` / `scan_active_segment_index`
      / `scan_run_state` and is called both on geometry change
      (`pushScanSegmentsToPlot`) and on every odom tick
      (`setLiveRobotTelemetry`) so the active-segment split point
      slides smoothly with the robot.
- [x] **Scan-stage spinner.** Animated. `PlannerScreen::ensureScanSegmentSpinnerTimer`
      (`planner_screen.cpp:9475`) starts a 30 ms `QTimer` that ticks
      `tickScanSegmentSpinner` (line 9488), incrementing
      `scan_segment_spinner_angle_` by 12° and re-rendering via
      `loadRotatedSvgPixmap(":/assets/missionplanner/scan_segment_active.svg",
      16, 16, "#3B82F6", angle)` — full revolution every ~900 ms.
      Functionally equivalent to a `QPropertyAnimation`-rotated
      pixmap; timer stops when no active segment row remains so the
      label doesn't burn CPU off-screen.

## Cross-cutting cleanup (track here, not release-blocking)

- [x] **Legacy `CoverageGUI`.** Removed (`coverage_gui.hpp` /
      `coverage_gui.cpp` deleted). Recordings-related `.cpp` files are
      retained beside the tree but **excluded from `GUI_SOURCES`** until
      Dashboard wiring — see `cpp/CMakeLists.txt` comment above
      `GUI_SOURCES`.
- [x] **Out-of-date docs.** `architecture_overview.md` and
      `revamped_architecture_blueprint.md` deleted (they described a
      stale 3-stage flow). `cpp/CLAUDE.md` is now the canonical
      architecture reference, pointed to from `AGENTS.md`.
- [x] **Git.** Repo lives at
      `github.com/Building-Diagnostic-Robotics/BDR_OCU` with CI cutting
      `.deb` releases on every push to `main`.

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
