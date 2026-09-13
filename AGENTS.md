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

### OCU-side surfaces (layered connectivity model)

The OCU combines **three independent signals** into the link state,
plus a Zenoh transport-layer tune so the worst-case "topics stale but
host is up" window is short enough to not trip the operator:

1. **ROS topic freshness** — `LinkHealthMonitor` stamps every
   incoming odom / udc_health / scan_status / controller_status /
   stream_status / fpv_frame callback. Stale-threshold is **10 s**
   (`LinkHealthMonitor::kStaleMaxMs`).
2. **Network reachability** — `RobotReachabilityProbe` runs ICMP
   (`ping -c 1 -W 1 <robot_host>`) on a 1 s cadence with TCP-22
   fallback via `QTcpSocket`. Pushed into the monitor via
   `setReachability()` after a 2-tick debounce.
3. **FPV proof-of-life** — RTP/UDP video frames arriving at EITHER
   `ExplorationScreen::lastFpvFrameWallMs()` OR
   `PlannerScreen::lastScanFpvFrameWallMs()` count as a
   `Source::FpvFrame` stamp. Camera RTP keeps flowing through brief
   Microhard fades (it's stateless), so as long as ANY frames are
   decoding the bot is verifiably alive end-to-end, regardless of
   Zenoh peer state. Stamped on every 1 Hz slow tick if the most
   recent frame is < 2 s old. **Must read from BOTH stages** — pre-
   fix the slot only checked Stage 4, so once the operator advanced
   to Stage 5 the FPV stamp went stale instantly even with the camera
   live, producing false-positive RECONNECTING flap (terminal log
   2026-05-12T05:41).

Combining them resolves four states:

- `Healthy` — topics fresh (always treated as connected).
- `Reconnecting` — topics stale **but** probe says host is reachable.
  Operator-facing pill text is `BOT LIVE - SYNCING X s` because
  with the FPV stamp in place we'd typically only land here when
  RTP video has ALSO stopped — an honest "all our channels are
  briefly out, but the network is up" signal.
- `Disconnected` — topics stale, probe failed, FPV gone. True offline.
- `Idle` — pre-arm.

#### Surfaces

- Top-bar **BOT pill** in both Stage 4 + Stage 5 (next to the existing
  Battery / Signal / REC pills). States: BOT IDLE (grey, pre-launch),
  BOT LIVE (green), BOT LIVE - SYNCING Xs (amber), BOT OFFLINE Xs
  (red). Both SYNCING and OFFLINE tick an "Xs" counter so the
  operator can see whether the recovery is making progress.
- Inline disconnect **banner** at the top of Stage 5 (Scan), and the
  amber map-halo on the `PlotWidget` / `ExplorationNavMapWidget` —
  shown ONLY in true `Disconnected`. RECONNECTING deliberately does
  not show banner / halo.
- Stage 5 `scan_tick_timer_` is NOT paused on link loss — operator
  wants the elapsed clock to keep ticking against real wall-clock.
  Stale telemetry is communicated via the per-widget grey-out
  (opacity 0.55) applied on every value derived from robot data.
- Pause/Resume, Cancel, Discard, Start Mapping, Finish + Save Map,
  Stop Pipeline, Complete Mission all hard-disable in BOTH
  `Reconnecting` and `Disconnected`. Tooltips swap between
  "Robot reconnecting…" and "Robot offline — wait for reconnect."
  based on `link_reachable_`.
- **E-STOP BYPASS**: the Emergency Stop button stays enabled in
  `Reconnecting` and is only disabled in true `Disconnected`. E-Stop
  is the one command where dropping it is unacceptable — even if the
  RPC lands 5-10 s late after Zenoh re-handshakes, that's better
  than refusing to send it. Both the screen-side gate (`updateScanRunUi`
  in `cpp/src/planner_screen.cpp`) and the AppShell-side gate
  (`onPlannerEmergencyStopRequested`) use `isRobotLinkUnreachable()`
  (strict) instead of `isRobotLinkOffline()`.
- `isRobotLinkOffline()` returns true for Reconnecting + Disconnected
  (general RPC gate). `isRobotLinkUnreachable()` returns true ONLY for
  Disconnected (used by the Complete Mission flow + the E-Stop bypass
  above).
- `onPlannerEmergencyStopRequested`, `onPlannerScanPauseRequested`,
  and `onPlannerScanResumeRequested` early-return on
  `isRobotLinkOffline()` and surface a `showCommandDroppedToast()`
  hint instead of optimistically mutating `planner_estop_active_`.
- `onRobotLinkRecovered()` is log-only on Disconnected → Healthy —
  the controller's heartbeat path (`mpc_accel_autonomous_controller.py`
  `_check_heartbeat_safety` / `_maybe_resume_dc`) owns autonomy
  resync. Re-publishing from the OCU here would race with that path
  and could clobber operator intent.

### Data-first Complete Mission

`AppShellWindow::onPlannerCompleteMissionRequested` branches on
`isRobotLinkUnreachable()` (strict — only true Disconnected):

- Healthy / Reconnecting → `executeCompleteMissionNormalPath()` (the
  motor-disarm wait + `/dc/finalize_mission` RPC + teardown chain that
  has always run). The screen-side button gating means the operator
  can't actually press Complete Mission while in Reconnecting; the
  strict check here is defence-in-depth.
- Disconnected → `OfflineFinalizeDialog` (frameless, parented to the
  shell). Three CTAs:
  - **Wait for reconnect** (default, primary).  Polls the link
    monitor; auto-accepts as `WaitReconnected` when it goes Healthy.
    A 5-minute auto-fallback to SSH-offline keeps the bot from sitting
    in `CLOSED_LOOP_CONTROL` indefinitely if the operator forgets the
    modal.  Live countdown shown.
  - **Finalize via SSH (offline)** runs
    `pilot_control/scripts/finalize_mission_local.py` over SSH which
    rescans `/R_DATA/<day>/<building>/Mission_HHMMSS/` and atomically
    writes `mission_finalized_at` + `finalized_via=ssh_offline` into
    `mission_config.json`. Invoked **directly via `python3`** (NOT
    `ros2 run pilot_control …`) — non-interactive SSH doesn't source
    the ROS env so `ros2` isn't on PATH; the script is pure file I/O
    and doesn't import `rclpy`, so direct invocation works.
    Then runs the existing teardown SSH (kills the launch tree →
    motors disarm via the controller's exit handlers).
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

### Programmatic Seek thermal USB reset (production-wired)

Wedged-thermal-SDK recovery without operator unplug.  The Seek
SDK locks up if the previous UDC process was killed mid-stream
without `seekcamera_manager_destroy()`; the next UDC opens the
device, never receives `SEEKCAMERA_MANAGER_EVENT_CONNECT`, and
silently records empty visual frames.  Pre-fix the only recovery
was a physical USB replug — impossible once Roofus is buttoned up.

The recovery path:

1. **UDC in-process detectors** (`src/unified_data_collector.cpp`).
   Two triggers, both call `requestExitForUsbReset()` →
   `rclcpp::shutdown()` → `main()` returns
   `kExitThermalNeedsReset = 75`.
   - **Connect watchdog**: 5 s timer; fires if no `CONNECT`
     event by then. Catches the startup-wedge case.
   - **Runtime drop-rate detector**: 5 s timer; counts as a
     "stuck window" iff `recording_active && !paused &&
     drops_thermal_delta >= 10 && rows_delta == 0`. Two
     consecutive stuck windows trigger the exit. Catches the
     *post-CONNECT* wedge that the operator originally hit
     ("second scan in same OCU session — empty CSV").
   - `~UnifiedDataCollector` runs synchronously in `main()`
     (`node.reset()` before `rclcpp::shutdown()`) so
     `seekcamera_manager_destroy()` actually fires before the
     supervisor's reset — without it the next UDC re-wedges
     on the same dead handle.

2. **Supervisor** (`scripts/udc_supervisor.py`).
   - On rc=75 invokes `seek_usb_reset.py`, increments
     `_usb_resets_used`, sleeps `USB_RESET_COOLDOWN_SECONDS = 3`,
     respawns UDC. Does **not** count toward
     `MAX_CRASHES_IN_WINDOW` (USB-reset cycles are recoveries,
     not crashes).
   - Capped at `MAX_USB_RESETS_PER_LAUNCH = 2`. After that
     publishes `DEAD_USB_RESET_FAILED` (distinct from the
     existing `DEAD_MAX_RESTARTS`) so the OCU can show the
     "physically inspect cable" message.
   - Runs ONE proactive reset before first spawn, best-effort.
     Pre-empts the post-prior-session wedge case at ~500 ms cost.
     Does not consume the per-launch budget.

3. **Reset script** (`scripts/seek_usb_reset.py`).
   Finds Seek device by VID `289d` under `/sys/bus/usb/devices/`,
   toggles per-device `authorized` 1→0→1, polls re-enumeration.
   Tries direct write first, falls back to `sudo -n tee` (so the
   path works even if udev rule isn't installed; just pays a
   sudo NOPASSWD requirement instead).

4. **Udev rule** (`udev/99-bdr-seek-thermal.rules` +
   `scripts/install_seek_udev_rule.sh`).
   Grants `roofus` group write to per-device `authorized` so the
   reset script never needs sudo. One-time operator install via
   `ros2 run pilot_control install_seek_udev_rule.sh`.

### Rules for agents touching this path

- **Do NOT remove or "simplify"** any of the following — they are
  load-bearing:
  - `LinkHealthMonitor` class (states: Idle / Healthy /
    Reconnecting / Disconnected — do NOT re-introduce the dropped
    Degraded/LAGGY tier).
  - `RobotReachabilityProbe` and its ICMP → TCP-22 fallback chain.
    Removing the probe regresses the OCU to single-signal freshness
    and the false-positive offline flapping returns.
  - The persistent `frameStampProbe` in `video_stream_widget.cpp`
    (every-frame atomic store; freezes are otherwise invisible).
  - `OfflineFinalizeDialog` and the `Choice::WaitReconnected` /
    `FinalizeOverSsh` / `Cancelled` enum.
  - `finalize_mission_local.py` and the `finalized_via` JSON field.
  - The `mission_heartbeat_callback` + `mission_touch` plumbing.
  - `seek_usb_reset.py`, the `99-bdr-seek-thermal.rules` udev rule,
    and the `kExitThermalNeedsReset = 75` ↔
    `EXIT_THERMAL_NEEDS_USB_RESET = 75` constant pair (UDC ↔
    supervisor wire).
  - The `node.reset()` BEFORE `rclcpp::shutdown()` in UDC's
    `main()` — without it, the wedged Seek SDK is never destroyed
    and the supervisor's reset can't recover.
- The **5-minute auto-fallback** in `OfflineFinalizeDialog::kAutoFallbackMs`
  is the user-locked safety ceiling — don't change without operator
  signoff.
- The single freshness threshold is **10 s** in
  `LinkHealthMonitor::kStaleMaxMs`. Tighter values bring back the
  Zenoh-rediscovery false positives that motivated the layered model.
- Zenoh transport-layer keepalive lives in BOTH
  `pilot_ws/src/pilot_control/config/zenoh/zenohd_laptop.json5` AND
  `zenohd_robot.json5` (`transport.link.tx.lease = 4000`,
  `keep_alive = 4`). They MUST stay symmetric — if only one side runs
  aggressive keepalive the other can leave a stale socket in
  CLOSE_WAIT for 30-60 s and reject the laptop's reconnect. Don't
  drop `lease` below ~3 s without a field test; Microhard jitter on
  busy 4-channel links can swallow a single keepalive and cascade
  into reconnect thrashing on a perfectly healthy link.
- The laptop-side `connect.retry { period_init_ms: 1000,
  period_max_ms: 4000, period_increase_factor: 2 }` block is what
  brings the post-fade recovery window from ~30-60 s down to ~5-8 s.
  `timeout_ms: -1` + `exit_on_failure: false` are load-bearing —
  removing them lets a startup fade kill `zenohd` entirely and the
  OCU loses ALL ROS comm with no recovery path.
- The FPV proof-of-life stamp (slow-tick block in `app_shell.cpp`)
  MUST read from BOTH `stage4_->lastFpvFrameWallMs()` and
  `stage5_->lastScanFpvFrameWallMs()` — using only one stage
  produces false-positive RECONNECTING the moment the operator
  changes screens.
- When adding a new ROS subscriber on the AppShell side, **always**
  stamp the appropriate `LinkHealthMonitor::Source` from its callback
  — otherwise the link monitor will go OFFLINE during a perfectly
  healthy session that happens to use only that new topic.
- The `RobotReachabilityProbe` arms on the same host
  `startRobotCompleteLaunch` SSHs to (resolved via
  `RobotRegistry`/`robots.json`). If you add a new "select different
  robot" path, make sure you re-arm the probe on the new host —
  otherwise the layered model will forever probe the wrong IP.
- The SSH "Finalize via SSH" path **must not** invoke
  `ros2 run pilot_control finalize_mission_local.py` —
  non-interactive SSH doesn't source the ROS env, so `ros2` isn't on
  PATH and you'll get rc=127. Always invoke
  `python3 /home/<ssh_user>/pilot_ws/install/pilot_control/lib/pilot_control/finalize_mission_local.py`
  directly. The script has no `rclpy` imports.

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

## Upload pipeline (Stage 3 Dashboard, production-wired)

Operator-driven cloud upload. Replaces the legacy
`CloudUploadDialog`/`DataTransferDialog` pair (which orchestrated
`aws s3 sync` from the laptop with IAM credentials in `~/.aws/`). The
new flow runs **robot → S3 directly** via presigned URLs minted by the
BDR backend's API Gateway + Lambda; the laptop holds zero AWS state and
just observes progress over SSH.

### Topology

- **Robot side** (`pilot_ws/src/pilot_control/scripts/uploader.py`):
  Python 3 script that walks a single section/mission folder, calls
  `POST /presign` per file, PUTs to the returned S3 URL, writes
  `upload_state.json` after each file (atomic tmp + `os.replace`),
  generates `manifest.json`, and finally calls `POST /complete`. Reads
  `BDR_CLOUD_API_BASE` / `BDR_CLOUD_CLIENT_ID` /
  `BDR_CLOUD_DEVICE_TOKEN` / `BDR_UPLOAD_WORKERS` from the environment
  so the OCU can push per-robot creds via the SSH `env(1)` prefix
  without editing the script per laptop.
- **OCU side** (`cpp/include/upload_runner.hpp` +
  `cpp/include/upload_dialog.hpp`): two helpers + a frameless
  `UploadDialog` reachable from the Stage 3 "Upload Data" quick-action
  card.
  - `UploadStateProbe` — one-shot SSH `find /R_DATA -mindepth 3
    -maxdepth 3` walk that emits `SECTION|<date>|<building>|<section>|
    <state>|<completed>|<total>|<size>` lines. State derives from
    on-robot sentinels: `manifest.json` present → Done,
    `upload_state.json` present → Partial, neither → None.
  - `UploadRunner` — sequential queue driver. One `QProcess` per
    target running `ssh -tt user@host env … python3 -u uploader.py
    <data_root> <robot_id> <run_id>`. Stdout is parsed line-by-line
    for `^✓ Uploaded:`, `^Skipping already uploaded:`, `^Connection
    error:`, `^Manual pause detected.`, `^Unexpected error:`, etc.,
    and emitted as Qt signals.

### Decisions baked into the design

- **Robot-side, not laptop-side.** Robot has direct internet on base
  wifi. Skips the rsync robot→laptop hop entirely. Laptop reimage
  loses nothing because all state (`upload_state.json`,
  `manifest.json`) lives on robot disk.
- **Per-section `run_id`.** `run_id = "<date>/<building>/<section>"`
  (or `Mission_HHMMSS`). Atomic — one section failure ≠ whole-day
  failure. S3 layout `<client_id>/<robot_id>/<run_id>/<relpath>` mirrors
  on-disk reality 1:1.
- **Single PUT only.** Backend mints single-PUT presigns; per-file
  ceiling is 5 GB. Roofus rosbags / PCDs are sized below this; if
  that ever stops being true the backend has to add multipart presigns.
- **Sequential targets, parallel files within a target.** Script's
  internal `UPLOAD_WORKERS=12` provides per-section parallelism. OCU
  walks the queue one section at a time so the dialog has a single
  progress story.
- **Per-file progress.** Stdout `^✓ Uploaded:` line per file is the
  unit of progress — the bar updates once per file completion. Per-
  byte streaming is a future option (would require chunked PUT in
  `uploader.py` + a callback signal); not needed for v1.
- **Manifest schema is strict.** `{client_id, robot_id, run_id,
  files[]}` only. Operator-entered metadata
  (`session_config.json`, `mission_config.json`) ships as ordinary
  files in the section folder so backend can index without
  schema-migrating the manifest.
- **State on robot, not laptop.** No backend `GET /manifest`
  endpoint required. The probe re-derives Done/Partial/None from
  on-disk truth on every dialog open.
- **Hard-blocked while a scan is alive.** `onUploadDataRequested`
  reuses the same `launch_active` composite the close-event guard
  uses (`exploration_launch_in_progress_ ||
  exploration_launch_ready_ || laptop_launch_started_ ||
  robot_launch_started_`). Operators must Complete Mission first.

### Configuration

- `cpp/config/robots.json` carries:
  - top-level `cloud_api_base` (global; one URL across the fleet)
  - per-robot `cloud_client_id` + `cloud_device_token`
- An optional sibling `cloud_config.json` with `{"cloud_api_base":
  "..."}` overrides the top-level key — useful for dev installs.
- Compiled-in fallback in `kDefaultCloudApiBase` keeps the OCU usable
  on a fresh checkout.
- `RobotProfile::cloud_client_id` / `cloud_device_token` are read by
  `AppShellWindow::onUploadDataRequested`; missing values produce a
  `BdrMessageBox` and refuse to open the dialog.

### Pause / cancel

- **Pause** SSH-touches `<remote_path>/pause.flag`. The script polls
  for it and exits cleanly at the next file boundary. Resume = press
  Upload again with the same selection — script re-reads
  `upload_state.json` and skips completed files.
- **Cancel** SIGTERMs the SSH process. State on disk is preserved
  (writes are post-file), so the next run resumes cleanly.
- **Closing the dialog while busy** routes through `requestPause()`
  for graceful resume — see `UploadDialog::closeEvent`.

### Rules for agents touching this path

- **Do NOT remove or "simplify"** any of the following — they are
  load-bearing:
  - `uploader.py`'s atomic state writes (`os.replace`).
  - `python3 -u` in the SSH invocation (without it the OCU's stdout
    parser stalls 4 KB at a time and operators see no progress).
  - The SSH `find -mindepth 3 -maxdepth 3` probe — depth-3 is the
    canonical layout (`<date>/<building>/<section>`).
  - `RobotRegistry::cloudApiBase()` and the three-tier resolution
    (top-level → sibling cloud_config.json → compiled-in fallback).
- **Do NOT downgrade the launch-active gate** in
  `onUploadDataRequested` — uploads during a live scan would race
  with `unified_data_collector` writes.
- **Do NOT add IAM credentials to the OCU.** This is the security
  posture the new design exists to enforce.
- The script invocation **must NOT** go through `ros2 run` — non-
  interactive SSH doesn't source the ROS env, same gotcha as
  `finalize_mission_local.py`. Always invoke via direct
  `python3 /home/<ssh_user>/pilot_ws/install/pilot_control/lib/pilot_control/uploader.py`.

## FAST-LIVO2 migration (planned, not yet built)

Goal: replace FAST-LIO2 with FAST-LIVO2 for more robust odometry
(visual constraints in LiDAR-degenerate roof geometry) and an
RGB-colorized PCD deliverable. Full study + ordered plan in
`pilot_ws/src/pilot_control/docs/FASTLIVO2_CAMERA_NOTES.md`; vendor
camera PDFs in `pilot_ws/src/pilot_control/docs/vendor/see3cam_24cug/`.

Facts agents should not re-derive:

- BDR owns **6× See3CAM_24CUG_CHL_TC** (global shutter AR0234CS, M12,
  no enclosure — the ONLY variant with hardware trigger support).
  TRIG pin 4 (active-high, 3.3 V-drivable, ≥10 µs) + STROBE pin 5
  (open-drain actual-exposure output) on the CN6 header.
- **Trigger mode disables free-run streaming** → one camera cannot be
  both FPV and hardware-triggered VIO. Plan: left camera stays
  UDC-owned MJPEG FPV untouched; a **dedicated VIO camera** (spare
  unit, rigid LiDAR-adjacent mount) publishes UYVY `image` +
  `camera_info` for LIVO2. No UDC tee refactor needed.
- LIVO2 feed must be **UYVY, never MJPEG** (JPEG artifacts break the
  direct photometric method), which requires USB 3.2 Gen 1
  enumeration — verify `lsusb -t` shows 5000M on the robot port.
- Lock AE/AWB/gain via UVC controls during scans; stock lens is ~128°
  diagonal — pick a narrower (~70–90°) M12 lens BEFORE intrinsic
  calibration.
- Cut-over contract: remap LIVO2 outputs to `/Odometry` +
  `/cloud_registered` in `camera_init` so the tilt-corrector chain,
  costmaps, and nav-grid stay untouched; keep an XYZI cloud for
  costmap/nav-grid consumers (color only for the deliverable).

## Stage 6 — ROI Coverage (Measured / Satellite), branch `feature/satellite-roi-stage`

Autonomous ROI coverage per `docs/AUTONOMY_CONOPS.md` Mode B. **Start New
Scan no longer routes to Stage 4/5** — it opens `ScanSetupDialog` (saved
plans unexecuted-first, or Measured/Satellite mode cards), then the
metadata modal (building prefilled from the chosen plan), then
`SatelliteScreen` (Stage 6). The Dashboard "Plan Job" card opens the same
screen in planning-only trim. Classic Stage 4/5 remain in-tree, unrouted.

### Key files (all `satellite_*` prefixed, flat in cpp/include + cpp/src)

- `satellite_screen.{hpp,cpp}` — the stage. Stage 4/5 top-bar construction
 (SVG back button, battery/BOT/state pills, motors chip, 184px
 window-controls reservation), 320px LEFT rail of zinc cards, per-element
 Arimo, object-name-scoped QSS ONLY (a repolish sweep in applyTheme is
 load-bearing: replacing an ancestor stylesheet does not reliably
 repolish the QScrollArea subtree).
- `satellite_map_widget.{hpp,cpp}` — one canvas, two modes: Esri imagery
 (overzoom fallback: failed tiles remembered 60 s, parent-fetch cascade,
 ancestor scaling — imagery 404s past native LOD) or measured CAD grid
 (adaptive metric grid, 0.1 m snap, z23 ceiling, fixed reference-origin
 anchor). The authored geometry is an **arbitrary polygon** (`RoiPolygon`,
 ground meters + geo anchor): "Draw Shape" appends a vertex per click and
 right-click closes it, vertices drag individually, each edge carries a
 unit-aware dimension chip that is click-to-edit (typing a length slides
 the next vertex along that edge), and tapping an edge toggles its
 roof-edge flag (red solid = physical fall hazard). Drawing follows the
 Stage 5 model: a Rectangle | Polygon toggle and one Draw ROI button
 (`armRectangleDraw` is press-drag-release, `armPolygonDraw` is
 click-per-corner); both produce a `RoiPolygon`. The robot marker only
 shows its rotate handle while **selected** (click toggles, click elsewhere
 clears); an unselected marker can only translate. View tools ported from
 the Stage 5 `PlotWidget` — `zoomIn/zoomOut`, `fitToRoi`, two-click
 `startMeasure` ruler — sit in a tool stack over the canvas's right edge
 (`SatelliteScreen::buildCanvasTools`). `RoiRect` survives only as the
 four-corner special case that backs the rail's along/across/heading
 spinboxes (field trim only; hidden in the office) — those disable
 themselves the moment the polygon has more than four vertices, because
 the rect mirror is stale then and the per-edge chips are the live
 dimensions.
- `satellite_site_prefetch.{hpp,cpp}` — `SitePrefetcher`, the office
 imagery download engine (age probe → fetch queue → stitch → manifest),
 split from any dialog so it can be reused. `PrefetchRequest::forSite`
 derives every parameter from the ROI (centroid, radius + 60 m margin,
 150 m floor, z19, 3 y).
- `components/satellite_plan_confirm_dialog.{hpp,cpp}` — the office
 **Save Plan** confirmation: ROI thumbnail, per-edge lengths, robot pose,
 tile estimate, **Download & Save** running the prefetch in place, and the
 imagery knobs (radius / zoom / age / Clarity / Wayback) behind a collapsed
 Advanced disclosure. Outcomes: `SavedWithImagery`, `SavedWithoutImagery`
 (explicit operator choice after a failure or with no API key),
 `Cancelled`. The old standalone `DownloadAreaDialog` is gone.
- `satellite_tile_service.{hpp,cpp}` — Esri tiles + geocode + provenance,
 plus the office prefetch: `tilesForArea`, `imageryMeetsAge`,
 `stitchArea` (max-zoom site JPEG), `writeSiteManifest`/`readSiteManifest`,
 `setCacheRoot` (per-job tile tree), `setMaxZoomCap`, Clarity and Wayback
 layer URLs, `listWaybackReleases`.
- `satellite_job_model.{hpp,cpp}` — plans as JSON under
 AppData/satellite_jobs; **schema 4** carries `mode`
 (satellite|measured), the `polygon` + `polygon_roof_edges` arrays,
 `gps` (the map-collection seed fix), `imagery_cache` (what the office
 prefetch actually cached), `alignment` (the `Similarity2D` PCD->imagery
 fit) and `last_executed_at`. Schema <= 3 rect-only plans convert to a
 4-gon on load. `JobStore::assetsDir(id)` is the per-job asset folder
 (`tiles/`, `site.jpg`, `imagery.json`). **Plan lifecycle**:
 `last_executed_at` is stamped ONLY on a successful finalize
 (`SatelliteScreen::markCurrentPlanCompleted`, from both the RPC and
 SSH Complete Mission paths), never at Send; valid => COMPLETED.
 `JobStore::remove(id)` deletes the JSON **and** the assets folder.
 `JobStore::pruneCompleted()` keeps the `kCompletedPlansKept = 5` most
 recently completed plans and runs after every stamp; PLANNED plans are
 never pruned. An operator Save Plan clears the stamp (back to PLANNED).
 Tests: `tests/satellite_job_store_tests.cpp`.
- `similarity_2d.{hpp,cpp}` — Umeyama 2D similarity (scale + rotation +
 optional reflection + translation) mapping PCD/robot_init metres to
 satellite pixels, with RMSE in both units. Reflection is tried both ways
 and the lower-RMSE fit wins.
- `satellite_mission_controller.{hpp,cpp}` — Send: geo->robot_init export
 (marker pose = anchor; verified numerically), SSH launch of
 `robot_autonomous_coverage_director.launch.py roi_vertices:='[...]'`
 (+ `roi_edge_flags` ONLY when edges are marked — older robot builds
 reject undeclared args), laptop_teleop launch for the heartbeat.
- `satellite_ros_link.{hpp,cpp}` — the stage's own rclcpp node:
 cmd_vel/autonomy_enable pubs, coverage/odom/status subs, axis-state
 clients, `pushSessionMetadata` (coordinator SetParameters).
- `components/scan_setup_dialog.{hpp,cpp}` — the mode/plan selector modal.
 The **New Satellite Plan** card is gated on imagery reachability: a
 `QNetworkAccessManager` HEAD against `TileService::connectivityProbeUrl()`
 every 5 s while the modal is open, two consecutive successes to enable,
 one failure to disable (same debounce shape as `UploadDialog`'s cloud
 probe). Satellite planning needs internet, so it happens in the office.
 Plans are split into **SAVED PLANS** (PLANNED, open) and a collapsed
 **COMPLETED (N)** disclosure (newest scan first, `LAST RUN` chip; rows
 still open the plan). Every row has a trash button — the only manual
 delete path (confirm → `JobStore::remove` → row dropped in place,
 dialog stays open, `planDeleted(id)` emitted).

### Two trims

- **Office** (`planning_only_`, Dashboard "Plan Job"): no step header, no
 footer, no Find Robot, no numeric ROI spinboxes, no Collect Map. Plan card
 = name / address / Go / Rectangle|Polygon / Draw ROI / Clear / Place Robot
 / Save Plan. **Save Plan on the satellite canvas IS the imagery step** —
 it requires an ROI (it sizes the download), fits the view, and opens
 `SatellitePlanConfirmDialog`. The saved polygon is a draft the operator is
 expected to adjust on the roof.
- **Field** (scan trim, "Start New Scan"): the five-step header and footer.
 Save Plan on a plan that already carries `imagery_cache` re-writes
 geometry only and carries the cache forward — a roof edit must never
 fetch. A satellite plan **created in the field** (no cache) probes
 `TileService::connectivityProbeUrl()` on Save (`probeImageryReachable`,
 3 s HEAD): online → the same `SatellitePlanConfirmDialog` + prefetch as
 the office; offline → geometry-only save plus a `BdrMessageBox::warning`
 that 3D Alignment needs the site cached once. Connectivity, not trim,
 decides whether a save fetches. **Step-1 Next is the field's save**
 (`onNextClicked` → `cacheSiteThenAdvance`): a satellite plan with no
 `imagery_cache` probes connectivity, then runs
 `saveSatelliteWithImagery(job, site_from_view=true)` — no ROI exists yet,
 so the disc is centred on the map centre (the located address) at
 `PrefetchRequest::kMinRadiusM`; the step only advances on
 `SavedWithImagery`. Offline → warning, stays on step 1 (alignment cannot
 run without `site.jpg`). Plans already cached skip straight through.
 The step-2 chip runs the same detour. `loadJob` treats a cached site as
 aimed (`canvas_aimed_`) and lands the view on the manifest centre when the
 plan has no geometry yet — the save's own `loadJob` round-trip must not
 fail the step-1 gate. **Send persists the sent geometry** (polygon, edge
 flags, anchor) straight into the job (`job_store_.save`, no `loadJob`):
 the field rail has no Save Plan, so this is the only path by which the
 roof-drawn ROI reaches disk.
 **The field satellite rail is per step** (`applyStepVisibility`,
 `frame_rail = !planning_only_ && Satellite`; rail is 288 px =
 `kLeftRailWidth`, frames 238:4289 / 222:1155): **step 1 has no rail**
 (`rail_scroll_` hidden like the picker) — a 420 px floating search pill
 (`buildSearchBar`, `#SatSearchBar`) sits 40 px under the step header
 over the full-width canvas, and the imagery provenance rides the
 bottom-left `layer_chip_` ("Satellite • <date> · <gsd> · <age>"). The
 plan card is never shown in this trim; name comes from the chosen plan
 + metadata modal and `loadJob()` still fills the hidden `job_name_` so
 the step-1 gate holds. Find Robot is not on step 1 (address only, per
 operator). **Type-ahead** is Esri World Geocoder `suggest`
 (`TileService::suggest`, free of geocode credits): 300 ms debounce,
 ≥ 3 chars, `maxSuggestions=6`, `countryCode=USA`, `category=Address,
 Point Address,Street Address,POI`, `location=` map centre bias,
 collections dropped, newer call aborts the older, `suggest_seq_` drops
 stale replies. A pick resolves via `geocode(text, GeocodeBias{magic_key})`
 so the landing is the tapped record; `forStorage=false`, `outSR=4326`,
 `LongLabel` in the log. Rooftop-grade (`PointAddress`/`Subaddress`)
 lands at z19, interpolated `StreetAddress` at z18 with the existing
 "INTERPOLATED" warning. Popup is a `QListWidget` (`#SatSearchPopup`,
 NoFocus so the edit keeps the caret); ↑/↓/Esc handled in
 `eventFilter`, FocusOut closes it. Step 3 shows `roi_card_` (`buildRoiCard`, Figma
 222:1284): title/blurb, Vertices/Status/Area stats box, **Edge
 Dimensions** rows whose value buttons call
 `SatelliteMapWidget::beginEdgeLengthEdit(i)` (row hover →
 `setHighlightedEdge`; the open row + chip both go green via
 `edgeEditChanged`), Clear ROI, boundary note, plus the `canvas_tag_`
 over the map. There is **no Draw button and no along/across/heading
 spinbox on this rail**: entering step 3 with no polygon
 `armPolygonDraw()`s the canvas, Clear ROI re-arms it, and clicking near
 the first vertex closes the polygon (right-click still works). Step-3
 gate = closed polygon (`!isDrawing()`); the Confirm-ROI ack card is
 hidden there. Step 4 = `edge_review_card_` only. The log card only
 shows on step 5. Office and measured keep the single authoring card +
 ack.

### Alignment: robot map -> satellite imagery

GPS alone is a seed, not an answer. The accuracy step is an operator-picked
correspondence fit, ported from the legacy `AutonomyScreen`:

Step 2 (`Step::Alignment`, satellite mode) is the **full-width two-pane
picker** built 1:1 px from Figma file `I9tRcFEniAqnXD0yiMlo6W` frames
`234:1954` (empty), `219:291` (captured, 0 pairs), `235:2246` (4 pairs)
and `235:3146` (aligned) — the frames are 1920×1080 and every Stage 6
constant (`kTopBarHeight = 49`, `kStepHeaderHeight = 55`,
`kFooterBarHeight = 65`, `kCorrBarHeight = 45`) is the frame's px value
unscaled. The rail (`rail_scroll_`) is hidden; `correspond_page_` fills
the canvas column with a 45 px instruction bar (`#SatCorrBar`: amber info
glyph + prompt, right side legend only: `● Satellite (n) ● Point Cloud
(n)` + the `n/N pairs` mono chip, green via the `satisfied` dynamic
property) over `sat_pick_` (58 %) and `pcd_pane_stack_` (42 %:
`pcd_empty_` capture CTA until `pcd_image_` exists, then `pcd_pick_host_`
= `pcd_pick_` with `align_success_card_` floated over it). Pane tags read
`SATELLITE MAP — click to add correspondences` once both images exist.
**Step-2 actions live in the shared footer**: `clear_pairs_button_` beside
Back, `align_button_` (`Align (N pairs)`, zinc) beside Next; both only
while a cloud exists, Align hidden once aligned. Undo has no button (the
frame has none) — it is `QKeySequence::Undo` on the page. Icons are the
Figma exports under `:/assets/satellite/{align_info,align,align_success,
clear_pairs,footer_back,footer_next,scan_frame,refresh}.svg`.
`setSelectedStep(Alignment)` routes through `showCorrespondPage()`, which
tolerates a missing site image (shows why in the pane) and a missing cloud
(empty state). The measured variant keeps the rail's `align_card_`.
`setAlignStatus()` is the one writer for capture progress/errors — it feeds
the rail label, the empty-state title/hint and both capture button labels.
The top-bar title is `Satellite ROI Setup — <plan name>` (`refreshTitle()`,
re-run on every name edit).

**There is no review page.** Align solves AND anchors in one click
(`onAlignClicked` → `applyAlignmentAnchor`): the robot origin is drawn on
the satellite pane, the "Alignment Successful / RMSE" card covers the
point cloud, Next enables. Clear pairs (or Undo) drops the fit with the
picks. `alignment_confirmed_` now means "anchor applied", and is still
required by `stepComplete(Alignment)` because a solve without geo bounds
must not pass.

**Alignment state is per visit.** `resetAlignmentSession()` clears
`pcd_image_`, `pcd_bounds_m_`, `capture_gps_`, `sat_image_`,
`site_manifest_`, picks, fit and `alignment_confirmed_`. It runs on
`loadJob()` for a *different* id (same-id reloads after a mid-alignment
Save keep the session), on `newJob()`, and on the top-bar Back after a
`confirmDialog("Leave to Dashboard?")` — a collected map is never reused
across plans or visits. A cancelled Back keeps everything.

1. **Capture Point Cloud** (`satellite_map_capture.{hpp,cpp}`,
 `MapCaptureRunner`) SSHes `robot_map_collection.launch.py`: arm, 360° spin,
 forward/back GPS baseline, save map + `*_final_pose.yaml`. The remote script
 backgrounds the launch and polls for a manifest newer than the one on disk,
 then SIGINTs the tree itself — `ros2 launch` frequently never returns
 because Fast-LIO, Livox and the ODrive nodes ignore the shutdown request.
 The PCD and pose come back over `scp`, the cloud is re-origined on the
 robot's final pose, and `renderTopDownAlphaDensity` rasterises it.
2. **Pick correspondences** in the two `PanZoomImageWidget`s (stitched
 `site.jpg` left, top-down raster right), strictly alternating
 satellite-then-map so a pair can never half-form on the wrong side.
 Neither pane picks until both images exist.
3. **Align** runs `estimateSimilarity2D(pcd_m, sat_px)`. Minimum pairs is
 **3 with a GPS seed, 5 without** (`SatelliteScreen::minCorrespondences`) —
 the seed independently pins position and usually heading, so the fit only
 has to find scale.
4. **Anchor** (same click, `applyAlignmentAnchor`) turns the fit into a
 surveyed robot anchor: robot_init (0,0) maps to a stitch pixel, and the
 manifest's `stitch_bounds` (normalized Web Mercator) turns that pixel into
 a lat/lon + heading, which becomes the ROI marker and is saved on the job.
 Every exported ROI vertex inherits that accuracy.

The `(image, bounds_m)` pair IS the point cloud's scale bookkeeping — there
is no metres-per-pixel member. Convert with `pcdImageToWorld` /
`worldToPcdImage`. Raster row 0 is **max northing**, so both helpers flip Y.

### Offline imagery (office prefetch)

The field has no internet, so the whole site pyramid is downloaded in the
office and stored **per job** under `JobStore::assetsDir(id)`. The office
Save Plan allocates the job id, then `SitePrefetcher` caches `tiles/`,
stitches a max-zoom `site.jpg`, and writes `imagery.json`; the job is
persisted only after the dialog resolves (`adoptImageryManifest` copies the
manifest into `imagery_cache`). `loadJob` replays that manifest through
`applyImageryManifest()` so the canvas paints off disk and `setMaxZoomCap`
stops the operator zooming into blanks. Cancelling a never-saved plan's
download removes the orphan assets folder.

Zoom selection is **the highest native zoom whose `SRC_DATE` is within N
years** (default 3, newest preferred; editable under Advanced). There is
deliberately no fallback to older-but-sharper imagery: planning a roof
against a decade-old flight is a real failure mode. `imageryMeetsAge`
**fails closed** — unknown provenance is not treated as fresh. Wayback is
an optional Advanced dropdown for pinning a dated mosaic release.

### Rules for agents touching Stage 6

- **The metadata push is the arming gate**: Start Autonomy stays disabled
 until `/data_collection_coordinator/set_parameters` accepts
 building/operator/units (retried 3 s while the stack boots). Same
 hard-block contract as Stage 5 — do not weaken it.
- The BOT pill runs the layered link model: AppShell arms
 `link_monitor_` + `reachability_probe_` on `missionActiveChanged(true)`
 (probe host = the Send SSH target) and every Stage 6 ROS callback stamps
 the monitor. New subscribers must stamp too.
- ROI vertices/edge flags travel in RAW corner order — the robot-side
 manager resolves flags to segments BEFORE `Polygon.buffer(0)` (which may
 reorder rings). Marked edges get `roof_edge_clearance` (0.5 m default)
 planning setback robot-side; physical keep-out still wins.
- `MissionController`'s send helpers are **polygon-only by design**. The
 `RoiRect` overloads were removed: they silently dropped every vertex past
 the fourth. Callers convert with `RoiPolygon::fromRect()` at the boundary
 so the lossy step stays visible. Do not re-add them.
- **`RoiPolygon` is the authored geometry, `RoiRect` is not.** Anything
 that sends, saves, or renders the ROI must read `map_->polygon()` and
 fall back to `RoiPolygon::fromRect()` only when no polygon exists. Do
 not "simplify" the send path back to the rectangle — it silently drops
 every vertex past the fourth.
- `saveJob()` rebuilds the `Job` from the rail, so it must copy `gps`,
 `imagery_cache`, `alignment`, and `align_rmse_m` forward from the stored
 job. Those are produced by the prefetch / map-collection / alignment
 steps, not by the plan card, and re-saving from the rail would otherwise
 wipe them.
- `TileService::imageryInfoAt` coalesces concurrent queries for the same
 cell and notifies **every** waiting callback. Do not go back to dropping
 coalesced callers: the download dialog gates its whole prefetch on that
 callback, so a dropped one hangs the download forever.
- **Complete Mission** runs the same data-first contract as Stage 5, but
 self-contained on this screen (Stage 6 owns its own rclcpp node and launch
 orchestration, so it cannot reuse AppShell's `exploration_ros_node_`
 clients). `onCompleteMission` branches on `isRobotLinkUnreachable()`
 (strict — Disconnected only):
 - reachable → `executeCompleteMissionNormalPath()`: `beginMotorsIdleWait`
 (request IDLE, poll `RosLink::motorsIdle()`, 6 s ceiling) →
 `RosLink::finalizeMission()` (/dc/finalize_mission, 250 ms discovery
 wait then give up) → `teardownMission()`.
 - Disconnected → `OfflineFinalizeDialog`, same three CTAs as Stage 5;
 `FinalizeOverSsh` runs `finalize_mission_local.py` via **direct
 `python3`** and skips both the disarm wait and the RPC.
 Cancel is legitimate: the robot's 10-min idle watchdog is the net.
 **Complete Mission is deliberately NOT link-gated here** (it enables on
 mission-active alone). Stage 5 disables it on link loss and treats its
 strict check as defence-in-depth; Stage 6 instead wants the offline dialog
 to be reachable, because that dialog is the only way to land finalize
 metadata on a dead link. Do not add a link gate to `end_button_`.
- `RosLink::motorsIdle()` requires **fresh** controller_status on both
 axes. Do not relax that to "state == IDLE" alone — a dead CAN bus would
 then read as disarmed while the axes are still in closed loop.
- The motors chip is driven by `updateMotorsChip()` from live
 controller_status, not by the axis-state RPC's ack. The request being
 accepted is not the same as the axes having moved.
- `BDR_DEV_STAGE6_SHOT=<png>` renders the stage headlessly and exits
 (`_DARK`, `_MODE=measured|measured_map|scan|align_empty|correspond|review|roi|plan|
 plan_confirm`, `_STAGE=3|4|5`, `_TOGGLE` modifiers) — the agent-side
 visual verification loop. `plan_confirm` also writes the Save Plan dialog
 to `<png>_dialog.png` / `_dialog_adv.png`. See docs/DEV_BYPASSES.md.
- **Save Plan in the office must keep requiring an ROI and must keep
 running the prefetch.** The imagery is the save's product; a satellite
 plan without cached tiles is not usable on the roof. In the field,
 `saveJob()` must keep the three-way branch: cached → geometry only (never
 fetch on the roof); uncached + online → `saveSatelliteWithImagery`;
 uncached + offline → save + warning. Do not collapse it back to
 "field never fetches" — that strands plans created on site — and do not
 make the field save fetch unconditionally.
- **`review` shot mode = the aligned state** (235:3146): success card over
 the cloud, robot origin on the satellite pane, Next enabled. Do not
 re-add a separate review/confirm page; the frame has none.
- **Do not drop the imagery knobs** (radius / zoom / age / Clarity /
 Wayback). They are defaulted and behind Advanced, not removed — the
 operator asked for that explicitly.
- **Do not stamp `last_executed_at` at Send** or on a failed / cancelled /
 watchdog-deferred finalize. COMPLETED means data is on disk; an aborted
 mission must leave the plan PLANNED so it can be re-run. Only
 `markCurrentPlanCompleted()` writes the stamp, and only from the two
 finalize-success branches. Do not raise the auto-prune above
 `kCompletedPlansKept = 5` without operator signoff, and never let
 `pruneCompleted()` touch a PLANNED plan.
- **Do not re-enable the rotate handle on an unselected marker.** The
 selection gate exists so a drag near the arrow cannot spin the heading;
 `hitTest` returns `RotateMarker` only while `marker_selected_`.
- The `ScanSetupDialog` satellite gate reads `imagery_reachable_` in the
 click handler as well as disabling the card. Keep both: a queued click
 can land after a probe flips the state.
- `PanZoomImageWidget` re-fits on resize until the operator pans or zooms
 (`user_adjusted_`). Do not go back to a one-shot fit: `setImage()` runs
 before layout has sized the pane, so the image ends up tiny in a corner
 of a pane that later grew.
- `SiteManifest::stitch_bounds` is load-bearing for alignment — without the
 Web Mercator extent a fit lands in pixels that mean nothing geographically
 and Confirm cannot place the robot. `loadSiteImage()` refuses manifests
 that lack it rather than silently producing a bad anchor.
- **Robot-side counterpart is pilot_ws branch `cliff-on-autonomy`**
 (worktree `~/BDR_data/pilot_ws`), commit `1c5676e`: GPS-stamped
 `map_collection_node` + `roi_edge_flags`/`roof_edge_clearance` on
 `robot_autonomous_coverage_director.launch.py` + differential erosion in
 `coverage_planner_core.py` (tests: 54 passing). Robot must be rebuilt from
 that branch for the state pill + edge setback to be live.
 Do **not** go back to `feature/ocu-satellite-roi` despite its name: that
 branch only carries the older `robot_autonomous_coverage.launch.py` +
 `coverage_horizon_manager.py` stack, and the OCU launches the *director*
 stack (`satellite_mission_controller.cpp`), which exists only on
 `cliff-on-autonomy`. `feature/ocu-satellite-roi` has its own independent
 `roof_edge_clearance` implementation in `coverage_horizon_manager.py` —
 that one is the dead horizon-manager path, kept only for history.

## Docs worth reading

- `cpp/CLAUDE.md` — authoritative architecture overview and build notes.
- `docs/DEV_BYPASSES.md` — the re-wiring checklist (see above).
- `docs/OTA.md` — OTA state machine, runner UX, field-test recipe.
- `docs/TILT_CALIBRATION_PLAN.md` — tilt calibration design + TODO list.
- `docs/SATELLITE_WORKFLOW.md` — Stage 6 operator narrative: office
  prefetch, ROI drawing, map collection, correspondence alignment, Send.
- `docs/AUTONOMY_CONOPS.md` — north-star concept of operations for
  autonomous scan-while-exploring (edge/obstacle detection, on-robot
  coverage, ROI flow, RTK/frontier roadmap). Design intent, not yet built.
- `pilot_ws/src/pilot_control/docs/FASTLIVO2_CAMERA_NOTES.md` —
  See3CAM_24CUG vendor-doc study + FAST-LIVO2 migration plan (see the
  FAST-LIVO2 section above).
