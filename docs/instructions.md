# BDR Coverage Planner — Operator Procedures & Edge Cases

Working document. Captures end-to-end operator procedures for performing a scan
plus every edge case / bug / gotcha encountered along the way. We are appending
to this file as we go; once coverage is complete the contents will be
re-organized and lifted into a published operator manual.

> **Maintenance rule:** when a new edge case surfaces during testing, add it
> under "Known Edge Cases & Recovery" with the symptom, root cause, fix, and
> the date / commit it was addressed. Do not delete entries — they become the
> manual's troubleshooting section.

---

## 1. Scan Procedure (Happy Path)

> _To be filled in as we walk through each stage. Stub below — expand with
> screenshots + button-by-button steps once the UI is stable._

1. **Stage 1 — Setup**: enter Robot ID + PIN, confirm connection.
2. **Stage 2 — Pre-flight**: wait for diagnostics green, press Continue.
3. **Stage 3 — Dashboard**: confirm robot status, start new scan.
4. **Stage 4 — Exploration**: drive the bot, capture map, save processed PCD.
5. **Stage 5 — Mission Planner**:
   1. **Map Processing**: load PCD, set voxel/z/alpha, run hull.
   2. **Coverage Planning**: select preset, generate paths, confirm route.
   3. **Scan Splitting**: pick segment count, publish segments.
   4. **Scan**: Start Scan, monitor segment progress, Complete Mission.

---

## 2. Mid-Scan Controls

### Pause / Resume
- **Pause** is a soft hold — DC stays alive, motors zero, planner status pill
  reads "Paused". Resume picks up at the same waypoint.
- **Resume Scan** re-enables `/mpc_autonomy_enable` and clears the pause.

### Emergency Stop
- Latches `scan_estop_latched_`. Motors → IDLE. DC paused via `/dc/pause`.
- Required by safety contract before any **Cancel Scan** is permitted.
- Press again to **Clear E-Stop** and resume.

### Manual Override (`Q` key on the planner stage)
- Toggles teleop control. Disables autonomy. DC is paused with reason
  `manual_override`. Engaging override at least once latches
  `scan_manual_override_engaged_once_` for the rest of the run, which is one
  of the two unlocks for Cancel Scan.

### Cancel Scan (mid-run abort)
- Amber button, X icon. Gated behind `(estop_latched OR manual_override_once)`.
- Confirmation dialog → `/dc/cancel_scan` → controller deletes the in-flight
  section + every prior section + the mission folder (UBX + config). Pipeline
  processes stay alive.
- OCU returns operator to **Map Processing** with map + hull preserved but
  planned path / segments / coverage cleared, so they must re-plan.

### Discard Scan (post-completion delete)
- Same physical button as Cancel, swaps to **dark red `#B91C1C`** + trash icon
  + label "Discard Scan" once `scan_run_state == Completed` and the operator
  has not yet pressed Complete Mission.
- Same `/dc/cancel_scan` service, but the OCU helper retries once on failure
  (worst case ~16 s wait — two 8 s ceilings).
- After success: segment list, planned path, and cached stats are all
  wiped. The OCU navigates back to **Map Processing** so the operator can
  re-plan from scratch on the same map + hull (point cloud + hull are
  preserved — only planning-and-after is cleared). Pipeline keeps running.
- After failure (both retries exhausted): a warning dialog surfaces that
  mission data may still be on the robot under `/R_DATA/<today>/` and the
  GNSS log may still be open; the operator should SSH in and clean up
  before starting another mission. After dismissing the warning, the OCU
  navigates back to Map Processing anyway — pinning the operator to a
  "Discarded (failed)" terminal state would only delay the same cleanup.

### Complete Mission
- Always available once `scan_run_state == Completed` OR
  `scan_manual_override_engaged_once_`.
- Confirmation → IDLE motors → `/dc/finalize_mission` (stops continuous mission
  GNSS, writes `mission_config.json`) → pipeline teardown → Dashboard.
- Safe to press post-Discard: `/dc/finalize_mission` no-ops gracefully when
  `mission_folder == ''`.

---

## 3. Known Edge Cases & Recovery

> Each entry follows the format:
> **Symptom**, **Root cause**, **Fix**, **Status**, **Where**.

### EC-001 — Cancel Scan → re-plan → Start Scan: bot drives but never starts DC

- **Symptom.** After cancelling a scan and going back through Map Processing
  → Coverage Planning → Scan Splitting → Scan → Start Scan, the robot navigates
  to the first waypoint but `/dc/start` is never called. No data is collected.
  At the final waypoint the controller calls `/dc/end_and_save` which fails
  because the coordinator has nothing open, autonomy gets stuck disabled.

- **Root cause.** `mpc_accel_autonomous_controller.dc_active` is a
  process-local flag (`mpc_accel_autonomous_controller.py:1161`). It is set
  `True` by `_dc_start_sequence` at line 2193 and only cleared by
  `_dc_end_sequence` at line 2253. The Cancel Scan flow goes
  OCU → coordinator only — the coordinator wipes its own state but the
  controller is never told, so `dc_active` stays `True`. On the next mission's
  first waypoint with `Tag=1`, the gate
  `if dc_flag == 1 and not self.dc_active:` evaluates `False` → the entire
  start-DC branch is silently skipped. The same problem exists for
  `dc_pause_reasons` (e.g. `manual_override`, `heartbeat`).

- **Fix.** New `/dc/state_reset` topic (`std_msgs/String`):
  - `data_collection_coordinator.cancel_scan_callback` publishes
    `"cancelled"` on this topic at the end of a successful cancel.
  - `mpc_accel_autonomous_controller` subscribes; on receipt it acquires
    `_dc_sequence_lock` (so any in-flight `_dc_start_sequence` /
    `_dc_end_sequence` finishes first — worst case ~20 s while a
    `/dc/end_and_save` call is in flight) and resets:
    - `dc_active = False`
    - `_clear_dc_pause_reasons()`
    - `_dc_sequence_phase = "idle"`
    - waypoint nav state (`waypoint_navigation_active = False`,
      `waypoints = []`, `current_waypoint_index = 0`, `has_target = False`,
      `target_x/y` cleared, `previous_waypoint = None`)

  Topic was chosen as `String` (vs `Empty`) so future cancel-like events
  (e.g. `mission_finalized`, `pipeline_killed`) can reuse the same channel.

- **Status.** Fixed. Note: in initial field testing this fix appeared to
  not work — the symptom persisted after deploying the
  `/dc/state_reset` plumbing. Investigation found EC-002 below was
  hiding it: Cancel Scan was silently no-op'ing at the OCU, so
  `cancel_scan_callback` (and thus the new publish) never ran. Fixing
  EC-002 unblocks EC-001.

- **Where.**
  - `pilot_control/scripts/data_collection_coordinator.py` —
    `cancel_scan_callback` publishes `dc_state_reset_pub`.
  - `pilot_control/scripts/mpc_accel_autonomous_controller.py` —
    `_on_dc_state_reset` handler.

### EC-002 — Cancel / Discard / Complete Mission silently no-op when OCU runs on the operator laptop

- **Symptom.** Operator presses Cancel Scan, Discard Scan, or Complete
  Mission. The OCU UI returns to Map Processing (cancel) or Dashboard
  (complete) as if it succeeded, the status pill flips, the segment list
  clears. But on the robot the coordinator never logs `CANCELLING SCAN` /
  `FINALIZING MISSION`, no `/dc/state_reset` fires, the in-flight section
  folder + mission folder + UBX log all remain on disk, and (critically)
  the controller's `dc_active` flag is never reset → next scan exhibits
  EC-001 even with the EC-001 fix in place.

- **Root cause.** Robot-side zenoh router
  (`pilot_control/config/zenoh/zenohd_robot.json5`) uses an explicit
  `ros2dds.allow.service_servers` allowlist. Only `^/dc/pause$` and
  `^/dc/resume$` were ever added. `/dc/cancel_scan` (added with the
  Cancel/Discard feature) and `/dc/finalize_mission` (added with the
  continuous-GNSS Complete Mission rework) were missing. Result: the
  laptop OCU's service-discovery query is dropped at the router → the
  service never appears on the laptop → `wait_for_service(250 ms)` times
  out → the OCU follows its "service unreachable" fallback path which
  resets the UI without surfacing an error to the operator.

  The OCU log makes it obvious in hindsight (`grep '/dc/cancel_scan' on
  the bdr_coverage_planner stderr` → many lines of
  `service not available — proceeding with UI reset only`), but the UI
  doesn't display this so the operator has no signal that anything went
  wrong.

- **Fix.** Add to `service_servers` in `zenohd_robot.json5`:

  ```json5
  "^/dc/cancel_scan$",
  "^/dc/finalize_mission$",
  ```

  Then on the robot: restart the zenoh router, then restart the launch.
  Verify from the **laptop** with
  `ros2 service list | grep -E 'dc/(cancel_scan|finalize_mission)'` —
  both must appear before the OCU will use them.

  > **Maintenance note.** Every new robot-side service the OCU calls
  > MUST be added to this allowlist or it will silently behave like
  > EC-002. There is no diagnostic in the UI today; any future Cancel-
  > like or Finalize-like service should also publish a follow-up event
  > the OCU can subscribe to so a missed call is loud, not silent.

- **Status.** Fixed in `zenohd_robot.json5`.

- **Where.**
  - `pilot_control/config/zenoh/zenohd_robot.json5` — `service_servers`
    allowlist.
  - `cpp/src/app_shell.cpp` — `cancelActiveScanDataCollection`,
    `finalizeMissionDataCollection` (callers, unchanged; the fallback
    branches that masked the bug live here at the
    `wait_for_service` 250 ms timeout).

### EC-003 — _placeholder for next edge case_

---

## 4. State / Process Map (reference)

| Owner | Lives in | Cares about Cancel? | How it learns |
|---|---|---|---|
| OCU (`bdr_coverage_planner`) | C++ process on operator laptop | Yes — initiates | Operator click |
| `data_collection_coordinator` | Python ROS2 node | Yes — handles `/dc/cancel_scan` | Service call |
| `mpc_accel_autonomous_controller` | Python ROS2 node | Yes — `dc_active` flag | `/dc/state_reset` topic (EC-001) |
| `unified_data_collector` / `gpr_scan_controller` / `rosbag2` / `gps_driver` | Python ROS2 nodes | Stopped via `/video_record_set`, `/gpr_scan/stop`, `/rosbag/stop`, `stop_gps_raw_log()` from coordinator | Coordinator orchestrates |

---

## 5. ROS Service / Topic Cheatsheet (DC-related)

| Name | Type | Direction | Purpose |
|---|---|---|---|
| `/dc/start` | `Trigger` | OCU/controller → coordinator | Begin a new section's DC |
| `/dc/pause` / `/dc/resume` | `Trigger` | OCU/controller → coordinator | Soft pause/resume video + GPR + rosbag |
| `/dc/end_and_save` | `SetBool` | controller → coordinator | Close & rename section folder |
| `/dc/end_and_delete` | `Trigger` | OCU → coordinator | End current section + delete its folder |
| `/dc/finalize_mission` | `Trigger` | OCU → coordinator | Stop continuous mission GNSS + write mission_config.json |
| `/dc/cancel_scan` | `Trigger` | OCU → coordinator | Stop everything, delete every section + mission folder |
| `/dc/state_reset` | `String` | coordinator → controller | Tell controller to drop stale `dc_active` etc. (EC-001) |
| `/dc/start_gnss_precapture` | `Trigger` | controller → coordinator | Pre-arm GNSS raw log before first /dc/start |
| `/scan_segment_status` | `String` | controller → OCU | Publishes `segment_complete` / `segment_saved` |
| `/f2c_waypoints` | `Float64MultiArray` | OCU → controller | Per-segment waypoint list (xy + DC tag) |
| `/mpc_autonomy_enable` | `Bool` | OCU → controller | Master autonomy gate |
