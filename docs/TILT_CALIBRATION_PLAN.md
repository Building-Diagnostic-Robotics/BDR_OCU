# Tilt Calibration Implementation Plan

## Executive Summary

This document provides a detailed plan for integrating **tilt calibration** into the BDR Coverage Planner. The tilt calibration corrects LiDAR mount tilt to improve odometry and LiDAR map accuracy. It is critical for robots deployed on roofs (flat, uneven, and rough), where mechanical wear and environmental stress can change the LiDAR–body alignment over time.

---

## 1. Tilt Calibration Script Overview

### 1.1 Implementation (`/home/avenblake/pilot_ws/src/pilot_control/scripts/tilt_calibration.py`)

| Aspect | Details |
|--------|---------|
| **Entry point** | `ros2 run pilot_control tilt_calibration` |
| **Input** | IMU accelerometer data from `/livox/imu` (Livox MID360) |
| **Output** | `/R_DATA/tilt_calibration/tilt_correction_matrices_<index>.npz` + CSV |
| **Algorithm** | Averages ~100 IMU samples → gravity vector in body frame → pitch-only rotation (R_align) → applies R_flip → saves R_map = R_flip @ R_align |
| **Assumption** | Robot must be **stationary** on **level ground** during calibration |
| **Optional** | `start_lidar:=true` (default) – auto-starts Livox driver; `num_samples:=200` for higher precision |

### 1.2 Data Flow After Calibration

```
tilt_calibration.py
       ↓
tilt_correction_matrices_<index>.npz  →  robot_complete.launch.py (picks latest)
       ↓
odom_tilt_corrector  →  /Odometry_tilt_corrected_diff
       ↓
pose_controller, unified_data_collector, raw_map_saver, OCU (`bdr_coverage_planner` / dashboard & planner stages)
```

- **odom_tilt_corrector**: Loads `R_map`, transforms Fast-LIO2 odometry from tilted LiDAR frame to ground-aligned robot body frame.
- **raw_map_saver**: Applies tilt correction when saving raw LiDAR maps.
- **Fallback**: If no calibration file exists, a fixed 15° pitch is used.

---

## 2. Calibrate Button Flow

### 2.1 UI Placement

- **Location**: Dashboard Quick Actions (alongside Start New Scan, Run Diagnostics, View Recordings), or as a dedicated action on the Calibration card.
- **Label**: “Calibrate Tilt” or “Run Tilt Calibration”.
- **Icon**: Settings or level icon to indicate alignment/calibration.

### 2.2 Flow When Calibrate Is Pressed

```
┌─────────────────────────────────────────────────────────────────────────┐
│ 1. User clicks "Calibrate Tilt" on Dashboard                            │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 2. Pre-Check Dialog                                                     │
│    - Robot must be stationary on level ground                            │
│    - Ensure no scan/motion in progress                                   │
│    - Takes ~30–60 seconds (LiDAR init + sample collection)               │
│    [Cancel] [Proceed]                                                    │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 3. SSH Remote Command (same pattern as Run Diagnostics)                  │
│    - Connect to robot_host (from QSettings)                              │
│    - Source ROS + pilot_ws                                               │
│    - ros2 run pilot_control tilt_calibration [options]                   │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 4. Modal/Overlay Progress UI                                             │
│    - "Starting LiDAR..."                                                 │
│    - "Collecting IMU samples (X/100)..."                                 │
│    - "Computing calibration..."                                          │
│    - Non-dismissible until complete or error                             │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 5. Result                                                                │
│    SUCCESS: "Calibration saved. Restart robot launch to apply."          │
│    FAILURE: Error message + suggestion (e.g., start_lidar:=false)        │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.3 Remote Command Implementation

Use the same pattern as `StartupScreen::startDiagnostics`:

```cpp
// Pseudocode – similar to startup_screen.cpp lines 554–574
QString script = QString(
    "set -e; "
    "if [ -f /opt/ros/humble/setup.bash ]; then source /opt/ros/humble/setup.bash; "
    "elif [ -f /opt/ros/foxy/setup.bash ]; then source /opt/ros/foxy/setup.bash; fi; "
    "if [ -f \"$HOME/pilot_ws/install/setup.bash\" ]; then source \"$HOME/pilot_ws/install/setup.bash\"; fi; "
    "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp; export ROS_DOMAIN_ID=0; "
    "ros2 run pilot_control tilt_calibration --ros-args -p num_samples:=100"
);
QString remote_cmd = QString("bash -lc \"%1\"").arg(script.replace("\"", "\\\""));
// Run via ssh with sshBaseArgs(robot_host)
```

- Use `QProcess` with `ssh` and `sshBaseArgs(robot_host)`.
- Capture stdout/stderr to update the progress UI and final result.

### 2.4 Safety and Constraints

| Constraint | Implementation |
|------------|----------------|
| Robot stationary | Inform user; optionally disable Calibrate while a scan is active |
| Level ground | User checklist item – cannot be enforced by software |
| No motion | Do not run if `robot_complete.launch` or motion nodes are active, or warn and block |
| LiDAR exclusive | Calibration starts its own Livox driver; avoid conflicts with an already running full launch |

---

## 3. When and How Often to Perform Tilt Calibration

### 3.1 Recommended Schedule

| Schedule | Use Case |
|----------|----------|
| **Initial setup** | After LiDAR/mount installation or any mechanical change |
| **Pre-deployment** | Before first scan of the day or new site |
| **Periodic (every N scans)** | Suggested: every **10–20 scans** for heavy-use roofs |
| **Periodic (time)** | Every **1–2 weeks** if robot operates daily on rough terrain |
| **After maintenance** | Any work on LiDAR, mount, or chassis |
| **After transport/impact** | If the robot is dropped, bumped, or handled roughly |

### 3.2 Triggers and Conditions

| Trigger | Action |
|---------|--------|
| **Mechanical wear** | Uneven/rough roofs cause chassis flex, loosening mounts; recalibrate when odometry/map drift is observed |
| **Temperature extremes** | Thermal expansion can shift alignment; recalibrate when operating in very different conditions |
| **Mount adjustment** | Any physical change to LiDAR or mount |
| **Software/firmware update** | If IMU or LiDAR driver behavior changes |
| **Observed drift** | Recalibration when path overlaps or map quality degrades |
| **Transfer/transport** | After shipping or moving between sites |

---

## 4. Roof Environment Considerations

### 4.1 Environmental Factors

| Factor | Effect on Tilt |
|--------|----------------|
| **Flat roofs** | Lower stress; calibration holds longer |
| **Uneven surfaces** | Vibration, flex; mounts can shift over time |
| **Rough surfaces** | Higher vibration and impact; faster wear |
| **Temperature** | Expansion/contraction of chassis and mounts |
| **UV / weathering** | Possible plastic/metal degradation |
| **Dust / debris** | Mechanical abrasion, possible obstruction |

### 4.2 Mechanical Wear

- **Mount screws**: Can loosen on rough roofs → periodic torque check + recalibration.
- **Chassis flex**: Repeated loads can bend frame → alignments drift.
- **Bumpers/suspension**: Wear can change resting pose.
- **LiDAR housing**: Thermal/mechanical stress may alter internal alignment.

**Suggestion**: Integrate calibration schedule with preventive maintenance (e.g., “calibrate every 15 scans or every 2 weeks, whichever comes first”).

---

## 5. Implementation Checklist

### 5.1 C++ / Qt (BDR_CP)

- [ ] Add “Calibrate Tilt” action (button or card action).
- [ ] Add pre-calibration confirmation dialog with checklist.
- [ ] Implement `TiltCalibrationRunner` or similar using `QProcess` + SSH (mirror `startDiagnostics`).
- [ ] Create modal/overlay for progress (or reuse diagnostics-style live log).
- [ ] Parse stdout for progress (e.g., “Collecting IMU samples... (50/100)”) and update UI.
- [ ] Handle success/failure with clear user feedback.
- [ ] Wire “Last Calibration” display to calibration metadata (e.g., timestamp from latest `.npz` or a small manifest).

### 5.2 Optional Enhancements

- [ ] Query robot for latest calibration file timestamp via SSH and show in “Last Calibration”.
- [ ] Add “Calibration Due” reminder based on scan count or time.
- [ ] Expose calibration history (list of `.npz` files) for advanced users.
- [ ] Support `start_lidar:=false` when LiDAR is already running (e.g., from a separate launch).

### 5.3 Tilt Calibration Script (pilot_control)

- [ ] Ensure script exits with code 0 on success, non-zero on failure (already in place).
- [ ] Optional: Add `--ros-args -p output_json:=/tmp/tilt_cal_result.json` for structured output if UI needs it.

---

## 6. Calibration Procedure Summary (Operator Checklist)

1. **Preparation**
   - Robot powered on.
   - Robot on **level, stable ground** (not on a slope or uneven roof).
   - Robot **stationary**; no motion, no scan in progress.

2. **Execution**
   - Open Dashboard.
   - Click “Calibrate Tilt” (or equivalent).
   - Confirm pre-check dialog.
   - Wait for calibration to complete (~30–60 s).

3. **After Calibration**
   - Restart `robot_complete.launch` (or full robot stack) so `odom_tilt_corrector` loads the new calibration.
   - Optionally run a short test scan to validate odometry and map quality.

---

## 7. Summary of Recommendations

| Topic | Recommendation |
|-------|----------------|
| **Frequency** | Every 10–20 scans or 1–2 weeks on rough roofs; more often if drift is observed |
| **Triggers** | Maintenance, transport, impact, temperature change, observed drift |
| **UI** | Dedicated Calibrate button with confirmation, progress overlay, and clear success/failure feedback |
| **Execution** | SSH + `ros2 run pilot_control tilt_calibration` (same pattern as diagnostics) |
| **Application** | Restart robot launch to load new calibration |
| **Environment** | Account for mechanical wear and roof conditions; integrate calibration into maintenance schedule |

---

*Document generated from tilt_calibration.py implementation and BDR_CP architecture review.*
