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

## Disconnect resilience (Stages 0-6, complete and production-wired)

The OCU detects radio loss (Zenoh-mediated ROS topics going stale)
within 2-5 s and surfaces it explicitly so the operator can never
mistake a frozen UI for a working scan. The robot side has matching
safety nets so data never gets lost when the OCU disconnects.

### Single source of truth

`cpp/include/link_health_monitor.hpp` (`LinkHealthMonitor`, owned by
`AppShellWindow`).  Stamped by every existing ROS callback:

- `Source::Odom` — `/Odometry_tilt_corrected_diff`
- `Source::UdcHealth` — `/udc/health`
- `Source::ScanStatus` — `/scan_segment_status`
- `Source::ControllerStatus` — `/{left,right}/controller_status`
- `Source::StreamStatus` — `/stream_status`
- `Source::FpvFrame` — non-ROS, RTP/UDP video pipeline (the
  `frameStampProbe` in `video_stream_widget.cpp` updates an atomic
  every frame; `AppShellWindow::onExplorationLiveSlowTick` polls it
  and stamps the source if a frame was delivered in the last 1 s).

State derivation: any source < 2 s old → Healthy; 2-5 s → Degraded;
all sources ≥ 5 s → Disconnected.  Armed at exploration launch start;
disarmed at teardown.

### OCU-side surfaces

- Top-bar **BOT pill** in both Stage 4 + Stage 5 (next to the existing
  Battery / Signal / REC pills).  States: BOT IDLE (grey, pre-launch),
  BOT LIVE (green), BOT LAGGY Xs (amber), BOT OFFLINE Xs (red).
- Inline disconnect **banner** at the top of Stage 5 (Scan).  Visible
  only while Disconnected.
- Stage 5 `scan_tick_timer_` is **paused** while Disconnected so the
  elapsed clock doesn't drift past the moment the radio dropped.  The
  cached `scan_elapsed_ms` is preserved so the displayed elapsed time
  reflects only "active" time across the disconnect.
- Pause/Resume, E-Stop, Cancel, Discard, Start Mapping, Finish + Save
  Map, and Stop Pipeline buttons all hard-disable while Disconnected
  with "Robot offline — wait for reconnect." tooltips.
- `onPlannerEmergencyStopRequested`, `onPlannerScanPauseRequested`,
  and `onPlannerScanResumeRequested` early-return on
  `isRobotLinkOffline()` and surface a `showCommandDroppedToast()`
  hint instead of optimistically mutating `planner_estop_active_`.
- On Disconnected → Healthy transition `onRobotLinkRecovered()`
  re-publishes `/mpc_autonomy_enable` to match the OCU's local
  intent (paused if `planner_estop_active_`, else armed).  Cheap and
  idempotent so the bot rebooting mid-disconnect lands back in the
  operator-intended config without a manual click.

### Data-first Complete Mission

`AppShellWindow::onPlannerCompleteMissionRequested` branches:

- Healthy / Degraded → `executeCompleteMissionNormalPath()` (the
  motor-disarm wait + `/dc/finalize_mission` RPC + teardown chain that
  has always run).
- Disconnected → `OfflineFinalizeDialog` (frameless, parented to the
  shell).  Three CTAs:
  - **Wait for reconnect** (default, primary).  Polls the link
    monitor; auto-accepts as `WaitReconnected` when it goes Healthy.
    A 5-minute auto-fallback to SSH-offline keeps the bot from sitting
    in `CLOSED_LOOP_CONTROL` indefinitely if the operator forgets the
    modal.  Live countdown shown.
  - **Finalize via SSH (offline)** runs
    `pilot_control/scripts/finalize_mission_local.py` over SSH which
    rescans `/R_DATA/<day>/<building>/Mission_HHMMSS/` and atomically
    writes `mission_finalized_at` + `finalized_via=ssh_offline` into
    `mission_config.json`.  Then runs the existing teardown SSH
    (kills the launch tree → motors disarm via the controller's exit
    handlers).
  - **Cancel** keeps the operator on Stage 5; the robot-side
    auto-finalize watchdog (10 min idle) is the safety net for an
    abandoned mission.

### Robot-side safety net (`data_collection_coordinator.py`)

- `update_json_file()` is now **atomic**: tmp + `os.replace()`.
  A power-loss / SIGKILL between write and rename leaves the previous
  valid JSON on disk.
- 1 Hz **heartbeat timer** stamps `last_heartbeat_at` (ISO-8601 wall
  clock) into `mission_config.json` while a mission is open.  Lets
  post-flight tooling and the OCU's reconnect path detect open-but-
  stale missions deterministically.
- **Auto-finalize watchdog**: if no `/dc/*` service activity for >=
  `mission_idle_timeout_sec` (default 10 min), the coordinator calls
  `finalize_mission_callback` itself and stamps `finalized_via:
  watchdog`.  Operator-abandoned, OCU-crashed, OCU-closed-without-
  Complete-Mission — all the same recovery path so on-disk data is
  always properly tagged.
- All operator-driven service handlers (`start`, `pause`, `resume`,
  `end_and_save`, `finalize_mission`, `cancel_scan`,
  `start_gnss_precapture`) call `mission_touch()` to bump the
  watchdog's idle clock.

### Rules for agents touching this path

- **Do NOT remove or "simplify"** any of the following — they are
  load-bearing:
  - `LinkHealthMonitor` class.
  - The persistent `frameStampProbe` in `video_stream_widget.cpp`
    (every-frame atomic store; freezes are otherwise invisible).
  - `OfflineFinalizeDialog` and the `Choice::WaitReconnected` /
    `FinalizeOverSsh` / `Cancelled` enum.
  - `finalize_mission_local.py` and the `finalized_via` JSON field.
  - The `mission_heartbeat_callback` + `mission_touch` plumbing.
- The **5-minute auto-fallback** in `OfflineFinalizeDialog::kAutoFallbackMs`
  is the user-locked safety ceiling — don't change without operator
  signoff.
- The Disconnect threshold is **5 s** in `LinkHealthMonitor::kDegradedMaxMs`.
  Tunable, but dropping below ~3 s produces false positives on Zenoh
  reconnects after brief radio fades.
- When adding a new ROS subscriber on the AppShell side, **always**
  stamp the appropriate `LinkHealthMonitor::Source` from its callback
  — otherwise the link monitor will go OFFLINE during a perfectly
  healthy session that happens to use only that new topic.

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

## Mission metadata + units (production-wired)

Operator-driven session metadata captured via the **New Scan
Information** modal (`MissionMetadataDialog`) on Stage 3 Dashboard
**Start New Scan**. Building name, operator name, and a Metric/ANSI
units toggle are persisted to `QSettings` and pushed to the robot's
`/data_collection_coordinator` as ROS string parameters before
autonomy arms.

### Key entry points

- `cpp/include/components/mission_metadata_dialog.{hpp,cpp}` — the
 frameless modal (slugifies building name, persists to `QSettings`,
 styled to match `TiltCalibrationDialog`).
- `cpp/src/app_shell.cpp` — `onStartNewScan` (intercept point,
 applies `QGraphicsBlurEffect` to Stage 3) and
 `sendDataCollectorSessionMetadata` (the `SetParameters` push).
- `cpp/include/units_system.hpp` + `cpp/src/units_system.cpp` —
 `UnitsProvider` singleton + `units::format{Length,Speed,Area}`
 namespace helpers. **Display-only**; SI everywhere else.
- `cpp/include/settings_constants.hpp` — `kSettingsBuildingNameKey`,
 `kSettingsOperatorNameKey`, `kSettingsUnitsKey`.
- `pilot_ws/src/pilot_control/scripts/data_collection_coordinator.py`
 — robot-side consumer of the three ROS params; mirrors slugify
 logic in Python.
- `pilot_ws/src/pilot_control/config/zenoh/zenohd_robot.json5` —
 `service_servers` allowlist must contain
 `^/data_collection_coordinator/set_parameters{,_atomically}$`.

### Data layout (current)

```
/R_DATA/<Month_DD_YYYY>/<building_slug>/Section_<N>_<HHMMSS>/
                                       ├── Visual_data/
                                       ├── GPR_scan_data/
                                       ├── rosbag_*/
                                       ├── *_map_*.pcd
                                       └── session_config.json
/R_DATA/<Month_DD_YYYY>/<building_slug>/Mission_<HHMMSS>/
                                       ├── GNSS_data/rover_*.ubx
                                       └── mission_config.json
```

`session_config.json` and `mission_config.json` carry
`building_name`, `building_slug`, `operator_name`, `units_preference`.

### Path consumers updated for the building tier

- `cpp/src/dashboard_screen.cpp` — Total Scans / Next Calibration
 SSH probe uses `find /R_DATA -mindepth 3 -maxdepth 3 -type d
 -name 'Section_*'`. Pre-modal depth-2 sections are not counted.
- `cpp/src/transfer_manager.cpp` — `fetchSectionsForDate` SSH
 script walks `for b in */; do for d in "$b"Section_*/`,
 populates `SectionInfo.buildingSlug`. Per-row format is
 11 fields (`building|name|size|count|mtime|...`).
- `cpp/src/cloud_upload_manager.cpp` — `scanLocalData` recognizes
 three layouts (Flat, Dated, DatedBuilding) via two helper
 lambdas (`looksLikeSection`, `buildSection`).
 `verifyUploadedSections` builds the matching S3 URL shape
 (`s3://bucket/prefix/<date>/<building>/<section>/`).
 `ScanMetadata::toJson` emits a `"building"` field (omitted when
 empty so legacy uploads stay clean).

### Rules for agents touching this path

- **Never concatenate a hardcoded unit suffix** (`" m"`, `" m/s"`,
 `" m²"`, `" ft"`) in display strings. Always go through
 `units::formatLength` / `formatSpeed` / `formatArea` /
 `lengthUnitSuffix`. The toggle is global and operators flip it
 between missions without restarting the OCU.
- **Never convert units before sending to the robot.** Only the
 display layer is unit-aware; ROS payloads, `QSettings` values,
 scan plan data, and the autonomy stack remain SI.
- For widgets that hold static endpoint labels (e.g.
 `PlannerScreen` slider min/max badges), capture the `QLabel*`
 in a member vector and connect to `UnitsProvider::unitsChanged`
 to re-render — the screen is constructed once per OCU run and
 reused across missions, so static text set in the constructor
 will go stale after a toggle.
- The `building_slug` Python helper in
 `data_collection_coordinator.py` and the C++ `slugify` in
 `MissionMetadataDialog` MUST stay byte-for-byte equivalent.
 If you change one, change both — the OCU and robot agree on
 the path purely by convention.
- `sendDataCollectorSessionMetadata` is the **arming gate**.
 Anything new that fires after Stage 5 Start Scan should hang
 off the `on_complete(true)` callback in
 `onPlannerScanStartRequested`, never in parallel — a failed
 push must hard-block autonomy.

## Docs worth reading

- `cpp/CLAUDE.md` — authoritative architecture overview and build notes.
- `docs/DEV_BYPASSES.md` — the re-wiring checklist (see above).
- `docs/OTA.md` — OTA state machine, runner UX, field-test recipe.
- `docs/TILT_CALIBRATION_PLAN.md` — tilt calibration design + TODO list.
