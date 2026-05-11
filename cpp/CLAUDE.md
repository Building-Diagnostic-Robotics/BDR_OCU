# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

BDR Coverage Planner — the Operator Control Unit (OCU) and coverage planning GUI for "Roofus," an autonomous mobile robot that performs building roof scanning. Built with C++17, Qt Widgets, and ROS2 (rclcpp). All code lives in the `f2c_cpp` namespace.

## Build

```bash
# Standard build (Release)
./build.sh

# Debug build
./build.sh Debug

# Manual CMake
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run
./build/bdr_coverage_planner
```

The build requires a sourced ROS2 Humble workspace (`pilot_ws`). Optional dependencies: `Fields2Cover` (`HAVE_FIELDS2COVER`), `CGAL` (`HAVE_CGAL`). The app prints which features are active at startup.

**Debian package:**
```bash
./build.sh && ./create_deb.sh
sudo dpkg -i build/bdr-coverage-planner_1.0.0_amd64.deb
```

## Architecture: Staged Screen Flow

`AppShellWindow` (`app_shell.hpp/cpp`) is the single `QMainWindow`. It owns a `QStackedWidget` that routes between five stages:

| Stage | Class | Purpose |
|-------|-------|---------|
| 1 | `SetupScreen` | Initial config (robot selection, settings) |
| 2 | `StartupScreen` | Pre-flight checks / login |
| 3 | `DashboardScreen` | Robot status overview |
| 4 | `ExplorationScreen` | Live scan operation with teleop |
| 5 | `PlannerScreen` | Post-scan coverage path planning |

Each stage is a self-contained `QWidget`. `AppShellWindow` wires them together via signals/slots and holds all ROS2 node state. Stages are instantiated lazily (`ensureStage2()` etc.) and only created once. Stage transitions happen through `goToStageN()` slots.

## Critical Rules

**No QML, no `.ui` files.** All UI is built programmatically in C++.

**Thread safety:** ROS2 callbacks and `QProcess` handlers must NEVER touch Qt widgets directly. Always cross the thread boundary:
```cpp
QMetaObject::invokeMethod(target, [=]() { /* UI update */ }, Qt::QueuedConnection);
```

**Frameless windows:** All dialogs use `setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint)`. No native OS chrome.

**Theming:** Use `uiThemeTokens(bool dark_mode)` from `ui_theme_constants.hpp` for all colors. Never hardcode color values inline — read the token struct. The app supports dark/light toggle.

## Key Components

**`CoveragePipeline`** (`coverage_pipeline.hpp/cpp`) — pure C++ (no Qt). Wraps Fields2Cover for boundary polygon → swaths → path. Uses PCL for point cloud loading/filtering, CGAL for alpha-shape concave hulls (falls back to convex hull without CGAL).

**`RobotRegistry`** (`robot_registry.hpp/cpp`) — loads `config/robots.json` at startup. Maps `robot_id` (e.g., `"Roofus#001"`) to connection profile (host IP, SSH user, radio IP, SNMP OIDs). Robot IPs are never shown in the UI.

**`TransferManager`** (`transfer_manager.hpp/cpp`) — singleton. Manages rsync-over-SSH transfers from robot (`/R_DATA/`) to laptop. Queue-based with resume (`--partial`), retry, and checksum verification.

**`ScanSessionTracker`** — tracks in-progress and completed scan sessions.

**`CloudUploadManager`** — handles post-scan cloud upload workflow.

**`NetworkMonitor`** — polls robot reachability; also reads SNMP RSSI/SNR from the radio.

**`PresetManager`** / **`PresetDialog`** — save/load named `CoverageConfig` presets via `QSettings`.

## Reusable UI Components (`src/components/`, `include/components/`)

- `BdrMessageBox` — frameless replacement for `QMessageBox`
- `BdrProgressDialog` — frameless progress dialog
- `BanterLoaderWidget` — animated loading indicator
- `TiltCalibrationDialog` — 3-page (Setup → Progress → Success) dialog that SSHs into the robot and runs tilt calibration
- `MissionMetadataDialog` — frameless "New Scan Information" modal shown when the operator clicks **Start New Scan** on Stage 3 Dashboard. Collects building name, operator name, and a Metric/ANSI units toggle, persists them to `QSettings`, and gates the transition to Stage 4. See **Mission Metadata + Units** below.

## ROS2 Integration

The ROS2 node (`exploration_ros_node_`) is owned by `AppShellWindow` and spins in a background thread. Subscriptions cover: odometry, ODrive motor controller status, thermal thumbnails, local nav grid, and stream status. Publishers: `cmd_vel` (teleop), stream target. Service clients: save map, axis state (arm/disarm), GPR power-off, video record, **`/data_collection_coordinator/set_parameters`** (mission metadata push — see below).

`CYCLONEDDS_URI` is cleared at startup if it references `rf_cyclonedds.xml` (which would break zenoh-ros2dds discovery). The app expects `RMW_IMPLEMENTATION=rmw_cyclonedds_cpp` and a loopback CycloneDDS config at `~/cyclone_loopback.xml` when run manually.

## Settings Persistence

User settings are stored via `QSettings` under org `"PilotControl"`, app `"BDRCoveragePlanner"` (see `settings_constants.hpp`). Use these constants — don't hardcode org/app strings.

## Data Layout on Robot

Scan data lives at `/R_DATA/<Month_DD_YYYY>/<building_slug>/Section_<N>_<HHMMSS>/` with sub-folders: `Visual_data/`, `GPR_scan_data/`, and map files. The `<building_slug>` parent (e.g., `Acme_HQ`) is derived from the operator-entered building name via the New-Scan-Information modal — see **Mission Metadata + Units** below. Sibling `Mission_<HHMMSS>/mission_config.json` and per-section `session_config.json` carry the operator name, units preference, and building slug for downstream AWS-per-client tracking.

`TransferManager` (SSH/rsync) and `CloudUploadManager` (AWS S3) mirror this structure both locally and in the cloud — `s3://bucket/prefix/<date>/<building>/<section>/...`. Path consumers that walk `/R_DATA` (`DashboardScreen` Total Scans probe, `TransferManager::fetchSectionsForDate`, `CloudUploadManager::scanLocalData`) all expect depth-3 (`<day>/<building>/<section>`) — pre-modal depth-2 sections are not surfaced.

## Mission Metadata + Units

The **New Scan Information** modal (`MissionMetadataDialog`) intercepts `DashboardScreen::startNewScanRequested` in `AppShellWindow::onStartNewScan` *before* advancing to Stage 4. It collects three pieces of session-wide state, persists them to `QSettings` (keys in `settings_constants.hpp`: `kSettingsBuildingNameKey`, `kSettingsOperatorNameKey`, `kSettingsUnitsKey`), and applies a `QGraphicsBlurEffect` to the Dashboard underneath while open.

| Field | Purpose | Used by |
|-------|---------|---------|
| Building name | Slugified to `<building_slug>` for the data layout above | Robot-side `data_collection_coordinator.py` |
| Operator name | Audit / metadata.json | Cloud upload metadata |
| Units (Metric / ANSI) | **Display-only** preference | `UnitsProvider` singleton |

**`UnitsProvider`** (`units_system.hpp/cpp`) is a `QObject` singleton that owns the current `Units` selection, persists it to `QSettings`, and emits `unitsChanged()`. The `units::` namespace provides static formatting helpers — `units::formatLength(meters)`, `units::formatSpeed(mps)`, `units::formatArea(sqm)`, `units::lengthUnitSuffix()`, etc. — that produce the right text (`"1.50 m"` vs `"4.92 ft"`) based on the current selection. **All ROS payloads, internal state, and persisted scan data remain SI** — the toggle only swaps display strings. New display sites should always go through these helpers; never concatenate a hardcoded unit suffix.

UI surfaces wired through `UnitsProvider`:

- `ExplorationScreen` — telemetry cards (Speed, Position, Altitude).
- `PlannerScreen` — value labels and slider min/max badges (re-rendered live via `relabelUnitEndpointBadges()` when the operator backs out and re-toggles between missions). The "Distance per scan" `QLineEdit` keeps its meters value but shows a `(≈ X.XX ft)` hint in ANSI mode (`refreshScanDistanceAnsiHint()`); the resulting scan segment list formats lengths via `units::formatLength`.
- `PlotWidget` — ROI rectangle dimensions, scan-segment hover tooltip, cursor coordinate tooltip.

**Metadata push to robot:** `AppShellWindow::sendDataCollectorSessionMetadata()` calls `rcl_interfaces/srv/SetParameters` on `/data_collection_coordinator` to push `building_name`, `operator_name`, `units_preference` as ROS string parameters. It is invoked from `onPlannerScanStartRequested()` (Stage 5 **Start Scan**) *before* `sendControllerMaxLinearVelocity` and the `mpc_autonomy_enable=true` publish — if the push fails (service unreachable within 1.5 s, response missing/partial, any param rejected), the function shows a `BdrMessageBox` and **hard-blocks** the scan from arming. The autonomous controller (`mpc_accel_autonomous_controller.py`) fires `/dc/start` a few hundred ms after `autonomy_enable=true`, by which point the coordinator's `ensure_mission_session()` has the metadata it needs to land the section folder under `/R_DATA/<day>/<building_slug>/Section_*/`.

**Zenoh allowlist:** `pilot_control/config/zenoh/zenohd_robot.json5` `service_servers` must include `^/data_collection_coordinator/set_parameters$` and `^/data_collection_coordinator/set_parameters_atomically$` for the push to reach the robot — both are present.
