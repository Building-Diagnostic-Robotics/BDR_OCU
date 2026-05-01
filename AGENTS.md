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

- `cpp/src/setup_screen.cpp` — Stage 1 login is a dev bypass (no SSH/auth);
  `robot_login.cpp` is implemented but unused.
- `cpp/src/setup_screen.cpp` — ping target is a hardcoded
  `192.168.168.101`; not resolved from `RobotRegistry`.
- `cpp/src/startup_screen.cpp` — `kEnableLaunchDashboardPassthrough = true`
  forces Stage 2 Continue always-enabled regardless of preflight result.
- `cpp/src/app_shell.cpp` — `DashboardScreen::viewRecordingsRequested`
  signal is emitted but intentionally not connected yet.
- `cpp/src/planner_screen.cpp` — Manual progression mode on the Scan
  Splitting stage is a "coming soon" stub; Automatic mode is wired through
  `/f2c_waypoints` (Stage 4 execution screen still TODO).
- `cpp/src/dashboard_screen.cpp` — info-card values (firmware, last
  calibration, battery) are placeholders with no live data source.

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

The legacy `CoverageGUI` (`cpp/src/coverage_gui.cpp`, ~6,874 LOC) still
compiles but is not instantiated from `main.cpp`. Treat it as reference-only
until a decision is made on deletion vs. CMake-guard.

## Docs worth reading

- `cpp/CLAUDE.md` — authoritative architecture overview and build notes.
- `docs/DEV_BYPASSES.md` — the re-wiring checklist (see above).
- `docs/TILT_CALIBRATION_PLAN.md` — tilt calibration design + TODO list.
- `architecture_overview.md` and `revamped_architecture_blueprint.md` —
  **out of date** (still describe a 3-stage flow). Prefer `cpp/CLAUDE.md`.
