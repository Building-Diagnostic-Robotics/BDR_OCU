# AUTONOMY_CONOPS.md — Roofus Autonomous Scan-While-Exploring

**Status: design intent / north-star. NOT built yet.** This document
captures the target concept of operations (CONOPS) for autonomous roof
coverage so agents and humans share one mental model when touching
exploration, edge/obstacle detection, coverage planning, or the ROI flow.
Nothing here is wired into the live stack today — it is the goal the
near-term work builds toward. Treat it as the "why" behind those changes,
not a description of current behavior.

When you implement a piece of this, link the new code back to this doc and
update the **Roadmap / status** section so it stays honest.

---

## 1. The goal in one paragraph

The operator deploys Roofus on a roof, gives it (implicitly or explicitly)
a region to scan, and presses start. Roofus then **explores to build the
map AND performs data collection (GPR / visual / thermal) simultaneously in
a single pass**, while autonomously avoiding obstacles and — critically —
the roof edge (open drop-offs, parapets, and any perimeter). There is no
separate "map first, plan offline, then execute" pass for this mode; the
robot discovers and covers at the same time. The pre-existing offline
Fields2Cover (F2C) coverage planner and the CSF ground model remain the
tooling for the *manual* exploration workflow, not this autonomous one.

---

## 2. Two deployment modes

Both modes share the **same** live substrate (walkable/keep-out map +
on-robot coverage executor). They differ **only** in how the coverage
region is defined.

### Mode A — Corner start, whole-roof discovery
- Operator places Roofus **parallel to a wall at one corner** of the roof
  and starts the scan.
- The coverage region is **discovered online**, bounded by the physical
  roof edges as they are found.
- The corner + wall is a *seed*: gives a boundary to wall-follow to
  bootstrap the first edge, and a heading datum so lanes start straight.
- Harder: requires online boundary discovery. This is the later milestone.

### Mode B — Center start, operator-drawn virtual ROI
- After power-on, the onboard **GPS** gives an approximate building
  location. The operator zooms the app's GPS/satellite map, places the
  robot marker where they physically set the robot, and keeps the robot at
  that orientation.
- The operator draws a region (e.g. **~20×20 m**). The ROI is **aligned to
  the robot's orientation** (axis-aligned to the robot's start heading), not
  to the building footprint in the imagery.
- The robot covers that virtual ROI.
- Easier to ship first: the planner has a boundary from t=0 (close to the
  existing F2C boustrophedon), so it exercises detection → trim → turn
  **without** also solving online boundary discovery. **Mode B is milestone
  one, not Mode A.**

---

## 3. Hardware / software context this rides on

(See `AGENTS.md` "Disconnect resilience" and the explore findings for
detail; summarized here so this doc stands alone.)

- **LiDAR:** tilt-mounted (~15° forward pitch) **Livox MID360** (solid-state
  3D, non-repetitive scan pattern). Raw `livox_ros_driver2/CustomMsg` on
  `/livox/lidar`.
- **SLAM:** **Fast-LIO2** (LiDAR + MID360 IMU) → `/Odometry`, registered
  cloud on `/cloud_registered` (map frame), body cloud on
  `/cloud_registered_body`.
- **Ground-aligned frame:** `odom_tilt_corrector` →
  `/Odometry_tilt_corrected_diff`, the level robot frame where the roof
  floor sits at **z ≈ 0**. All autonomy consumes this frame.
- **Existing live nav:** `local_nav_grid_publisher` bins `/cloud_registered`
  into a Z-slice `[-0.20, +0.10] m` occupancy grid — **visualization only**
  for the Stage 4 mini-map. It does **not** stop the robot.
- **Existing safety:** the only live motion stop is the **OCU heartbeat
  gate** in `mpc_accel_autonomous_controller.py`
  (`_check_heartbeat_safety()` → `send_zero_velocity()`). **Nothing stops on
  LiDAR today.**
- **Controller:** the MPC is a **path tracker** — it follows given
  waypoints and publishes `odrive_can/ControlMessage` directly to
  `/{left,right}/control_message` (bypassing `/cmd_vel`). It does not plan.
- **Offline tooling (manual workflow only):** CSF ground segmentation +
  F2C coverage in `cpp/src/obstacle_detector.cpp` / the F2C planner.
- **GPS:** onboard, **consumer-grade** (meters, worse with multipath next to
  a building). RTK is a **future** upgrade, not present for v1.
- **Zenoh note:** raw LiDAR/map topics are **not** bridged to the OCU; only
  low-bandwidth derived topics cross the radio link. Edge/obstacle
  processing must run **robot-side**.

---

## 4. Core concept: the live "walkable map"

Everything keys off one primitive: **the connected region of confirmed roof
floor containing the robot.** Build a 2.5D grid in the tilt-corrected frame
(floor ≈ z=0) and classify each cell:

| State | Evidence | Treatment |
|-------|----------|-----------|
| **FLOOR** | enough returns with `z ∈ [−floor_tol, +step]` | traversable |
| **POSITIVE** | returns `z > +step` (parapet / curb / HVAC) | keep-out |
| **DROP** | returns `z < −drop` (street far below — hard cliff) | keep-out |
| **UNKNOWN** | too few returns | **NOT safe** — treat as potential edge |

- **"Edge" scope (operator's choice):** treat the *entire roof perimeter as
  keep-out regardless of whether it's a parapet or an open drop*. So we
  never need to distinguish wall-vs-cliff for safety — both just bound the
  walkable floor. Parapets are also caught as POSITIVE obstacles; open edges
  fall out as the floor simply ending.

### The one safety invariant
```
coverable = ROI ∩ confirmed-floor − keep-out(edges, obstacles) − margin
```
- The **ROI bounds** the robot from wandering (Mode B) / is discovered
  (Mode A).
- The **physical keep-out always wins.** A virtual ROI is a *maximum
  intended extent*, never permission to drive. If the operator draws the
  20×20 past the real parapet, the live edge map clips coverage smaller.

### CSF does NOT belong here
CSF is a **batch** filter (needs a near-complete dense cloud, too slow
per-scan). It only ever belonged to the *manual exploration → offline
coverage planning* path. The autonomous online classifier is its own thing:
- **Cheapest:** per-cell tilt-corrected **z-band** test (floor ≈ 0 makes
  FLOOR/POSITIVE/DROP nearly free on a flat roof).
- **Better on slopes:** **Patchwork++** (already vendored in
  `pilot_control` test launches) — real-time ground segmentation.

### Persist the grid in the MAP frame, not instantaneously
The MID360's 15° forward pitch leaves a near-field/side **blind zone**, so
the robot can rotate/back toward an edge it never imaged. Maintain a
**persistent map-frame** walkable grid that remembers confirmed-FLOOR cells
through occlusion. Rule: **only drive onto cells previously confirmed
FLOOR.** Let the robot "peek" (creep + scan) to extend confirmed floor
rather than commit at speed into UNKNOWN. Watch for Fast-LIO
relocalization jumps shifting the grid.

### Margin math
```
margin ≥ footprint/2 + v_max × (sensor + compute + brake latency) + Fast-LIO drift
```
Registered cloud is ~10 Hz; cap autonomy speed so braking distance stays
under the margin.

### Suggested starting parameters
`cell 0.10 m`, `floor_tol 0.08 m`, `step 0.06 m`, `drop 0.25 m`,
`margin 0.50 m`, lookahead = braking distance at the capped speed.
Offline and online detection must agree on `step`/`drop`/`margin` so the
planner never routes into a band the guard then rejects.

---

## 5. Coverage exploration, NOT frontier-maximization

- Data collection needs **systematic, complete** coverage → the natural
  pattern is online **boustrophedon** (back-and-forth lanes) over floor
  revealed as you go. The roof edge **bounds** the lanes; it is never the
  goal you steer toward.
- Contrast with true **frontier exploration** (steer at the known→unknown
  boundary to maximize newly-seen area): that is the *most edge-prone*
  strategy on a roof, because the frontier and the roof edge are often the
  same line. It is deliberately **deferred to the RTK phase**.
- **Frontier-vs-edge disambiguation** (needed only once true frontier mode
  lands): a frontier and an edge look identical in 2D (both are
  floor→unknown). Discriminate with 3D evidence — DROP/POSITIVE beyond ⇒
  edge (veto as goal); floor continues near z≈0 ⇒ real frontier;
  no-evidence (beam off into space) ⇒ potential edge, peek-before-commit.

---

## 6. Where planning lives, and "replanning"

Because Mode A/B both run a **single explore-while-scan pass with no prior
plan**, planning **cannot** be a pre-computed OCU path the MPC tracks. It
must be **on-robot and incremental.** Three replan models considered:

1. **On-robot incremental coverage planner (the target):** consumes the
   live walkable map, emits the next lane segment / waypoints to the MPC,
   trims lanes at edges, turns around early, extends into newly-confirmed
   floor. This is what frontier+RTK will build on directly, so it is the
   investment that carries forward.
2. **OCU global re-plan loop (fallback only):** robot stops, ships the
   obstacle/edge polygon up, OCU re-runs F2C, sends a new path. Reuses
   existing tooling but needs a radio round-trip and is throwaway once
   on-robot planning exists. Keep as an operator-reseed fallback, not the
   primary.
3. **Stop / back-off + operator decision (safety floor, ship first):** the
   live guard halts, backs off, flags the OCU. Minimal new autonomy. This is
   the non-negotiable baseline regardless of which planner lands.

**Decision:** safety floor (#3) first, then build the **on-robot planner**
(#1). Do **not** invest in the OCU global loop (#2) as the primary.

### How the guard stops the robot (integration pattern)
Mirror the existing heartbeat safety, do **not** fight the MPC over
`/cmd_vel`:
- New robot-side C++ node (mirror `local_nav_grid_publisher`), robot-side
  only (LiDAR not bridged). Subscribes `/cloud_registered` +
  `/Odometry_tilt_corrected_diff`. Publishes a keep-out/distance field
  (low-rate, bridgeable for OCU viz) and an **alarm** (state + min-distance
  + nearest-edge direction).
- MPC subscribes the alarm and extends its safety gate in `control_loop()`:
  alarm active → `send_zero_velocity()` + back-off, same authority as the
  heartbeat check. Reference hooks: `_check_heartbeat_safety()` /
  `send_zero_velocity()` in
  `pilot_ws/src/pilot_control/scripts/mpc_accel_autonomous_controller.py`.
- Also gate **teleop** `/cmd_vel` (in `diff_drive_controller` or by zeroing
  the forward component) so the operator can't drive off the edge manually.
- Reserve `request_axis_state → IDLE` (hard disarm) for E-stop, not the soft
  cliff hold.

---

## 7. ROI definition & framing (Mode B)

- **Source of truth = body-frame offsets.** The ROI is stored/executed as a
  rectangle in the robot's start-heading frame, which maps 1:1 onto
  `/Odometry_tilt_corrected_diff` (origin at start, heading 0). **GPS
  accuracy never touches control or safety** — it is only for operator
  placement and the map background.
- **Rendering = robot-relative, overlaid on the GPS/satellite map.** The
  rectangle is anchored to the robot icon at the GPS fix and shown over the
  satellite tile for context.
- **Heading for the overlay** (cosmetic only — wrong heading misleads the
  operator's view but never endangers the robot, since control is
  body-frame and physical detection clips):
  - **Drive-forward GPS course** — nudge the robot a couple meters straight
    at start; GPS course-over-ground (NMEA `RMC`/`VTG`) ≈ heading for a
    diff-drive going forward. Needs a few meters of motion, decent sky view,
    degrades with multipath; assumes forward = facing.
  - **Manual rotate-to-match** — operator rotates the rectangle to match
    building edges in the imagery. Simplest, no motion dependency.
  - Magnetometer = poor fallback (steel deck / HVAC). **Dual-antenna RTK
    heading = eventual clean answer.**
- **Latch the ROI to the local (Fast-LIO) frame at scan-start.** Do NOT
  re-anchor to live GPS — consumer GPS jitter/drift (meters) would make the
  ROI crawl. Use GPS only for initial placement + a static background tile.

---

## 8. RTK's role (future)

RTK is the enabler for the full autonomous / frontier phase, not v1:
- Anchors the walkable/keep-out map and lane grid to a **global datum** so
  boustrophedon lanes stay parallel and the map doesn't smear under Fast-LIO
  drift over a large roof.
- Dual-antenna RTK gives a **true heading** (fixes the cosmetic overlay
  problem and enables map-aligned ROIs / repeatable missions in global
  coordinates).
- Makes the persistent keep-out map trustworthy at roof scale, which is what
  makes true frontier exploration safe enough to attempt.

---

## 9. Roadmap / status

The honest gap to the full CONOPS is three new pieces; check them off here as
they land.

- [~] **(1) Live keep-out map** — robot-side edge + obstacle detection,
  CSF-free, persistent map-frame walkable grid. *(Phase-one focus.)*
  **F1 built** — `roof_edge_costmap` node (passive viz, no MPC authority).
  See §11.
- [ ] **(2) On-robot incremental coverage planner** — consumes (1), feeds
  lane waypoints to the MPC, trims/turns at edges.
- [ ] **(3) Autonomy-driven data-collection triggering** — the
  `data_collection_coordinator` exists but is operator-driven today; couple
  it to autonomous coverage progress.

**Suggested milestone order:**
1. **Safety floor** — live guard stops/back-off + MPC alarm gate + teleop
   gate. (Subset of piece 1.)
2. **Mode B, single ROI** — detection → trim → turn proven via wall-follow
   /  one edge-bounded boustrophedon lane with a safe turnaround on real
   geometry, then full multi-lane coverage over a known ROI rectangle.
3. **Mode A** — online boundary discovery (corner start, whole roof).
4. **RTK** — global framing + true heading for both modes.
5. **Frontier exploration** — the final autonomous mapping mode.

---

## 10. Hard rules for agents (carry over from the discussion)

- **Physical keep-out always overrides the virtual ROI.** Never let a drawn
  ROI authorize driving onto an unconfirmed/UNKNOWN/edge cell.
- **UNKNOWN ≠ safe.** Only drive onto previously confirmed FLOOR. Peek
  before committing at speed.
- **No CSF in the online path.** CSF stays offline/manual-workflow only.
- **Detection runs robot-side.** Raw LiDAR is not bridged over Zenoh.
- **Stop like the heartbeat does.** Gate the MPC via an alarm topic;
  don't fight `/cmd_vel`; reserve IDLE/disarm for E-stop.
- **Latch the ROI to the local frame; GPS is placement/display only.**
- **Offline and online detection must share `step`/`drop`/`margin`.**

---

## 11. F1 — Roof-edge costmap (as built)

Piece (1) of the roadmap, v1. Robot-side node `roof_edge_costmap`
(`pilot_ws/src/pilot_control/src/roof_edge_costmap_node.cpp`). **Passive**:
it produces a costmap, a height map, and a continuously replanned path for
RViz, and commands the robot in **no way**. MPC authority + the peek/turn
behavior layer are later phases.

### Pipeline

1. **Levelling (Option B, strict).** Reuses `local_nav_grid_publisher`'s
   transform: match a raw `/Odometry` sample to the
   `/Odometry_tilt_corrected_diff` sample of the same stamp to derive
   `r_init_` / `p0_lidar_`, then level every `/cloud_registered` point into
   the corrected frame (floor at z≈0). **No fixed-pitch fallback** — the NPZ
   tilt calibration is a hard arming gate (`validateCalibration` extracts
   `R_map`); if it's missing/unparseable the node disarms and emits nothing.
2. **Persistent dual-stat height map** (`PersistentHeightMap`), map-frame,
   grows as the robot explores (reallocate-with-copy, capped at
   `max_map_cells`). Per cell: Welford running mean/variance of point height
   (representative ground + confidence) + observed z extremes + two driven
   masks (full-footprint, wheel-track). **Terrain-relative overhead reject at
   integration:** a return more than `overhead_margin_m` (40 cm) above the
   column's established ground (its running min) is dropped before it touches
   any stat. The reference is per-column, so it rides curved / hammock /
   peaked roofs (unlike a fixed plane or a robot-z offset, which would clip
   legitimate up-slope ground ahead). This mirrors the old CSF path's
   `[min, max]` clearance band against the draped cloth, made incremental:
   soffits / pipes / ceilings above the robot never become obstacles, the
   floor *under* an overhang stays ground-confident (drive-under), and a real
   obstacle's lower band (floor → 40 cm) is still integrated. The loose
   absolute `z_max_clip_m` only trims wild flyers.
3. **Split confidence (two independent gates).** Variance gating is right for
   *floor* but wrong for *obstacles* (a tall locker/wall face has huge
   within-cell z-variance and would never pass it — that's the original
   "locker invisible" bug). So:
   - **Ground/FREE gate:** a cell is trustworthy **FLOOR** only once
     `count ≥ min_obs_count` (5) **and** `stderr(mean) ≤ max_stderr_m`
     (2 cm). Its Welford mean is the representative ground height and the
     `local_floor` reference. Relaxed from the original 8 / 1 cm so flat
     ground fills densely.
   - **Obstacle/LETHAL gate:** point-count + z-extreme evidence, *independent*
     of variance (below).
4. **Local floor reference.** Per cell, `local_floor` = the lowest
   ground-confident mean within `local_floor_radius_cells` (6 → 0.30 m). All
   rises/drops are measured against this, so an obstacle is judged against the
   surrounding walkable height, not against itself.
5. **Eager obstacle/drop detection (variance-independent, lethal).** For any
   cell with `count ≥ obstacle_min_points` (4) that has a `local_floor`:
   - **POSITIVE** if `zmax − local_floor > obstacle_height_m` (4 cm) **and**
     `mean − local_floor > obstacle_corroborate_rise_m` (2 cm) **and**
     `zmin − local_floor ≤ overhead_margin_m` (40 cm, **base check**). The mean
     corroboration rejects a single high flyer (one stray point can't lift the
     mean) — important at 4 cm, near the ~2.7 cm LiDAR noise floor. This is
     what now catches the locker/wall regardless of its z-variance. The
     `obstacle_height_m` floor pairs with the 40 cm overhead reject above to
     form the obstacle band (4 cm → 40 cm clearance over local ground).
     The **base check** (`zmin`) makes the per-column overhead reject robust
     to *occluded-floor* columns: the integration-time reject is relative to a
     column's own ground, so floating / overhead returns in columns whose
     floor was never observed (occluded beside the structure, or out of the
     15°-down FOV) survive with a high `zmin`. Requiring the obstacle's base
     to sit within 40 cm of the *neighbourhood* floor drops them — a
     ground-rooted wall has `zmin ≈ local_floor` and is unaffected.
   - **NEGATIVE / cliff** if `local_floor − zmin > big_drop_m` (20 cm) **and**
     `local_floor − mean > drop_corroborate_m` (5 cm). A roof edge reads as the
     visible far-below ground (street) beyond it → strongly negative.
   These eager flags are **never** driven-cleared (a >8 cm wall / >20 cm drop
   was never driven over).
6. **Subtle-step classification + driven prior (8-neighbour, ground-confident
   neighbours only).** For ground-confident cells not already flagged eager:
   - **POSITIVE** if it rises above a neighbour by `> step_up_m` (2 cm);
     **NEGATIVE** if it sits below by `> step_down_m` (4 cm). Catches curbs,
     drains, shallow depressions.
   - **Driven-traversability prior (asymmetric, escape hatch).** If the robot
     actually drove the cell: full **footprint** clears small **positive**
     steps; only the **wheel-track** clears small **negative** steps (a
     bridged hole under the belly stays lethal). Override only up to
     `driven_override_max_step_m` (4 cm).
7. **Cell resolution order:** lethal (eager ∪ subtle) → **LETHAL**; else
   ground-confident → **FREE**; else driven-footprint → **FREE**
   (known-traversable, fills the path corridor); else **UNKNOWN**.
8. **Outputs** (all in the corrected-odom frame):
   - `/roof_edge/costmap` — `nav_msgs/OccupancyGrid`, 3-value
     (FREE 0 / LETHAL 100 / UNKNOWN −1), robot/goal-windowed.
   - `/roof_edge/heightmap` — `sensor_msgs/PointCloud2` (x,y,z,intensity),
     decimated + rate-limited. Now shows structure too: floor cells render at
     `mean`, positive cells at `zmax`, drops at `zmin`.
   - `/roof_edge/planned_path` — `nav_msgs/Path`.
   - **Static TF** `lidar_world_frame` (`camera_init`) → corrected frame
     (`robot_init`), broadcast once levelling initialises (rotation
     `r_init_ᵀ`, translation `p0_lidar_`). Co-registers the raw
     `/cloud_registered` with the levelled costmap in RViz — **replaces the
     external identity `static_transform_publisher` hack** (don't run both).
9. **JPS replan.** The OCU-side `GridPlanner` JPS core is ported verbatim to
   `pilot_control::RoofEdgeGridPlanner`
   (`include/roof_edge_grid_planner.hpp` + `src/roof_edge_grid_planner.cpp`),
   polygon ingest dropped, `buildFromGrid()` added (direct occupancy →
   EDT-inflate → JPS → any-angle smooth). Every publish tick (`publish_rate_hz`,
   2 Hz) it builds from the live costmap window and replans robot →
   `/goal_pose`. `plan_through_unknown` (default **false**) treats UNKNOWN as
   hard obstacle (peek-before-commit); lethal cells are inflated by
   `inflation_radius_m` (0.35 m).

### Wiring

- Launched in `robot_complete.launch.py` (node `roof_edge_costmap`,
  alongside `local_nav_grid_publisher`); `calibration_file` =
  `tilt_calibration_file`.
- Build: `roof_edge_costmap` target in `pilot_control/CMakeLists.txt`.
- Zenoh (`zenohd_robot.json5`): publishers `^/roof_edge/(costmap|heightmap|
  planned_path)$`, subscriber `^/goal_pose$`. Heightmap is the heavy stream —
  decimated + 1 Hz; set `publish_heightmap:=false` if the Microhard link is
  tight.

### Try it (RViz)

1. Launch the robot tree (calibration must be present, else the node
   disarms — check the log).
2. On the laptop, RViz with **Fixed Frame = `camera_init`** (or `robot_init`).
   The node now broadcasts `camera_init → robot_init`, so the raw
   `/cloud_registered`, the costmap, and the height-map all co-register —
   **do not** also run an external `camera_init → robot_init`
   `static_transform_publisher` (two publishers for one edge fight). Add the
   `OccupancyGrid`, `PointCloud2`, and `Path` displays; drop a goal with the
   **2D Goal Pose** tool (publishes `/goal_pose`). Teleop and watch the
   costmap + replanned path.

### Key tuning knobs (ROS params)

Ground/FREE: `min_obs_count` (5), `max_stderr_m` (2 cm). Eager
obstacle/drop: `obstacle_min_points` (4), `obstacle_height_m` (4 cm),
`obstacle_corroborate_rise_m` (2 cm), `big_drop_m` (20 cm),
`drop_corroborate_m` (5 cm), `local_floor_radius_cells` (6),
`overhead_margin_m` (40 cm, terrain-relative overhead reject). Subtle steps:
`step_up_m`, `step_down_m`, `driven_override_max_step_m`. Planner/IO:
`inflation_radius_m`, `window_radius_m`, `max_plan_radius_m`,
`publish_rate_hz`, `heightmap_rate_hz`/`heightmap_decimate`,
`plan_through_unknown` (false), `max_range_m`,
`z_min_clip_m`/`z_max_clip_m` (2 m), `lidar_world_frame` (`camera_init`),
`track_width_m`/`wheel_width_m` (verify `track_width_m` against the physical
robot — defaulted to 0.38 m).

### Rules for agents touching F1

- **Keep the strict calibration gate.** Re-introducing a fixed-pitch
  fallback regresses to a guessed level plane and silently wrong cliffs.
- **Keep the split confidence model.** Do not gate obstacle detection on
  low variance — that re-hides tall structures (the locker bug). FREE uses
  the variance gate; LETHAL uses point-count + z-extremes vs `local_floor`.
- **Keep the eager flags non-driven-clearable.** Only the small
  neighbour-step classifications are cleared by the driven prior.
- **Keep the overhead reject terrain-relative** (per-column ground in
  `addPoint`), not a fixed plane or robot-z offset — the latter clips real
  up-slope ground on curved/peaked roofs. It must run at integration so
  overhead points never pollute the per-cell mean/variance.
- **Keep the `camera_init → robot_init` TF broadcast here** and don't also
  publish that edge externally — co-registration relies on a single owner.
- **Keep `step`/`drop`/`margin` shared with the offline path** (CONOPS hard
  rule) — don't fork the thresholds.
- **F1 is passive.** Do not wire its path/costmap into `/cmd_vel` or the MPC
  here; that's a later, separately-gated phase.
