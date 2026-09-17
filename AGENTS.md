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

- Operational gates are folded under compile-time `kDevMode`
  (`cpp/include/dev_flags.hpp`). A Release build compiles them out.
  `kEnableLaunchDashboardPassthrough` and `kBypassPlannerStageGates`
  are both `= kDevMode`, not hardcoded `true`. Full site table lives
  in `docs/DEV_BYPASSES.md`.
- `cpp/src/startup_screen.cpp` — RGB row is **`left_rgb` only**
  (label "RGB Camera"); the robot preflight checks one See3CAM. Do
  not fold `right_rgb` back into the rollup. Stage 2 Continue is
  ungated only when `kDevMode` is on.
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
  tilt calibration. `TiltCalibrationDialog::setDarkMode` is called
  from the Dashboard; only `#TiltCalibrationContainer` gets
  `WA_StyledBackground` (opaque stack pages cover the 14px corners).
  The Upload Data card stays disabled and
  stylesheet-pulses until the robot→`RDATA_EXT` copy is complete
  (`OffloadStatus` + per-`robot_id` QSettings cache); Complete
  Mission does not wait on that copy. The lower System Information row is fully wired,
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
- **Fleet targeting rides in the release body**, not a separate asset:
  `release.yml` appends `<!-- ota-targets: {…} -->` from
  `cpp/config/ota_targets.json`; `UpdateChecker::parseTargets` /
  `targetsAllow` gate on `setup/robot_id` in BOTH `handleSuccess` and
  `replayPersistedRelease`. No second fetch, works through the ETag
  304 path. `exclude` beats `include`; empty lists = everyone; malformed
  JSON fails **open** (a typo must not freeze the fleet). Do not move
  this to a downloaded asset without carrying the replay path with it.
- **`include` is currently pinned to `["Roofus#0002"]`** to field-verify the
  `swath_overlap:=0.0` launch-arg fix on one laptop. A non-empty `include`
  means every other laptop in the fleet silently receives NO updates — this
  is a temporary state and must go back to `[]` once the fix is confirmed.
  Check this file before concluding "OTA is broken" for some laptop.
- `update/auto_check_enabled=false` is the per-laptop pin:
  `UpdateChecker::start()` returns before scheduling anything. Both
  controls only *offer*; neither installs. They cannot reach an OCU
  still on a pre-gate build — those need `api.github.com` blocked on
  the machine.

### Docs

- `docs/OTA.md` — state-transition diagram, runner UX, wrapper exit
 codes, fleet targeting / pinning, field-test recipe.

## Robot workspace sync (production-wired)

Operator-driven laptop ↔ robot `~/pilot_ws` updater. Every
`goToStage3` asks immediately (`requestRobotSyncCheckNow`, throttled
by `kRobotSyncMinRecheckMs = 60 s`), then checks every 5 min. Failures
on that timer stay silent, but every outcome is logged — one
`reposync` line per check in `update.log`. An actionable mismatch (laptop not on
`kRobotDeployBranch`, origin ahead, robot SHA/branch mismatch, or
helpers / `updateInstead` missing) raises a banner **below** the OTA
banner. Laptop current + robot merely offline is `robot_pending` —
no banner. Later snoozes 1 h (`kRobotSyncSnoozeMs`, its own constant —
the OTA snooze stays 4 h).

Sync targets compiled-in `kRobotDeployBranch` (`cliff-on-autonomy`).
A clean laptop on another branch switches; a dirty or diverged tree
hard-fails. Prepare robot is offered only when Check says helpers
are missing. Modal is `show()`, not `exec()`.

### Key entry points

- `cpp/src/repo_sync_manager.cpp` — laptop git/colcon/ssh orchestrator.
- `cpp/src/components/{robot_sync_banner,robot_sync_dialog}.cpp`
  — banner + frameless modal.
- `cpp/src/app_shell.cpp` — `armRobotSync` from `goToStage3`, banner
  host stacks OTA then robot, `launch_active` + battery < 20% gates.
- `cpp/include/settings_constants.hpp` — `kRobotDeployBranch`,
  `kSettingsRobotSyncSnoozeKey`.
- Robot helpers (already on `cliff-on-autonomy`, outside git once
  installed): `scripts/deploy/{install_deploy_helpers,rebuild_affected,robot_switch_branch}.sh`
  → `~/pilot_deploy/`.

### Rules for agents touching this path

- **Do NOT add** `reset --hard`, `checkout -f`, `clean`, or
  `push --force`. Dirty / diverged = hard fail.
- **Do NOT systemd-restart `rdata-offload`.** Mention it in the
  result detail only.
- **Do NOT add a production branch selector.** Changing the target
  means changing `kRobotDeployBranch` and shipping an OCU.
- **Do NOT build or ship from** `pilot_control/scripts/F2C/cpp/`.
  That tree is a **live parallel OCU**, not a stale fork — alignment
  work lands there first and is ported here (see "Porting from the
  parallel OCU" under Stage 6). This repo is the only copy that ships.
- **Do NOT call `workspacePackageNames()`** from the static
  `packagesForChangedFiles`. The static map and the unmapped /
  `package.xml` A/D/R/C guard must stay lock-step with
  `rebuild_affected.sh`; the instance method filters to present pkgs.
- **A switch is scoped the same as a sync.** Diff from
  `switchFromHead_` (the pre-checkout SHA — Checkout overwrites
  `laptopHeadBefore_`) and `--packages-above`. Full laptop
  `colcon build` only when `needsFullWorkspaceBuild` (build-affecting
  path, no mapped prefix) or a `package.xml` was added/removed/renamed.
  Always `robot_switch_branch.sh --no-build` then `rebuild_affected.sh`,
  which decides scoped vs full from **its** range (robot pre-switch
  SHA → new HEAD). Do not send `--no-build` until Prepare has copied
  a flag-capable helper onto the robot.
- Check-only queries `origin/<deploy>`, not the laptop's current
  branch, and must not overwrite `branch_` with laptop HEAD.
- Arm after login / `goToStage3` when the SSH target is known — not
  at ctor before `setup/robot_id`.
- Banner stays hidden while `isScanLaunchActive()` (no roof nag).
- Paths are `~/pilot_ws` on both sides. No auto-clone.
- `prepareRobot()` skips `LaptopState` and starts at `Stage::Prepare`.

### Docs

- `docs/ROBOT_SYNC.md` — flow, gates, safety contract, file index.

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
  zinc family with `UploadDialog` / `TiltCalibrationDialog`).
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

Operator-driven cloud upload **from the laptop, off the RDATA_EXT thumb
drive**. The robot's offload worker (`rdata_offload.py` +
`rdata_thumbdrive_sync.py`, systemd-owned, branch `cliff-on-autonomy`)
copies every finalized mission onto a USB stick labelled `RDATA_EXT`,
mirroring `/R_DATA/<date>/<building>/{Mission_HHMMSS,Section_*}`
(`thumbsync_manifest.json` written last = copy verified). The operator
carries the stick to the laptop; the OCU walks it and runs `uploader.py`
locally: `POST /presign` per file → single PUT to S3 → `POST /complete`.
The laptop holds **zero** AWS credentials — only the per-robot
`cloud_client_id` / `cloud_device_token` from `robots.json`.

The original **robot → S3 over SSH** path (`UploadSource::RobotSsh`) is
still compiled and selectable through `UploadDialog::setSource()`, but
it is **not wired from the UI**. It exists as a fallback; the robot copy
of the script under `pilot_control/scripts/uploader.py` is legacy and
kept in sync by hand.

### Topology

- **Script** (`cpp/scripts/uploader.py`, canonical; installed by the
  `.deb` at `/usr/share/bdr-coverage-planner/uploader.py`): Python 3 +
  `requests`. Walks a single section/mission folder, calls `/presign`
  per file, PUTs, writes `upload_state.json` after each file (atomic tmp
  + `os.replace`), generates `manifest.json`, then `/complete`. Reads
  `BDR_CLOUD_API_BASE` / `BDR_CLOUD_CLIENT_ID` / `BDR_CLOUD_DEVICE_TOKEN`
  / `BDR_UPLOAD_WORKERS` from the environment. **State lives on the
  stick next to the data**, so a resume works from any laptop.
- **`ThumbDriveWatcher`** (`cpp/include/thumb_drive_watcher.hpp`): 2 s
  poll, pure file I/O — `/dev/disk/by-label/RDATA_EXT` → device →
  `/proc/self/mounts` mount point; device present but unmounted →
  one `udisksctl mount -b` per insertion; fallbacks
  `/media/$USER/RDATA_EXT`, `/run/media/$USER/RDATA_EXT`; an operator
  **Browse…** path wins while it exists. States Absent /
  PresentUnmounted / Mounted; `freeBytes()` via `QStorageInfo`.
- **`UploadStateProbe`** (`cpp/include/upload_runner.hpp`): ThumbDrive
  mode is a `QDirIterator` walk of `<mount>/<date>/<building>/<section>`
  on a `QtConcurrent` worker (generation-counted so a stick swapped
  mid-scan is discarded, and a re-probe supersedes a running walk).
  Same classification as the SSH `find` probe: `manifest.json` → Done,
  `upload_state.json` → Partial, neither → None; file count excludes the
  three sentinels. `scanLocalDataRoot()` is the pure engine (tested).
- **`UploadRunner`**: ThumbDrive mode runs `python3 -u <script>
  <data_path> <robot_id> <run_id>` as a plain `QProcess` with the creds
  in `QProcessEnvironment`; `resolveLocalScriptPath()` tries the
  override → `/usr/share/bdr-coverage-planner/uploader.py` →
  `<appdir>/../scripts/uploader.py` (dev). `FailedToStart` (no python3)
  is routed into the normal finish path so the queue never sticks busy.
  Stdout parsing (`^✓ Uploaded:`, `^Skipping already uploaded:`,
  `^Connection error:`, `^Unexpected error:` …) is unchanged and shared
  with the SSH mode.
- **`UploadDialog`**: frameless modal from the Stage 3 "Upload Data"
  card. Flat list (Building, Operator, Date, Size, Status) — metadata
  from `session_config.json` / `mission_config.json`, no date combo.
  Header names the robot the upload is attributed to **and** the
  drive (`Drive: RDATA_EXT at /media/… · N GB free`). Banner + Upload
  gating = drive mounted AND cloud reachable (2 s `curl` HEAD, 2-miss
  debounce). Pulling the stick mid-upload pauses the runner.
- **`OffloadStatus`** (`cpp/include/offload_status.hpp`): parses
  `/R_DATA/.offload/status.json` (SSH, 5 s while Stage 3 is visible)
  and classifies the laptop stick. Copy-complete is per `Mission_*`
  — the robot writes `thumbsync_manifest.json` only there. Orphan
  `Section_*` folders with no missions are Incomplete. Worker
  top-level `state: "error"` is remapped from per-job state (queued
  jobs look like errors). Cache keys are per `robot_id`
  (`dashboard/offload_incomplete/<id>`, `dashboard/offload_state/<id>`).
- **`AppShellWindow::onUploadDataRequested`**: hard launch-active block,
  `robots.json` lookup by `setup/robot_id`, `setSource(ThumbDrive)`,
  plus `thumbCopyReady()`. `goToStage3` seeds `refreshThumbCopyStatus()`.
  No SSH target is resolved any more.

### Decisions baked into the design

- **One stick per robot.** The stick carries no robot identity (neither
  `mission_config.json` nor `thumbsync_manifest.json` has a robot id);
  everything on it uploads under the robot the operator logged into at
  Setup (`QSettings setup/robot_id`), exactly as the SSH path did. The
  dialog header shows that robot beside the drive so a mismatch is
  visible before Upload. If the fleet ever shares sticks, add a marker
  file robot-side — do not guess from folder names.
- **Per-section `run_id`.** `run_id = "<date>/<building>/<section>"`
  (or `Mission_HHMMSS`). Atomic — one section failure ≠ whole-day
  failure. S3 layout `<client_id>/<robot_id>/<run_id>/<relpath>` mirrors
  the stick 1:1. `thumbsync_manifest.json` is ordinary data to the
  uploader and ships with the mission (useful provenance).
- **Single PUT only.** Backend mints single-PUT presigns; per-file
  ceiling is 5 GB.
- **Sequential targets, parallel files within a target.** Script's
  `UPLOAD_WORKERS=12`; OCU walks the queue one section at a time.
- **Per-file progress.** One `^✓ Uploaded:` line per file is the unit of
  progress.
- **Manifest schema is strict.** `{client_id, robot_id, run_id, files[]}`.
- **State on the stick, not the laptop.** The probe re-derives
  Done/Partial/None from on-disk truth on every dialog open / stick
  insertion.
- **Hard-blocked while a scan is alive.** `onUploadDataRequested`
  reuses the `launch_active` composite the close-event guard uses.
  Operator-locked: Complete Mission first.
- **Hard-blocked until the robot→stick copy is done.** The Upload
  card stays disabled (and blinks) while `OffloadStatus` says the
  copy is incomplete. Complete Mission must not wait on that copy.

### Configuration

- `cpp/config/robots.json`: top-level `cloud_api_base` (fleet-wide) and
  per-robot `cloud_client_id` + `cloud_device_token`. Optional sibling
  `cloud_config.json` overrides the API base; `kDefaultCloudApiBase` is
  the compiled-in fallback.
- `.deb` `Depends:` now includes `python3, python3-requests, udisks2`;
  `create_deb.sh` fails if `cpp/scripts/uploader.py` is missing.
- Drive label defaults to `RDATA_EXT` (`ThumbDriveWatcher::kDefaultLabel`)
  and must match the director's `thumbdrive_copy_label` parameter.

### Pause / cancel

- **Pause** touches `<data_path>/pause.flag` on the stick; the script
  exits at the next file boundary. Resume = press Upload again. **The
  runner deletes a stale `pause.flag` before every launch** (locally, or
  via `rm -f` inside the SSH command) — the script never removes it, and
  before this fix a resume re-paused at its first file.
- **Cancel** SIGTERMs the process. State on disk is preserved.
- **Closing the dialog while busy** routes through `requestPause()`.

### Tests

`tests/upload_thumb_drive_tests.cpp` — local walk classification
(including `classifyStickSync` / `parseOffloadStatusJson`), script
resolver, `/proc/mounts` escape decoding, and an end-to-end runner
launch against a stub script (env, argv, stdout contract, pause-flag
clearing, `FailedToStart` handling).

### Rules for agents touching this path

- **Do NOT remove or "simplify"** any of the following — they are
  load-bearing:
  - `uploader.py`'s atomic state writes (`os.replace`).
  - `python3 -u` (+ `PYTHONUNBUFFERED=1`) in the launch — without it
    the stdout parser stalls 4 KB at a time and operators see no
    progress.
  - The depth-3 walk (`<date>/<building>/<section>`) in both probes.
  - `ThumbDriveWatcher`'s by-label → `/proc/self/mounts` resolution and
    the one-attempt-per-device `udisksctl` mount.
  - The pause-flag clear before launch.
  - `UploadSource::RobotSsh` and `setRemote()`/`setRemoteScriptPath()`
    — inactive, but the fallback the operator asked to keep.
  - `RobotRegistry::cloudApiBase()` and its three-tier resolution.
- **Do NOT downgrade the launch-active gate** in `onUploadDataRequested`.
- **Do NOT wait on the thumb-drive copy inside Complete Mission.**
  That gate is the Dashboard Upload card (`thumbCopyReady()`).
- **Do NOT require `thumbsync_manifest.json` on `Section_*`.** The
  robot writes it only on `Mission_*` after the mission and its
  sections land. Requiring it on every section makes a finished
  stick look incomplete after upload.
- **Do NOT nest `QGraphicsOpacityEffect` on the Upload card** — Qt
  cannot nest it under the Quick Actions drop shadow. Blink is a
  450 ms stylesheet pulse on title/icon/subtitle. Calibration still
  uses an opacity effect because it *replaces* its own shadow.
- **Do NOT add IAM credentials to the OCU.**
- **Do NOT make the dialog write anything to the stick except through
  `uploader.py`** (`upload_state.json`, `manifest.json`, `pause.flag`).
  The robot's `rdata_thumbdrive_sync.py` treats a destination with
  `thumbsync_manifest.json` as done and never re-copies, so anything
  else the laptop leaves there is permanent.
- The SSH fallback **must NOT** go through `ros2 run` — invoke
  `python3 /home/<ssh_user>/pilot_ws/install/pilot_control/lib/pilot_control/uploader.py`
  directly.

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
 AppData/satellite_jobs; **schema 6** adds the `scan` block
 (`coverage_width_m`, `scan_speed_mps`; absent => director defaults);
 5 added `polygon_edge_locks_m`; **schema 4** carries `mode`
 (satellite|measured), the `polygon` + `polygon_roof_edges` arrays,
 `gps` (the map-collection seed fix), `imagery_cache` (what the office
 prefetch actually cached), `alignment` (the `Similarity2D` PCD->imagery
 fit) and `last_executed_at`. Schema <= 3 rect-only plans convert to a
 4-gon on load. `JobStore::assetsDir(id)` is the per-job asset folder
 (`tiles/`, `site.jpg`, `imagery.json`). **Plan lifecycle**:
 `last_executed_at` is stamped ONLY on a successful finalize
 (`SatelliteScreen::markCurrentPlanCompleted`, from the RPC and
 SSH Complete Mission paths), never at Send and never after
 abort-and-save (partial sweep stays PLANNED so it can be re-run);
 valid => COMPLETED.
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
 reject undeclared args) + `ScanParams::launchArgs()`
 (`coverage_width swath_overlap:=0.0 desired_linear_speed`, always sent;
 robot must be on `cliff-on-autonomy`), laptop_teleop launch for the
 heartbeat. **Every value in `launchArgs()` must carry a decimal point.**
 The director's launch file feeds them into the node's `parameters` dict as
 bare `LaunchConfiguration`s, so launch_ros type-infers with YAML: `:=0` is
 an int, the node declares `swath_overlap` as a strict double, and an int
 override against a DOUBLE descriptor raises
 `InvalidParameterTypeException` inside the director's `__init__` — before
 the `/coverage/status` publisher exists. The director dies, every other
 node in the launch keeps running and logging, and Start Scan never
 unlocks. Cost a field session on 2026-09-15; guarded by
 `ScanParams.LaunchArgsAlwaysCarryADecimalPoint`.
- **Scan Parameters card** (`buildScanParamsCard`, both trims, visible once
 the polygon is closed): `TrackSlider` (the legacy Stage 5 slider lifted to
 `components/track_slider.*`) + a typed `QLineEdit` per knob. Slider is SI
 and the model; the edit is a units-layer view. Ranges / step / defaults
 live ONLY in `ScanParams` (`satellite_job_model.hpp`): width 0.30-2.00 m
 step 0.05, speed 0.4-0.6 m/s step 0.1, defaults 0.50 / 0.40 = director
 defaults. `snapWidth/snapSpeed` clamp + snap everything (typed input,
 hand-edited JSON, launch args). Do not add a second copy of the ranges.
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
 roof-drawn ROI reaches disk. **Per-job tile redirects are undone**
 by `TileService::resetToSharedCache()` (shared root, cap 0, World layer,
 no Wayback) from `newJob()` and the uncached branch of `loadJob()` —
 without it a new plan keeps writing tiles into the previous plan's
 assets folder and inherits its zoom ceiling.
 **The field rail is per step** (`applyStepVisibility`,
 `frame_rail = !planning_only_`; rail is 288 px =
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
 Next pops a **Confirm ROI modal** (closed polygon is readiness,
 `confirmed_vertices_` is completion). Step-4 Next pops **one** modal: in
 the office it is **Edges Reviewed**; in the field it is **Launch coverage
 stack** (edge summary + marker-at-pose checklist in the same dialog) and
 confirming **launches** the director stack — there is no Send button and
 no second confirm. Ack checkboxes are gone. Measured
 field trim hides Satellite Map (Robot Map is step 1); chips renumber
 1–4. **Step 5 is the shipped Stage 5 Scan page reproduced 1:1**
 (`buildScanLeftRail` / `buildScanRightRail` / `buildScanControlBar` /
 `buildScanFooter`, geometry lifted from `PlannerScreen`'s scan stage on
 `origin/main`): 384 px left rail (Overall Progress: coverage + quality
 bars, Scan Time; Telemetry: speed, X/Y, heading), the map framed as a
 card (`#SatCanvasStack[scan="true"]`) with the status pill above the tool
 stack and the 81 px control bar under it (Start Scan/Pause/Resume ·
 `mm:ss • swept/total intervals` · Cancel Scan · Emergency Stop /
 Clear E-Stop), 380 px
 right rail (Manual Override with the FPV — click = teleop, map click
 hands back and does NOT resume autonomy; Scan Statistics: distance, avg
 quality, ETA, data copy;
 Motors: Arm / Disarm — Arm is CLOSED_LOOP only, see the E-Stop latch
 rule below), and the 69 px footer (Edge Review back · Step 5 of 5 ·
 Complete Mission primary). The plan rail, log card and frame footer are
 hidden on this step. Scan Quality is
 `computeReprojectionQualityPercent` (odom trail vs `/coverage/
 planned_swaths`, 1 m association, off-thread every 2 s) — the Stage 5
 metric verbatim. Nothing above the FPV surface changes height at
 runtime: the native video widget does not repaint the region it vacates
 (the "double Manual Override header" ghost). `run` shot mode renders it.

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
3. **Align** runs `fitSimilarityRobust` (see the robust paragraph below;
 `estimateSimilarity2D` survives only as the unweighted seed that
 bootstraps a px/m when the manifest has no `res_m`). Minimum pairs is
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

The fit is **precision-weighted and robust**, not plain least squares.
Each pick records the zoom it was made at (`Correspondence::sigma_sat_px`
/ `sigma_pcd_m`, captured in `onSatellitePicked` / `onPcdPicked` from
`PanZoomImageWidget::scale()` — the operator zooms between the two halves
of a pair, so it cannot be recovered later). `pairSigmaM` combines the two
sides in PCD metres over `kPickFloorM`, and `onAlignClicked` runs
`fitSimilarityRobust`, so a sloppy pick is downweighted instead of
dragging the whole solve. `align_rmse_m` therefore holds **weighted**
RMSE — the field name predates the change and is kept for schema
compatibility. Sigma needs a px/m to make the satellite term metric:
the manifest's `res_m` is the honest source, and only when it is missing
is a scale bootstrapped from an unweighted seed fit.

The robust pass also reports `outliers`, `weakly_checked`, `studentized`
and `min_detectable_m`. `worstOutlierIndex` reduces that to the one pair
worth naming, and it surfaces **three ways, all advisory** — nothing is
blocked, removed or reweighted by the operator's attention: an amber
`setFlaggedMarker` ring on that pair in BOTH panes, a
`· Pair N is Xσ off its own expected precision — worth re-picking` clause
appended to the `#SatCorrBar` instruction, and the numbers in the mission
log. `leaveOneOutOutlier` runs only when a pair was flagged, on the
weights the IRLS settled on, and only to log what dropping it would be
worth. Do not read the absence of a warning as an all-clear below 4
pairs: both the studentised test and `leaveOneOutOutlier` abstain there,
which is why the 3-pair GPS-seeded minimum is annotated "4+ to check for
a bad pick" in the instruction bar.

The ring carries a matte halo under the amber stroke. Two of the eight
marker colours are themselves amber and the ring lands on the imagery, so
without it a tan roof or pair 3 swallows the mark.

### Live re-projection (step 2 satellite pane only)

From the **second** pick onward the satellite pane shows the stitch
re-projected into the robot frame, so the operator places each new pair
against imagery that already agrees with the cloud, and the agreement
tightens with every increment instead of arriving all at once at Align.
`updateSatelliteAlignmentView()` owns it and runs first inside
`updateCorrespondenceUi()`, because it writes the pane's image,
`sat_view_xform_` and every residual diagnostic the markers and the
instruction bar then read.

- **Correspondences are stored in ORIGINAL satellite pixels**, never
 canvas pixels, so a pair keeps its meaning as the projection moves under
 it. `sat_view_xform_` maps them out for drawing and `onSatellitePicked`
 runs a click backwards through its inverse. That handler must also
 bounds-check the result against `sat_image_`: rotating the imagery
 leaves padding in the canvas corners, the widget only checks the canvas,
 and a click out there would be stored as an off-image pixel and silently
 poison the fit.
- `sigma_sat_px` is in original pixels but `PanZoomImageWidget::scale()`
 is screen px per CANVAS px, so the pick sigma divides by
 `sqrt(|determinant|)` of `sat_view_xform_` as well. They differ only
 when the canvas hit `kAlignedSatMaxDim`, but conflating them
 mis-weights exactly the picks that are least trustworthy.
- **`plausibleForProjection` is a guard, not a nicety.** Two pairs fit a
 similarity exactly, so an early mismatched pick yields a *confident*
 nonsense transform; swinging the imagery by it would wreck the view the
 next pick is made in. A fit whose scale is more than
 `kScaleRejectFrac` from the manifest's own `res_m` holds the last good
 projection instead. With no `res_m` there is nothing to check against
 and the fit is allowed through.
- `applySatelliteView` holds the operator's viewpoint across the warp by
 carrying the anchor in original pixels — the one frame both canvases
 agree on — and correcting the zoom by the change in linear scale.
- The warp is **inline on the GUI thread**, guarded by `sameTransform` so
 a refresh that changed nothing does not repaint. Measured at
 **113–144 ms** for the worst realistic case (a z19 500 m-radius stitch,
 ~4580 px, into the 4096² `kAlignedSatMaxDim` canvas = 64 MB). That is 3–8
 warps per alignment session, so it is a hitch and not a freeze; if it
 ever needs to come down, lower the canvas cap before adding threading.
- The robot glyph on the satellite pane is driven by `preview_fit_` and
 has exactly **one** writer, `updateSatelliteAlignmentView`.
 `refreshCorrespondenceMarkers` must not also set it — last writer wins
 and the two disagree by the view transform.
- **This is step 2 only.** Step 3's ROI canvas is `SatelliteMapWidget`
 painting north-up Esri tiles, and it stays that way: the exported
 vertices are already robot-frame-correct because `applyAlignmentAnchor`
 turns the fit into a geo anchor, so rotating that canvas would change
 nothing about where the ROI lands. The legacy needed a calibrated ROI
 canvas (`sat_grid_->setCalibratedImage(sat_image_, pcd_to_sat_)`) only
 because it had no anchor to export through.

### Porting from the parallel OCU

`pilot_control/scripts/F2C/cpp/` (pilot_ws branch `autonomy`) is a **live
parallel OCU** where alignment work is developed. `similarity_2d.*` and
`alignment_geometry.*` are ports of that tree at commit `a287113`, and
their file headers name it. Keep the **code** identical to upstream —
the provenance note is the only intended difference — so the next port
stays a three-way diff instead of archaeology. Fix bugs in both copies
or in neither. The tests are the deliberate exception: upstream uses a
hand-rolled `int main()` / `printf` harness and this repo is uniformly
GoogleTest, so `similarity_2d_tests`, `robust_fit_tests`,
`canvas_extent_tests`, `aligned_canvas_tests` and `pick_loop_tests` are
converted rather than copied.

`alignedCanvasFor` is consumed by `renderAlignedSatellite` (see "Live
re-projection" above). `canvas_extent_tests` transcribes an extent rule
this repo never shipped — it exists only so the upstream regression has
something to fail against.

The **outlier and re-projection surfaces are deliberately NOT ports.**
Upstream shows both through a `QListWidget` of pairs sorted by studentised
residual, with `← OUTLIER` / `← can't be checked (isolated)` suffixes and a
Delete-selected button; the Figma correspondence frames here have no list,
so the same information became the pane ring plus one instruction-bar
clause. The *policy* is upstream's (`worstOutlierIndex`, the leave-one-out
call, `plausibleForProjection`, `kScaleRejectFrac`,
`kOutlierImprovementFrac`, `kAlignedSatMaxDim`) and should stay identical;
only the presentation diverges. Upstream's `preview_rmse_m_` has no
counterpart because there is no status label to put it in.

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

- **Link budget is the constraint, not the OCU.** The Microhard link
 measured ~0.5 Mbit/s of TCP headroom with the stack up (iperf3,
 2026-09-13). Zenoh maps DDS RELIABLE to *blocking* congestion control:
 one heavy reliable topic stalls the transport for 5 s and it is closed,
 dropping the heartbeat → MPC `execution_ready=false` → director
 `mpc` not ready → no motion. `RosLink` therefore subscribes to what the
 legacy autonomy screen did and nothing heavier: odom (SensorData),
 `/coverage/planned_swaths` (SensorData keep_last 1 — the coverage
 picture), `/coverage/status`, `controller_status`, `scan_segment_status`.
 **Do not subscribe to `/coverage/global_occupancy` or
 `/coverage/planned_path`** (RELIABLE + TRANSIENT_LOCAL at 5 Hz; the
 grid alone is ~0.9 Mbit/s) until the director offers a BEST_EFFORT
 ≤ 1 Hz copy. `/mpc_autonomy_enable` is RELIABLE + TRANSIENT_LOCAL,
 published **once per transition** (`RosLink::publishAutonomyEnable`
 dedupes) — no periodic latch. Teleop twists go out only while a key is
 down plus one zero. Director services (`conclude`, `abort`) and the
 end-of-mission disarm try the bridge first and fall back to
 `ros2 service call` over SSH (`MissionController::remoteServiceCall` /
 `remoteDisarm`) — zenoh queries are the first thing to time out on a
 congested radio, SSH is not.
- **Start Scan is the arming gate**: `/coverage/status` fresh (< 3 s) and
 not `ERROR` enables the button; pressing it pushes the session metadata
 **on demand** (bridge, retried 3 s; SSH `set_parameters` fallback after
 5 misses — `start_scan_pending_` resumes `beginStartScan` when it lands)
 and only then arms. Nothing is pushed during the boot window — the
 legacy screen pushed at Start Scan too, and a query every 3 s over the
 radio while the stack boots is load with no purpose. **Never gate on
 `initialized`**: the director only initializes once `ready.mpc` is true,
 and the MPC only raises `/mpc/execution_ready` after autonomy is enabled
 — which Start Scan sends. Gating on it deadlocked in the field
 (2026-09-13). Start Scan requests CLOSED_LOOP and waits up to 6 s for
 both axes; on timeout it enables anyway (the robot self-arms at launch
 and an unarmed MPC cannot move; the MOTORS chip shows the truth). On
 mission end the OCU latches `/mpc_autonomy_enable=false` so a relaunched
 director can never read a stale `true`. A laptop-launch exit is handled
 like a robot-launch exit (heartbeat gone = dead run) — same
 `launchDied` signal, the modal names the side.
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
- `persistJob(job, reload)` — office / step-1 save reloads (`true`).
 Field mid-flow (Confirm ROI, map capture) must pass `false`:
 `loadJob` clears `confirmed_vertices_` and would hide Next.
- `TileService::imageryInfoAt` coalesces concurrent queries for the same
 cell and notifies **every** waiting callback. Do not go back to dropping
 coalesced callers: the download dialog gates its whole prefetch on that
 callback, so a dropped one hangs the download forever.
- **Complete Mission** is reachable on mission-active alone (offline
 dialog on true Disconnected). Reachable path: `MissionFinalizeDialog`
 (shown, not `exec()` — async callbacks must keep flowing) →
 `concludeCoverage()` → wait until `/coverage/status` shows save done
 (`complete` or `copy` in pending/copying/done/skipped) → motors IDLE
 wait → teardown. Abort-and-save is the live CTA when conclude
 refuses (offered at 10 s, consumed on click, leaves the plan
 PLANNED). Skip-copy (`/coverage/skip_copy`) stays connected on the
 dialog but Stage 6 never surfaces it — the OCU does not wait on the
 copy (those states already count as save-done) and the Dashboard
 Upload card owns that state. SSH-offline fallback is unchanged
 (`finalize_mission_local.py` via direct `python3`). **The only hard
 death signal is `MissionController::launchDied(side, rc)`** (the SSH
 session carrying the robot launch, or the laptop launch, exiting
 unasked) — `handleLaunchDeath` auto-teardowns, keeps the plan PLANNED,
 and returns to Edge Review. The status watchdog (`onDirectorWatchTick`,
 1 Hz) is advisory: it drives a `LAUNCHING · robot link / director Xs`
 pill, starts its clock at the first robot topic (`noteRobotTopic`), and
 after 120 s (180 s with no topic at all) asks Keep waiting / cancel. A
 mute robot launch (SSH up, no output from the launch itself — the
 first line after `BDR_LAUNCH_BEGIN` — for 25 s) only changes the
 pill and logs once. Lines before the marker are profile noise; a
 marker with nothing after it is the silent-`ros2 launch` case seen
 in the field. That clock is
 `robotSpawnMs()` from `spawnLaunches`, not `launch_wall_ms_`, because
 the sweeps in front of spawn can eat ~31 s. It must never tear down by
 itself — the full stack + Zenoh session takes 30-60 s and a 20 s
 auto-teardown killed a healthy launch in the field.
 Do not add a link gate to `end_button_`.
- **Step 5's inline styles carry a palette role, and `restyleScanPage()` is
 what makes the theme toggle reach them.** The page reproduces the Stage 5
 frames 1:1, so its widgets are styled per-element inside
 `buildScanLeftRail` / `buildScanRightRail` / `buildScanControlBar` /
 `buildScanFooter` rather than by the screen's object-name QSS. Those
 builders run once and the screen is reused across missions, so a sheet set
 there would hold the boot palette forever. Every styled widget records a
 role in the `satScanRole` / `satScanRoleArg` dynamic properties and
 `restyleScanPage()` (called from `setDarkMode`) re-resolves them. A new
 styled widget on step 5 needs a role, not a second patch site. Control-bar
 fills stay in the local `scanActionPalette` table, NOT the shared
 `danger_fill` / `warning_fill` tokens — the dialogs use a deeper amber and
 red, and step 5 has to keep matching Figma in dark mode. Their labels are
 tinted by `applyScanActionFg`, driven by a `ScanActionTint` event filter,
 because Qt cannot restyle a child `QLabel` from the button's `:disabled`
 pseudo-state and a white label on the disabled grey is unreadable in light.
- **A disabled Start Scan must always say why, and the step-5 corner pill
 is the only place it can.** The rail is hidden on step 5, and a disabled
 button's tooltip is not a surface a field operator can reach. `scanBlockReason()`
 is the single source for both: it returns a short pill label plus one
 plain sentence, in operator language with no ROS vocabulary.
 `updateStatePill()` owns the pill whenever a `/coverage/status` message is
 fresh; `refreshScanRunUi()` owns it the rest of the time — that boot / link
 window is exactly where the operator used to read a hardcoded **"Ready"**
 next to a dead button. Do not re-add a default pill string that claims
 readiness, and do not collapse the two owners into one.
 The **E-Stop latch is the one exception** to that ownership split:
 `updateStatePill()` returns early while latched
 (`estop_latch_policy::holdsStatusPill`) and `refreshScanRunUi()` paints
 BOTH pills, because `/coverage/status` is 1 Hz and would otherwise
 relabel the operator's E-STOP confirmation to `SWEEP` within a second.
- **E-Stop is latched, and the latch IS the run state**
 (`estop_latch_policy`, `ScanRunState::EmergencyStopped`). This is a
 safety fix, not a UX preference — do not unpick any part of it:
 - `onEstop()` sets the state through `onEmergencyStop()`. Because the
 latch is a *state*, every gate that tests `== Running` is false while
 it holds, which is what makes the fix total rather than a whack-a-mole
 of individual call sites. Do not demote it back to a `bool` beside
 `Running`.
 - **Nothing auto-resumes autonomy, ever.** The old
 `resume_after_override_` member latched "was running" when the operator
 took manual control and called `beginStartScan()` when they released
 it — so a **map click** re-armed CLOSED_LOOP and republished
 `autonomy_enable=true`. Post-E-Stop that fired three times next to a
 roof edge on 2026-09-16 and the robot had to be physically caught.
 Releasing manual override now ends teleop and nothing else; the state
 is `Paused`, so the primary button reads Resume and the operator arms
 deliberately. `releaseResumesAutonomy()` is a constant `false` on
 purpose — it documents the contract at the call site and a
 `static_assert` pins it.
 - `beginStartScan()` refuses outright on `!armingAllowed()`. It is the
 single choke point for arming-with-autonomy, so the guard belongs there
 even though the button is already disabled.
 - **Clearing costs its own press and does not arm.** The E-Stop button
 relabels to `Clear E-Stop` (matching the shipped Stage 5 control bar,
 `PlannerScreen::scan_estop_latched_`); clearing lands on `Paused`, and
 Resume is a second deliberate press. Do not move clearing onto the
 primary button — one press must never both clear and arm.
 - **Motors: Arm exists so E-Stop is not a dead end.** Arm was previously
 folded into Start Scan, so the only way to re-power the wheels also
 re-enabled autonomy — that dead end is what pushed the operator into
 the defect above. `arm_button_` requests `kAxisClosedLoop` and touches
 nothing else, deliberately does **not** consult `armingAllowed()`, and
 stays enabled while latched. Teleoping away from an edge must always be
 available.
 - **Space is E-Stop, and it is stop-only.** `onEstopShortcut()` calls
 `onEstop()` directly, never `onEstopButtonClicked()`: if the key toggled,
 an operator mashing a panic key an even number of times would release the
 stop they were applying. Clearing stays mouse-only.
 (`estop_latch_policy_tests` makes that hazard executable.) Three details
 are load-bearing:
 - `Qt::ApplicationShortcut`, not a `keyPressEvent`. Qt matches shortcuts
 before delivering the key to the focus widget, which is what stops a
 clicked control-bar button from eating Space as a click on itself, and
 it keeps the key alive while one of the screen's six modeless dialogs
 holds focus. The legacy `PlannerScreen::keyPressEvent` (Space →
 `onScanEmergencyStopClicked`, step-gated) is the weaker shape and
 carries both defects — do not copy it back.
 - The shortcut is **enabled only while `missionActive()`**, which is NOT
 redundant with the handler's own guard: an application shortcut
 swallows the key whether or not its slot acts, so an always-armed
 shortcut would break Space on every checkbox and dialog button in the
 app. `scanActionButton()` sets `Qt::NoFocus` for the between-missions
 window the shortcut deliberately leaves uncovered.
 - The handler **must** no-op when a `QLineEdit` / `QPlainTextEdit` has
 focus. Without it, typing a space into the plan name or the address
 search fires an E-Stop.
 - **No `/coverage/abort` in the E-Stop path.** `autonomy_enable=false` is
 already the full robot-side stop: `CoverageExecutionManager.set_autonomy`
 publishes a stop, drops the installed route, and `tick()` returns at the
 top of every later cycle. `abort` ends the run and closes the section as
 partial, which would make a recoverable stop unrecoverable.
- **The stop modal dwells three `/coverage/status` samples
 (`stop_prompt_policy::kStopDwellSamples`).** The executor latches a
 stop reason for as little as one 20 Hz tick (`degenerate_path` at the
 end of a sweep that has not settled yet); the status topic is 1 Hz, so
 a healthy robot can look stopped on a single sample. The corner pill
 still updates on the first sample — that is the operator's view of a
 blip. Do not drop the dwell, do not exempt ERROR / STALE_INPUT, and do
 not move the pill behind the same counter. A stop that persists is
 operator-actionable; a one-tick latch is not.
 The counter counts **consecutive stopped samples, not repeats of one
 reason** — a robot failing to replan cycles `blocked_*` →
 `route_invalid` → `replan_*` across samples, so keying the dwell to a
 matching `state|stop|stale` let a genuinely stuck robot reset the count
 forever and never prompt. `stop_prompt_key_` is for the
 already-explained dedupe only. `stop_dwell_samples_` resets in the
 `missionActiveChanged(false)` handler with the rest of the run state;
 leaving it set let the next mission's first stop skip the dwell.
- **Operator dialogs carry no raw launch output.** The robot launch is
 10-20 Hz of cliff / tfmini INFO lines, so a tail tells the operator
 nothing and the traceback worth reading has already scrolled out of it.
 The launch-wait prompt and `handleLaunchDeath` show plain sentences and
 name the mission log instead. Every line the screen displays goes through
 `SatelliteScreen::appendLog` → `MissionController::appendMissionLog`, which
 is the ONE choke point writing `AppData/mission_logs/mission_<stamp>.log`
 (flushed per line, newest `kMissionLogsKept = 10` kept). Do not also write
 from `hookProcessLogging` — process output would land twice — and do not
 re-log `recentRobotOutput()` at failure time for the same reason.
- **Launch is Edge Review Next**, not a Send button. Back from step 5
 while the stack is up (and Start Scan has never run) confirms teardown;
 once autonomy has run, Back is disabled — Cancel / Complete are the exits.
- **Launch/teardown primitives are shared and deliberately dumb.**
 `cpp/include/launch_env.hpp` holds the one env preamble and the one
 laptop sweep used by both the legacy Stage 4/5 path and Stage 6.
 `MissionController::kRobotSweep` is the shared robot sweep, run before
 a launch (a lingering step-2 map-collection tree must not coexist with
 the director) and at teardown: **`pkill -INT` the launch** (Ctrl-C —
 launch shuts its ~25 children down in order), wait for it to exit, kill
 by name what ignores shutdown (Fast-LIO, Livox, ODrive, director, MPC,
 UDC, bag record, zenohd), wait for UDC to release the Seek SDK, `-9`
 only the survivors. The pre-launch path appends a `_ros2_daemon` kill
 after that body — a wedged CLI daemon can block the next `ros2 launch`
 — and must NOT run at teardown, where an in-flight `ros2 service call`
 (conclude / disarm) still needs it. A non-zero sweep rc is ssh
 unreachable (255) or the 20 s hang deadline (-1); the script itself
 cannot fail (`set +e` / `|| true`). The launch path parks and asks
 before spawning; teardown only logs. **Never SIGKILL `ros2 launch` first and never
 `terminate()` the local `ssh -tt` before the remote launch has exited**
 — both orphan every node (the stray `ros2 bag record` found on
 2026-09-13 was exactly that), and the next launch then fights orphans
 for the LiDAR port, the CAN bus and `/coverage/status`. Teardown order
 is robot sweep → reap `robot_proc_` → SIGTERM laptop launch → laptop
 sweep. All run-state bookkeeping is reset in ONE place, the
 `missionActiveChanged(false)` handler — do not add resets at launch.
- **Launch and teardown are asynchronous, and must stay that way. The UI
 is never allowed to freeze.** Both are timer-driven chains over
 `cpp/include/async_process.hpp` (`async_proc::run` spawns with a
 deadline and SIGTERM → SIGKILL escalation; `async_proc::reap` waits on
 an already-running process; both call back exactly once, always). No
 `waitForFinished` / `waitForStarted` / `processEvents` may come back:
 those sweeps are 20 s + 11 s + 7 s of remote work and the operator read
 the motionless window as a crashed app.
 - `startMission` returns false only for input errors. It marks the
 mission active **before** spawning, so the operator lands on step 5
 and Cancel works during the sweeps, and narrates itself through
 `launchPhase` into `scanBlockReason()`'s corner pill. A spawn failure
 arrives as `launchDied`, same as any other death.
 - `teardownMission` reports `teardownPhase` → `missionStateChanged
 (false)` → `teardownFinished`, in that order. Callers that used to
 navigate on the next line go through `SatelliteScreen::teardownThen`.
 The `MissionFinalizeDialog` it raises is the operator's proof that
 something is happening, and it also blocks a second Cancel / Back.
 `forceStopTeardown` (offered after `kForceStopOfferMs`) is the escape
 hatch for a robot that stopped answering: it SIGKILLs locally only,
 and the next launch's sweep clears whatever it left on the robot.
 - `mission_seq_` is what stops a Cancel during the sweeps from being
 followed by `spawnLaunches()`. Keep it on any new chain step.
 - `SatelliteScreen::shutdownMission` (OCU quitting) is the ONE place
 that waits, with a bounded local `QEventLoop` — `closeEvent` cannot
 return before the robot's UDC releases the Seek SDK. Do not copy that
 pattern anywhere else.
 - `MapCaptureRunner` rasterises off-thread (`QFutureWatcher<MapCapture>`,
 failures ride back in `capture.error`). A roof cloud is millions of
 points; PCL on the GUI thread froze the alignment step for seconds.
- **Re-stamp `confirmed_vertices_` at launch**
 (`launchMissionFromEdgeReview`). Any nudge after Confirm ROI — a vertex
 drag, a marker move, which re-anchors every vertex — otherwise leaves
 `roiMatchesConfirmed()` false, step 5 unreachable, and `setSelectedStep`
 silently refusing: the stack launches and the operator sits on Edge
 Review pressing a button that looks dead. The fallback log line naming
 the blocking step is the tripwire if a future gate regresses.
- `RosLink::motorsIdle()` requires **fresh** controller_status on both
 axes. Do not relax that to "state == IDLE" alone — a dead CAN bus would
 then read as disarmed while the axes are still in closed loop.
- The motors chip is driven by `updateMotorsChip()` from live
 controller_status, not by the axis-state RPC's ack. The request being
 accepted is not the same as the axes having moved.
- `BDR_DEV_STAGE6_SHOT=<png>` renders the stage headlessly and exits
 (`_DARK`, `_MODE=measured|measured_map|scan|align_empty|correspond|review|roi|run|plan|
 plan_confirm`, `_STAGE=1|2|3|4|5`, `_TOGGLE` modifiers) — the agent-side
 visual verification loop. `plan_confirm` also writes the Save Plan dialog
 to `<png>_dialog.png` / `_dialog_adv.png`. See docs/DEV_BYPASSES.md.
- **The canvases paint, they do not style, so no stylesheet reaches them.**
 `SatelliteMapWidget::setDarkMode` and `PanZoomImageWidget::setDarkMode`
 are plumbed from `SatelliteScreen::setDarkMode` and a new painted color
 needs to go through `satpal` (`cpp/include/satellite_palette.hpp`) or a
 `dark_mode_` branch. `satpal` splits on one rule: **brand hues are fixed,
 neutrals follow the theme.** `accent`/`danger`/`warning`/`info` mark up
 the operator's geometry against satellite imagery, which is the same
 photograph in either theme; `text`/`cardBg`/`border`/`chipBg` are chip
 plates and label colors and are views over `uiThemeTokens`, so the zinc
 ramp has exactly one definition. Chrome follows the theme **even over
 imagery** — a near-black chip is the obvious choice against an aerial
 photo, but the operator works in direct sun where the screen's black is
 grey with glare, and light-on-dark is the harder read there.
- **The point-cloud raster has to be re-inked for a light canvas.**
 `renderTopDownAlphaDensity` draws every point at `kPointGray = 210` and
 puts all the density information in the alpha channel, so it vanishes on
 a white drafting surface — including in the step-2 picker, where the
 operator places correspondences. `tintDensityRasterForLightCanvas` is the
 light variant: a flat RGB rewrite that preserves alpha, so it does not
 re-run PCL. `SatelliteMapWidget` caches it in `map_raster_painted_` and
 keeps `map_raster_` pristine (tinting the tinted copy would compound);
 `SatelliteScreen::updateCorrespondenceUi` applies it to the PCD pane only
 — the satellite stitch is a photograph and must never be recolored.
 `PanZoomImageWidget::setImage` deliberately keeps the current pan/zoom on
 a **same-size** replacement, which is what makes the re-tint safe: a
 theme flip half-way through picking pairs must not refit the view.
- **The step-5 scan page's inline styles have a second theme seam beyond
 `restyleScanPage()`**: `FPVCameraView`'s placeholder is transparent over
 its host card, so its text is the only thing carrying contrast.
 `FPVCameraView::setDarkMode` is called from both `SatelliteScreen` and
 `PlannerScreen`.
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
- **Do not stamp `last_executed_at` at launch** or on a failed / cancelled /
 watchdog-deferred finalize. COMPLETED means data is on disk; an aborted
 mission must leave the plan PLANNED so it can be re-run. Only
 `markCurrentPlanCompleted()` writes the stamp, and only from the two
 finalize-success branches — abort-and-save is a third path that
 deliberately does **not** stamp. Do not raise the auto-prune above
 `kCompletedPlansKept = 5` without operator signoff, and never let
 `pruneCompleted()` touch a PLANNED plan.
- **Do not re-enable the rotate handle on an unselected marker.** The
 selection gate exists so a drag near the arrow cannot spin the heading;
 `hitTest` returns `RotateMarker` only while `marker_selected_`.
- **Wheel pans, `Ctrl`+wheel zooms, and the tool stack shows all four pills
 in both trims.** A trackpad has no middle button and the draw tools own the
 left one, so two-finger scroll is the operator's only pan gesture — step 3
 opens with polygon draw armed, and before this the canvas could not be
 panned from the moment the step opened. The cost is that the wheel can no
 longer zoom out, which is why the zoom-out and ruler pills are no longer
 hidden in the field trim (Figma 238:4518 carries zoom-in + fit only).
 `Ctrl`+wheel accumulates `kWheelNotch = 120` of `angleDelta` per level
 instead of stepping one per event; without it a single trackpad flick
 crossed ten levels. Do not remap scroll back to zoom without giving the
 field trim another pan gesture first.
- **`maybeRearmRoiDraw()` is what keeps step 3 drawable.** The ruler — and
 anything else that calls `cancelInteraction()` — disarms the draw, and the
 field's step-3 rail has no Draw button, only Clear ROI, which wipes. So the
 screen re-arms on `interactionChanged` whenever there is no polygon. Gate
 on `polygon().valid()`: `armPolygonDraw()` assigns `polygon_ = RoiPolygon{}`,
 so re-arming over a ring of three or more destroys the operator's shape.
 Rings of one or two vertices are already dropped by `cancelInteraction()`,
 which is why "no polygon" is the right gate.
- **`SatelliteMapWidget::setViewBounds` is the zoom-out ceiling**, imagery
 canvas only (the measured canvas floors on the collected map's hull). It
 feeds BOTH `minZoomNow()` and `clampCenter()` — a zoom floor without the
 pan clamp still lets the operator slide off the roof, or off the cached
 tiles. Cached site → the manifest's disc capped at `kMaxViewRadiusM = 500`;
 nothing anchored → the continental US box, which is what keeps the boot
 view at `kDefaultZoom = 5` legal. `applyAlignmentAnchor` re-centres the
 disc on the surveyed origin ONLY when `site_manifest_.radius_m > 0`:
 inventing a radius for a manifest that has none clamps the view onto a
 bogus anchor and the ROI vanishes off-screen. The `roi` shot mode is the
 tripwire — its synthetic manifest carries no radius. Measured plans live
 at lat/lon 0: `applySiteViewBounds` must `clearViewBounds()` on
 `plan_mode_ == Measured`, not the widget's imagery flag — `loadJob` runs
 before `applyModeVisibility()`, so the flag is still stale-true. `measured`
 / `measured_map` shots catch a regression.
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
 (worktree `~/BDR_data/pilot_ws`), tip `dbc303e`: GPS-stamped
 `map_collection_node` + `roi_vertices` / `roi_edge_flags` on
 `robot_autonomous_coverage_director.launch.py` + differential erosion in
 `coverage_planner_core.py`. `roof_edge_clearance` was dropped in
 `8e0fc53` (seeded strip is the open-edge standoff); the OCU never
 sent that arg. Robot must be rebuilt from that branch for the
 state pill + edge setback to be live.
 Do **not** go back to `feature/ocu-satellite-roi` despite its name: that
 branch only carries the older `robot_autonomous_coverage.launch.py` +
 `coverage_horizon_manager.py` stack, and the OCU launches the *director*
 stack (`satellite_mission_controller.cpp`), which exists only on
 `cliff-on-autonomy`. `feature/ocu-satellite-roi` has its own independent
 `roof_edge_clearance` implementation in `coverage_horizon_manager.py` —
 that one is the dead horizon-manager path, kept only for history.

## Touch input on the three canvases (production-wired)

The field laptop is a touch panel and the operator works standing on a roof,
so the map canvases take pinch/pan/twist directly. Two headers carry it:
`cpp/include/touch_gesture_state.hpp` is the Qt-free state machine (Idle /
Single / Pinch, tap slop, scale accumulator, twist dead zone) and is unit
tested by `tests/touch_gesture_state_tests.cpp`;
`cpp/include/touch_canvas_gestures.hpp` is the Qt glue plus
`SynthesizedMouseGuard`, which swallows the mouse events the system
synthesizes from touch so a pinch can never also read as a click or drag.
Three widgets consume them: `SatelliteMapWidget`, `PlotWidget` and
`PanZoomImageWidget`. Mouse and wheel behaviour is unchanged on all three.

One finger always pans. Two fingers pinch, anchored on the live midpoint, and
twist. Gestures are ungated — always on, no preference.

### Rules for agents touching this path

- **A tap on a canvas places nothing, and that is a correctness
  requirement, not an oversight.** ROI vertices and correspondence picks
  need pointer accuracy: a fingertip tap is worth several pixels of slop,
  and a misplaced correspondence silently corrupts the alignment fit that
  every exported ROI vertex inherits. So all three canvases dropped tap
  forwarding — the operator pans and zooms with fingers and *picks* with
  the trackpad. Do not "restore" tap-to-pick on any of them.
- The one carve-out is `SatelliteMapWidget::canvasTapped()`, which replaces
  the forwarded click. `SatelliteScreen` consumes it **only on step 5**, to
  leave manual override, because that is the one canvas action with no
  precision requirement. Adding a second consumer means re-arguing the rule
  above.
- **Pinch zoom is fractional.** `zoom_frac_` / `setContinuousZoom()` split
  the view zoom into the integer tile level and a painted remainder, because
  on integer levels alone a pinch reads as dead until it suddenly jumps 2x,
  and operators overshoot. `worldPixels()` is the single scale source;
  do not reintroduce `256 << zoom_` at a call site.
- **`paintTiles` installs the view rotation only when the bearing is
  non-zero.** A `painter.rotate(0.0)` still promotes the painter's transform
  type, which moves every tile blit off QPainter's aligned path onto the
  general transformed path and resamples on a different grid — merely
  *having* the rotation feature shifted the whole canvas by ~6/255 per
  channel on a north-up view. The tripwire is that a bearing-0
  `BDR_DEV_STAGE6_SHOT` is pixel-identical to a pre-rotation build.
- **Rotation is imagery-only.** `bearingSupported()` is `imagery_enabled_`:
  the measured/CAD canvas draws an axis-aligned metric grid and has no
  bearing to show or reset. `measured` / `measured_map` shots catch a
  regression.
- **The compass pill is not optional.** A twist is easy to trigger by
  accident while pinching, so every rotatable surface must always show where
  north is and reset in one click — `SatelliteScreen::refreshCompass()` for
  the map (it is the 5th pill in the canvas tool stack, hidden in measured
  mode) and `PanZoomImageWidget`'s own `reset_rotation_` chip for the
  alignment panes. A bearing with no way back is how an operator loses
  track of which way the roof faces.
- `PanZoomImageWidget`'s bearing is a **view** property only: `pointPicked`
  keeps emitting image pixels, so the similarity fit and both pick-precision
  sigmas are untouched by a twist.
- `BDR_DEV_STAGE6_SHOT_BEARING=<deg>` renders a rotated Stage 6 view
  headlessly — the agent-side check for the rotation paths.

## Theming (light mode is a field requirement, not a preference)

The operator runs this on a laptop on a roof in direct sun, where the
screen's black is grey with glare. Light mode is the sunlight mode, so
"readable in light mode" is a functional requirement and the reason the
contrast bar is high: **AAA (7:1) for small body text**, AA for button
labels, which are all ≥ 14 px bold.

`cpp/include/ui_theme_constants.hpp` is the only palette. `uiThemeTokens
(bool)` is the explicit form; `appThemeTokens()` / `appDarkMode()` read the
`bdrDarkMode` property that `main.cpp` seeds on `QApplication` **before any
widget is constructed** and `AppShellWindow::setDarkMode` keeps in sync —
that is how frameless dialogs get the right theme without being handed it.

### Decisions that are load-bearing

- **Green buttons keep the brand fill and swap the label.** White on
 `#00BC7D` is 2.5:1 and on `#009966` is 3.3:1. `on_accent` is `#FFFFFF`
 in dark and `#18181B` in light. Do not "fix" a green button by
 darkening the fill instead — the app is consistent on the label, and
 mixing the two approaches is what made the Setup and Stage 6 buttons
 diverge. `accent_text` is the separate token for green as *text* on a
 surface, where the fill value is unusable.
- **Fills and text colors are different roles and cannot share a value.**
 `danger` / `warning` are text (they go *darker* in light mode, against a
 light surface); `danger_fill` / `warning_fill` carry a white label (they
 also go darker, for the opposite reason). One value fails one of them.
- **In light mode `faint` IS `muted`.** Dark mode has four grey tiers;
 light mode runs out of contrast headroom after three, and anything
 lighter than `muted` on white drops under the bar. Hints, unit suffixes
 and section headers get their hierarchy from size and weight instead.
 Do not lighten `faint` to "get the tier back".
- **Disabled needs two tokens.** `disabled_text` is a label ON a disabled
 fill; `disabled_ghost` is a label on a disabled *transparent* control.
 The same value cannot do both — with no fill under it, `disabled_text`
 looks enabled. Qt cannot restyle a child `QLabel` from a parent's
 `:disabled` pseudo-state, so composite buttons (step 5's control bar)
 need the `applyScanActionFg` / `ScanActionTint` treatment instead.
- **A sheet set in a constructor holds the boot palette forever.** Every
 screen here is built once and reused across missions. Anything styled
 per-element inside a builder needs a re-style hook on the theme toggle:
 `restyleScanPage()` for step 5, `applyPlaceholderStyle()` for
 `FPVCameraView`, `setDarkMode` on the painted widgets.
- **A dialog with no render path is a dialog nobody verified.** Every
 Stage 6 confirmation — Leave to Dashboard, Collect Map from Robot,
 Confirm ROI, Edges Reviewed, Launch, Cancel / Complete Mission, and
 the two modeless notices — is ONE file-local helper,
 `makeSatPrompt` in `satellite_screen.cpp`. It shipped hardcoded dark
 through the whole light-mode sweep because it is not a
 `components/*_dialog.cpp` file and had no shot mode. Its chrome now
 lives in `applySatPromptStyle(QDialog*)` off `appThemeTokens()`, and
 `SatelliteScreen::restyleOpenPrompts()` (from `setDarkMode`) re-runs it
 over any open prompt — the revisit and robot-stopped notices are
 modeless and can outlive a toggle. The title and body labels carry
 object names instead of their own sheets so that one call restyles
 everything; do not put a `setStyleSheet` back on them.
 Do NOT consolidate this into `BdrMessageBox`: those two notices need
 `show()` (an `exec()` nests a loop in which the launch can die under
 the operator) and `BdrMessageBox` is `ApplicationModal` with different
 chrome. Destructive accepts ("Clear & Leave", "Cancel Mission") stay
 brand green on purpose — `onRobotSweepFailed` swaps its accept between
 "Launch anyway" and "Cancel launch" depending on which is primary, so
 there is no safe blanket rule mapping accept to `danger`.

### Verifying

`BDR_DEV_STAGE6_SHOT` with `_STAGE=1|2|3|4|5` and `_MODE=…|dialogs` renders
every screen and the shared dialogs headlessly in both themes — `dialogs`
also writes the Stage 6 confirm prompt to `<png>_prompt.png`. The shot pins
the window to **1920x1080**, the Figma frame size and the field laptop's
panel; do not shrink it. The staged constants are frame px unscaled, so a
shorter shot gives the layout less vertical budget than it will ever have
and manufactures phantom clipping — at 860 px step 5's right rail squeezed
the Manual Override and Scan Statistics cards together. Contrast
claims should be measured off those PNGs at the glyph core, not eyeballed —
point-sampling a label hits antialiasing and reads far lighter than the
text actually is.

## Docs worth reading

- `cpp/CLAUDE.md` — authoritative architecture overview and build notes.
- `docs/DEV_BYPASSES.md` — the re-wiring checklist (see above).
- `docs/OTA.md` — OTA state machine, runner UX, field-test recipe.
- `docs/ROBOT_SYNC.md` — laptop↔robot `~/pilot_ws` sync, gates, safety.
- `docs/TILT_CALIBRATION_PLAN.md` — tilt calibration design + TODO list.
- `docs/SATELLITE_WORKFLOW.md` — Stage 6 operator narrative: office
  prefetch, ROI drawing, map collection, correspondence alignment, Send.
- `docs/AUTONOMY_CONOPS.md` — north-star concept of operations for
  autonomous scan-while-exploring (edge/obstacle detection, on-robot
  coverage, ROI flow, RTK/frontier roadmap). Design intent, not yet built.
- `pilot_ws/src/pilot_control/docs/FASTLIVO2_CAMERA_NOTES.md` —
  See3CAM_24CUG vendor-doc study + FAST-LIVO2 migration plan (see the
  FAST-LIVO2 section above).
