# Architecture Overview - BDR Coverage Planner (Roofus)

## Summary
- C++/Qt desktop application built with Qt Widgets (no QML or .ui files).
- ROS2 integration for telemetry, teleop, and waypoint publishing.
- Coverage planning pipeline built on Fields2Cover (with fallback), PCL, Boost.Geometry, and optional CGAL.
- Two primary UI surfaces are compiled: a staged AppShell flow and a full CoverageGUI planner window.

## Qt Architecture

### UI Technology
- UI is built directly with Qt Widgets in C++ (`QMainWindow`, `QWidget`, layouts, custom widgets).
- No QML or Qt Designer `.ui` files are present.
- Resources are embedded via `cpp/resources.qrc`.

### Application Entry
- `cpp/src/main.cpp` initializes ROS2 (`rclcpp::init`), creates `QApplication`, then constructs `AppShellWindow` and runs the Qt event loop.

### Staged Flow (AppShellWindow)
`cpp/src/app_shell.cpp` implements a `QMainWindow` with a `QStackedWidget` to host three screens:
- `SetupScreen` (`cpp/src/setup_screen.cpp`)
  - Login form for Robot ID and Access Code.
  - Connection status based on periodic ping (`QProcess` + `QTimer`).
  - Current behavior bypasses real SSH auth and simply emits `loginSubmitted`.
- `StartupScreen` (`cpp/src/startup_screen.cpp`)
  - Pre-operation diagnostics UI.
  - Runs `ros2 run pilot_control startup_preflight` on the robot via SSH (`QProcess`).
  - Fetches a JSON report via SSH and updates status cards/logs.
- `DashboardScreen` (`cpp/src/dashboard_screen.cpp`)
  - Status cards, quick actions, and system info.
  - Emits signals for logout, diagnostics, start scan, and view recordings.

AppShell stage transitions are wired in `AppShellWindow` via signals/slots:
- `SetupScreen::loginSubmitted` -> `AppShellWindow::onLoginSubmitted` -> Stage 2.
- `StartupScreen::continueRequested` -> Stage 3.
- `StartupScreen::backRequested` and `DashboardScreen::logoutRequested` route back to Stage 1.
- `DashboardScreen::startNewScanRequested` and `viewRecordingsRequested` exist but are not currently connected in `AppShellWindow`.

### CoverageGUI (Planner Window)
`cpp/src/coverage_gui.cpp` defines a separate `QMainWindow` for coverage planning:
- Layout built around a `QSplitter` with left controls, center plot, and right auxiliary panel.
- Custom `PlotWidget` for ROI/obstacle selection, path visualization, and robot tracking.
- Video panel embedding GStreamer via `VideoStreamWidget`.
- Dockable teleop controls (`TeleopDockWidget`).
- Data transfer and cloud upload dialogs with tabbed UI.
- Preset management and workflow indicators.

Note: `CoverageGUI` is compiled into the executable but is not instantiated from `main.cpp` or wired into the AppShell stage flow.

### Supporting UI Modules
- `DataTransferDialog` and `TransferProgressWidget` (robot to laptop downloads).
- `CloudUploadDialog` (laptop to S3 uploads) with `NetworkMonitor`.
- `PresetManagerDialog` for planning presets.
- `TeleopWidget` for robot teleoperation.

## State and Event Management

### Core Patterns
- Qt signals and slots are the primary communication mechanism.
- Periodic and retry logic uses `QTimer`.
- External processes use `QProcess` (SSH, ping, rsync, AWS CLI).
- Cross-thread UI updates use `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.

### Staged Flow Events
- Login and navigation are managed through signals from screens to `AppShellWindow`.
- `SetupScreen` validates inputs and emits `loginSubmitted`.
- `StartupScreen` emits `continueRequested` after diagnostics (currently always enabled).

### CoverageGUI Events
- `PlotWidget` emits ROI and obstacle selections and selection events to `CoverageGUI`.
- Coverage pipeline progress is routed via `setProgressCallback`, then queued back to UI.
- Teleop status messages are emitted from `TeleopWidget` to `CoverageGUI`.
- ROS2 callbacks update internal state and use `QMetaObject::invokeMethod` to update UI.

### Async Operations
CoverageGUI uses background workers to avoid blocking the UI:
- Point cloud loading and obstacle detection: `QtConcurrent` + `QFutureWatcher`.
- Network and AWS checks: `QThread::create` in `NetworkMonitor` and `CloudUploadManager`.

## ROS2 Integration

### Initialization and Spin
- `rclcpp::init` is called in `main.cpp`.
- `CoverageGUI` creates `rclcpp::Node` (`bdr_coverage_gui`) on startup.
- ROS2 spinning runs in a background thread (`std::thread` with `rclcpp::spin`).
- Reconnection and re-initialization logic uses `QTimer` and re-calls `rclcpp::init` after shutdown.

### Key Publishers
- `/f2c_waypoints` (`std_msgs/Float64MultiArray`): planned or custom waypoint publishing.
- `/cmd_vel` (`geometry_msgs/Twist`): teleop velocity commands.
- `/mpc_autonomy_enable` (`std_msgs/Bool`): toggle MPC autonomy.
- `/stream_camera_select` (`std_msgs/String`): camera selection for streaming.
- `/stream_target_ip` (`std_msgs/String`): stream target address.

### Key Subscribers
- Robot odometry (default `/Odometry_tilt_corrected_diff`, `nav_msgs/Odometry`): pose, trail, and live stats.
- `/gps/fix` (`sensor_msgs/NavSatFix`): scan session GPS accumulation.
- `/stream_camera_status` (`std_msgs/String`): camera status.
- `/stream_status` (`std_msgs/String`): stream target status.

### Service Clients (Teleop)
- `/save_raw_map` (`std_srvs/Trigger`)
- `/video_record_set` (`std_srvs/SetBool`)
- `/rosbag/toggle` (`std_srvs/Trigger`)
- `/gpr_scan/toggle` (`std_srvs/Trigger`)
- `/gpr_line_start` (`std_srvs/Trigger`)
- `/gpr_line_stop` (`std_srvs/Trigger`)

### Qt and ROS2 Bridge
- ROS2 callbacks update UI by queueing work onto the Qt event loop using `QMetaObject::invokeMethod`.
- Timers manage ROS2 reconnection and Zenoh bridge monitoring without blocking UI.

## Coverage Planning Logic

### Core Pipeline
`cpp/src/coverage_pipeline.cpp` and `cpp/include/coverage_pipeline.hpp` implement:
- Point cloud I/O and filtering (PCL).
- Polygon and ROI operations (Boost.Geometry).
- Coverage generation with Fields2Cover (swaths, route, path).
- Fallback coverage generation when Fields2Cover is not available.
- Route patterns: boustrophedon, snake, spiral.
- Path planning options: Dubins, Dubins CC, Reeds-Shepp, Reeds-Shepp HC, or straight-line.
- Optional resampling to a fixed waypoint spacing.

### Obstacle Detection
`cpp/src/obstacle_detector.cpp` provides automatic obstacle detection:
- RANSAC plane fitting for ground.
- Outlier removal and clustering.
- Polygonization with options for hull or grid-based methods.
- ROI-aware filtering and micro-obstacle handling.

### UI Integration
`CoverageGUI` owns planning inputs, invokes the pipeline, and displays results in `PlotWidget`.
Generated routes and paths can be exported to CSV or published to ROS2.

## Architecture Decoupling (Web UI Readiness)

Areas tightly coupled to Qt UI that would need separation to expose data via a web UI:
- `CoverageGUI` aggregates UI, ROS2 node management, planning pipeline invocation, file I/O, and network operations in a single class.
- ROS2 callbacks and planning progress directly target UI widgets (through queued Qt invocations).
- Global progress callback (`setProgressCallback`) is bound to UI and not scoped to a service layer.
- Teleop and scan tracking are UI-owned but depend on ROS2 node handles.
- Diagnostics and data transfer run SSH and system commands directly in UI classes.
- Settings persistence (`QSettings`) is scattered across UI components rather than a centralized configuration service.

## Key Files (Starting Points)
- Entry point: `cpp/src/main.cpp`
- App shell flow: `cpp/src/app_shell.cpp`, `cpp/src/setup_screen.cpp`, `cpp/src/startup_screen.cpp`, `cpp/src/dashboard_screen.cpp`
- Planner UI: `cpp/src/coverage_gui.cpp`, `cpp/include/coverage_gui.hpp`
- Planning pipeline: `cpp/src/coverage_pipeline.cpp`, `cpp/include/coverage_pipeline.hpp`
- Obstacle detection: `cpp/src/obstacle_detector.cpp`, `cpp/include/obstacle_detector.hpp`
- ROS2 teleop: `cpp/src/teleop_widget.cpp`, `cpp/include/teleop_widget.hpp`
- Data transfer: `cpp/src/transfer_manager.cpp`, `cpp/src/data_transfer_dialog.cpp`
- Cloud upload: `cpp/src/cloud_upload_manager.cpp`, `cpp/src/cloud_upload_dialog.cpp`
- Connectivity: `cpp/src/network_monitor.cpp`
