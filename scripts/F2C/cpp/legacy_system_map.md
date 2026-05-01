# Legacy System Map: F2C C++ Coverage Planner

Scope: `pilot_control/scripts/F2C/cpp` (legacy, fully featured C++ coverage planner).

## Core Architecture and Data Flow

The system is a single Qt desktop application that orchestrates planning, visualization, robot IO, and data logistics. At runtime it initializes ROS2 and a Qt event loop, then drives a GUI-centric workflow with optional background workers and a dedicated ROS spin thread.

End-to-end lifecycle (typical path):
- **Startup**: `src/main.cpp` initializes ROS2, creates a `QApplication`, and launches `f2c_cpp::CoverageGUI`.
- **Map acquisition**:
  - Load from local PCD/PLY/XYZ files (async supported), or
  - Pull latest robot map via SSH + SCP (find most recent `.pcd` on robot).
- **Preprocessing**:
  - Height crop relative to Z=0 (robot origin),
  - Downsample (random, voxel grid, or statistical outlier removal),
  - 2D projection for planar planning.
- **Geometry extraction**:
  - Concave hull via CGAL alpha-shape / Delaunay / grid, or convex hull fallback,
  - Optional Douglas-Peucker simplification,
  - Manual ROI and obstacle selection, or auto obstacle detection from point cloud + driven path.
- **Coverage planning**:
  - Build effective field: `(boundary ∩ ROI) − obstacles`,
  - Generate swaths (Fields2Cover if available, fallback otherwise),
  - Route ordering (boustrophedon / snake / spiral),
  - Path planning (Dubins / Dubins CC / Reeds-Shepp / Reeds HC / straight),
  - Axial-turn fallback when obstacles present or configured,
  - Optional resampling to fixed waypoint spacing.
- **Output & execution**:
  - Export CSV of path (planned or custom),
  - Publish waypoints to ROS `/f2c_waypoints`,
  - Send start signal (Float64MultiArray `{0.0}`) to begin navigation,
  - Start scan session tracking (GPS + stats persistence).
- **Operational loops**:
  - Live robot tracking overlays via odometry,
  - Live progress/ETA stats against planned path,
  - Data download/upload pipelines via dialogs.

## Core Modules (Responsibilities)

Primary orchestration:
- `coverage_gui.hpp/.cpp`: Main window, workflow state, ROS2 integration, UI controls, plotting, statistics, and dispatch to pipeline.
- `coverage_pipeline.hpp/.cpp`: Point cloud IO/filtering, geometry utilities, coverage generation, and export helpers.

Visualization:
- `coverage_gui.hpp/.cpp` (`PlotWidget`): 2D map rendering, ROI/obstacle selection, custom path drawing, overlays.
- `coverage_gui.hpp/.cpp` (`VideoStreamWidget`): Embedded GStreamer pipeline for video.
- `qcustomplot.h/.cpp`: Optional plotting backend (if bundled).
- `resources.qrc` + `assets/`: GUI assets.

Planning support:
- `obstacle_detector.hpp/.cpp`: Automated obstacle detection (RANSAC ground + DBSCAN clustering + polygonization).
- `preset_manager.hpp/.cpp`, `preset_dialog.hpp/.cpp`: Parameter presets with JSON persistence.

Robot IO and operations:
- `teleop_widget.hpp/.cpp`: Keyboard teleop, ROS cmd_vel, and robot service controls.
- `scan_session_tracker.hpp/.cpp`: GPS accumulation and scan stats persistence.
- `robot_registry.hpp/.cpp`: Robot ID registry and SSH profile lookup.

Data logistics:
- `transfer_manager.hpp/.cpp`: Queue-based rsync downloads via SSH (with history/persistence).
- `data_transfer_dialog.hpp/.cpp`: UI for browsing/downloads.
- `cloud_upload_manager.hpp/.cpp`: Queue-based S3 uploads via AWS CLI, metadata, geocoding.
- `cloud_upload_dialog.hpp/.cpp`: UI for cloud uploads.
- `network_monitor.hpp/.cpp`: Internet + AWS status checks.

Legacy shell (present but not in main entry):
- `app_shell.hpp/.cpp`, `setup_screen.hpp/.cpp`, `startup_screen.hpp/.cpp`: Multi-stage setup shell (not invoked by `main.cpp`).

## Data Models and Key Abstractions

Core geometry & planning types (in `coverage_pipeline.hpp`):
- `Point2D`, `Point3D`, `Polygon2D`, `Obstacle2D`
- `Swath`, `PathState`, `SwathList`, `PathStateList`
- `CoverageConfig`: planning parameters (swath width, headland, turn radius, planners, decomposition, spacing)
- `CoverageResult`: swaths/route/path + effective area and error info

GUI + analytics:
- `CoverageStats` (coverage, path length, waypoints, ETA, overlap)
- `ReprojectionLine` (planned vs traversed error visualization)
- Live stats caches: planned path points + cumulative distance + live progress.

Operational data:
- `RobotProfile` and `RobotRegistry` (robot_id -> SSH profile + optional key pinning)
- `PlanningPreset` (saved parameter sets)
- `SectionInfo` / `SubFolderInfo` / `TransferJob` (download queue)
- `AwsConfig`, `ScanMetadata`, `UploadJob` (cloud upload queue)
- `ScanSession` / `GpsAccumulator` (session persistence)

## Complete Feature Inventory

### Map ingest and preprocessing
- Load PCD/PLY/XYZ from file; async loading to keep UI responsive.
- Fetch latest map from robot using SSH `find` + SCP download to local dated folder.
- Height crop relative to robot origin (Z min/max).
- Downsampling: random subsample, voxel grid, statistical outlier removal.
- 3D viewer launch (Open3D preferred, pcl_viewer or CloudCompare fallback) with optional path overlay.

### Boundary extraction and geometry utilities
- Concave hull with CGAL alpha shape, Delaunay, or grid-based boundary; convex hull fallback.
- Polygon simplification (Douglas-Peucker).
- Polygon validity checks, intersection/difference with ROI/obstacles via Boost.Geometry.
- Effective area calculation for stats (boundary ∩ ROI − obstacles).
- Swath alignment via minimum-rotated rectangle angle.

### ROI and obstacle management
- Manual ROI polygon selection with undo/cancel.
- Manual obstacle polygon selection, selection and deletion.
- Auto obstacle detection:
  - Footprint ground sampling using driven path,
  - RANSAC ground plane fitting (or median fallback),
  - Ground band separation and candidate extraction (including troughs),
  - Statistical outlier removal,
  - DBSCAN clustering,
  - Cluster merging and polygonization (grid/hull, hole preservation),
  - Micro-obstacle preservation (small but dense clusters).

### Coverage planning (Fields2Cover or fallback)
- Optional decomposition (boustrophedon or trapezoidal).
- Headlands generation.
- Swath generation per cell.
- Route ordering (boustrophedon, snake, spiral).
- Path planning (Dubins, Dubins CC, Reeds-Shepp, Reeds HC, or straight).
- Axial-turn path generation for obstacle-safe transitions.
- Resampling to fixed waypoint spacing for controller compatibility.
- Fallback “simple coverage” if Fields2Cover is missing.

### Path modes and output
- Planned path mode (Fields2Cover pipeline).
- Custom path mode: click-to-add waypoints, undo, clear, visited tracking.
- Export path CSV (planned or custom).
- Publish waypoints to ROS `/f2c_waypoints` (planned, custom, or selected segments).
- Start navigation signal (Float64MultiArray `{0.0}`).
- Optional upload of mission CSV to robot via SSH/SCP.

### Scan segmentation and execution tracking
- Segment planned/custom path into scan chunks by length.
- Publish selected scan segments.
- Track per-segment completion based on live progress.
- Reprojection error analysis: compare traversed trail vs planned path.

### Live tracking and analytics
- Subscribe to robot odometry; render live pose and trail.
- Live progress: completed distance, remaining, speed, ETA, elapsed.
- Visual overlay of live stats on the plot.
- Coverage stats panel: coverage percent, area, turns, waypoints, estimated time.

### Teleoperation and robot actions
- WASD keyboard teleop, UI button controls.
- ROS publishers/services:
  - `/cmd_vel`, `/mpc_autonomy_enable`
  - `/save_raw_map`, `/video_record_set`, `/rosbag/toggle`
  - `/gpr_scan/toggle`, `/gpr_line_start`, `/gpr_line_stop`

### Video streaming
- GStreamer-based UDP video playback in-app.
- ROS topics to select camera and configure stream target:
  - `/stream_camera_select`, `/stream_camera_status`
  - `/stream_target_ip`, `/stream_status`
- Local IP detection to guide stream target configuration.

### Data download and upload logistics
- Robot data download via `rsync` over SSH:
  - Date/section discovery, subfolder selection, queue, resume, retries,
  - Progress display, speed/ETA, persistent download history.
- Cloud upload via AWS CLI:
  - `aws s3 sync` (resumable),
  - metadata JSON generation and upload,
  - reverse geocoding (OpenStreetMap Nominatim),
  - upload history and verification (`aws s3 ls`).
- Network monitor:
  - Ping-based connectivity + stability stats,
  - AWS CLI presence/credentials/bucket validation.

### User preferences and persistence
- Presets: create, update, rename, duplicate, import/export (JSON).
- Session metadata persistence (scan_sessions.json).
- Transfer history and AWS configuration in user settings.
- Robot registry resolution via `robots.json` in config/share locations.

## State and Event Management

State model:
- Centralized in `CoverageGUI` with explicit fields for map, ROI, obstacles, swaths, route, path, custom waypoints, and session info.
- Robot tracking state protected by mutex (odometry updates vs UI rendering).
- Live stats cached and recomputed from odometry against a planned path cache.
- Login state is in-memory only (token + expiry), gates privileged actions.
- Scan session state maintained in `ScanSessionTracker` and persisted to JSON.

Event propagation:
- **Qt signal/slot** is the primary event mechanism across UI, dialogs, and managers.
- **QtConcurrent + QFutureWatcher** for background tasks (point cloud load, obstacle detection).
- **QTimer** for:
  - ROS reconnect, Zenoh bridge checks,
  - login session countdown,
  - transfer/upload progress and retry loops,
  - periodic network monitoring.
- **ROS2** runs in a background thread; callbacks push updates to UI via `QMetaObject::invokeMethod`.
- **QProcess** subprocesses drive SSH/SCP/rsync, ping, aws CLI, and external viewers.

Consistency strategy:
- Rebuilding caches when path/waypoints change (planned path cache, live overlay).
- Clearing dependent outputs when upstream data changes (e.g., changing hull clears swaths/route/path).
- UI gating for actions requiring login or published waypoints.

## External Integrations and Boundaries

ROS2 (runtime robotics interface):
- Topics/subscriptions:
  - `/f2c_waypoints` (publish path + start signal),
  - `/Odometry_tilt_corrected_diff` (robot pose + trail),
  - `/gps/fix` (GPS accumulation for scan sessions),
  - `/stream_camera_select`, `/stream_camera_status`,
  - `/stream_target_ip`, `/stream_status`.
- Services (teleop):
  - `/save_raw_map`, `/video_record_set`, `/rosbag/toggle`,
  - `/gpr_scan/toggle`, `/gpr_line_start`, `/gpr_line_stop`.

Robot host and mission control:
- SSH for authentication via `pilot_control_auth login` (JSON response).
- SCP for map download and mission CSV upload.
- Optional key pinning via generated `known_hosts` file.

File transfer:
- `rsync` over SSH with queue and resume support.
- Local storage under user config and data folders.

Cloud services:
- AWS CLI (`aws s3 sync`, `aws s3 ls`, `aws s3 rm`, `aws sts get-caller-identity`).
- OpenStreetMap Nominatim reverse geocoding (HTTP).

Video and visualization:
- GStreamer pipeline for embedded video.
- External viewers (Open3D via Python, pcl_viewer, CloudCompare) launched by the app.

Middleware and runtime environment:
- CycloneDDS loopback config via `CYCLONEDDS_URI`.
- Zenoh bridge detection via `pgrep zenohd` (bridge managed externally).

Core libraries:
- Qt (Core/Widgets/Concurrent/Network), PCL, Eigen, Boost.Geometry,
- Fields2Cover (optional), CGAL (optional), GStreamer, ROS2.

