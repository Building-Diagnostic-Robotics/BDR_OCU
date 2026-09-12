# Stage 6 satellite workflow

How an operator gets from "we have a roof to scan" to autonomy running on
that roof, and what the OCU is actually doing at each step. Companion to the
Stage 6 section of `AGENTS.md`, which carries the rules for changing this
code; this file is the narrative.

## The problem this solves

The robot plans coverage in `robot_init` metres — a frame whose origin is
wherever it happened to be when Fast-LIO started. The operator plans coverage
by looking at a roof. Stage 6 is the bridge: it lets the operator draw a
region on real satellite imagery and hands the robot that region in its own
frame, accurately enough that a 0.5 m roof-edge setback means something.

Two constraints shape everything:

- **There is no internet on the roof.** Imagery has to be downloaded in the
  office and travel with the job.
- **GPS is not accurate enough on its own.** A consumer fix is metres off and
  a heading needs a baseline the robot may not get. GPS is used as a seed;
  operator-picked correspondences are the accuracy step.

## Two trims, one screen

The screen has two very different jobs and dresses differently for each:

- **Office (planning-only trim).** Reached from Dashboard **Plan Job**.
  There is internet, there is no robot. One task: draw the roof, place the
  robot, save. No step header, no footer, no Find Robot, no Collect Map. Save
  Plan is the imagery step — see below.
- **Field (scan trim).** Reached from **Start New Scan**. There is a robot,
  there is no internet. The five-step flow below runs here. Saving in the
  field re-writes geometry only; it never tries to fetch.

Satellite planning starts in the office by construction: the **New Satellite
Plan** card in `ScanSetupDialog` is disabled until an imagery-reachability
probe (`TileService::connectivityProbeUrl`, every 5 s while the modal is
open, two consecutive answers to enable, one failure to disable) says tiles
can actually be fetched. On a roof it stays grey with "Needs an internet
connection". The Measured card is never gated: a tape works anywhere.

## The step model (field)

In the scan trim the screen is a five-step flow. The step header names the
steps, the left rail shows only the active step's controls, and a footer
button advances to the next one.

| # | Step | Complete when | Unavailable when |
|---|------|---------------|------------------|
| 1 | Satellite Map | a job is named and the canvas has been aimed at the building | — |
| 2 | 3D Alignment | measured: a map has been collected · satellite: the fit is confirmed | planning-only trim |
| 3 | ROI Definition | the operator confirms the ROI against the aligned map | — |
| 4 | Edge Review | the operator acknowledges having reviewed the edges | — |
| 5 | Autonomous Scan | the mission is finalized | planning-only trim |

Each step carries two separate predicates: whether it is **available** in
this trim, and whether its own work is **complete**. Keeping them apart is
what lets the same gating code run in the office (where steps 2 and 5 do not
exist) without the robot-dependent steps blocking anything. The footer
advances to the next available incomplete step; completed steps stay
clickable, because re-aligning or redrawing after seeing the result is normal
rather than exceptional.

**The step is derived, never stored.** `computeStep()` reads the same
expressions the buttons already gate on — a saved job id, `pcd_to_sat_.valid`,
`map_->polygon().valid()` and so on. A parallel step variable could drift out
of sync with those gates and show Send as reachable while the arming gate
still refuses, so there isn't one.

Some specifics that are easy to get wrong:

- **ROI comes after alignment, not before.** The operator draws against
  imagery already fitted to the robot's map, instead of drawing first and
  discovering the fit moved everything. The office still drafts a polygon;
  step 3 in the field is a confirm-and-adjust pass over that draft. It needs
  its own acknowledgement precisely because `polygon().valid()` is already
  true the moment a saved plan loads, so without one the step would arrive
  pre-completed and the operator could walk past the check that motivated
  the reordering.
- **Edge Review gates on the acknowledgement, not on a nonzero count.** A
  roof with no fall hazards is a legitimate answer, and gating on "at least
  one edge marked" would pressure the operator into marking something
  spurious to advance. The point is that the question got asked. The
  acknowledgement is per-session and is not persisted with the plan: parapets
  and skylights are exactly what imagery gets wrong, so an office review does
  not stand in for looking at the real roof.
- **Imagery is advisory in the field, not a gate.** The office save already
  cached it; if the operator chose "Save without imagery" the plan loads with
  `imagery_cache.cached = false` and the canvas shows whatever the shared
  cache has. Requiring tiles here would hard-block a plan on a roof with no
  way to fetch them.
- **Measured mode reduces steps, it never skips them.** Step 1 keeps the job
  name and drops the imagery tools; step 2 keeps Collect Map and drops the
  correspondence picker. Step numbering means the same thing in both modes.

Teleop lives only in step 5, reached by clicking the FPV view as in the
Stage 5 scan surface. The log is a collapsible canvas overlay on every step,
because SSH and launch failures surface during Collect Map and Send, not
only during the scan.

## Office: plan the job

The goal is the fewest operator steps that still produce a field-ready plan.
The ROI drawn here is a draft — the operator will very likely adjust it on
the roof — so the save's real product is the cached imagery, not the polygon.

1. Dashboard **Plan Job** opens `SatelliteScreen` in planning-only trim:
   mission and teleop cards hidden, no step header or footer, no Find Robot.
2. Type a building name (and optionally an address), hit **Go** to geocode,
   and pan to the roof.
3. Pick **Rectangle** or **Polygon**, press **Draw ROI**. Rectangle is a
   press-drag-release across the roof; Polygon takes a click per corner and
   closes on right-click (this is the Stage 5 drawing model). Either way the
   result is a `RoiPolygon`. Drag a vertex to move it; click an edge's
   dimension chip to type an exact length; tap an edge to mark it as a **roof
   edge** (it turns solid red). **Redraw ROI** starts over; **Clear** removes
   it.
4. **Place Robot**, then click where the robot will sit. The marker is
   selected on placement: a click on it toggles a rotate handle at the arrow
   tip, a drag on its body translates it, a click anywhere else deselects.
   Unselected, the arrow is heading information only — it cannot be spun by
   a careless drag.
5. **Save Plan** opens the **Confirm Plan** dialog: canvas thumbnail framed
   on the ROI, every edge length (pinned / roof-edge flags shown), the robot
   pose, and the imagery about to be cached. **Download & Save** runs the
   prefetch in place and saves the plan when it completes.

   Every download parameter is derived, not typed: centre = ROI centroid,
   radius = ROI radius + 60 m (floor 150 m), max zoom 19, World Imagery,
   live mosaic, imagery no older than 3 years. The knobs still exist — cache
   radius, max zoom, age window, Esri Clarity, Wayback release — behind a
   collapsed **Advanced imagery options** disclosure. They are defaulted, not
   removed.

   Zoom selection picks **the highest native zoom whose source imagery is
   within the age window**. It never falls back to older-but-sharper tiles:
   planning a roof against a decade-old flight is a real failure mode, and
   unknown provenance is treated as too old rather than fresh.

   The prefetch caches the tile pyramid, stitches a max-zoom `site.jpg`, and
   writes `imagery.json` under the job's assets folder, so the imagery is
   part of the plan rather than part of the laptop. On failure the dialog
   offers **Retry Download** (tiles already cached are kept) or **Save
   without imagery**, which saves the plan with `imagery_cache.cached =
   false` — visible, and not field-ready until re-saved with a connection.
   **Cancel** saves nothing; a never-saved plan's half-filled assets folder is
   removed.

The canvas carries a tool stack ported from the Stage 5 plot: **Zoom in**,
**Zoom out**, **Fit to ROI**, and a two-click **Measure** ruler (Esc or
right-click clears). Save Plan fits the view to the ROI before taking the
thumbnail.

Roof edges are the ones with a fall hazard on the far side. The robot applies
`roof_edge_clearance` (0.5 m default) as a planning setback on those edges
only — an unmarked interior edge gets the normal treatment. Physical keep-out
still wins; the setback is not a safety system.

## Field: anchor the plan to the robot

1. Start New Scan, pick the saved plan, fill in the metadata modal.
2. **Find Robot** recentres the canvas on the best position known so far.
3. Step 2, **3D Alignment**, is a full-width two-pane picker with no side
   rail: **SATELLITE MAP** (the cached site image) on the left, **3D POINT
   CLOUD** on the right. Until a cloud exists the right pane is an empty
   state with one button, **Capture Point Cloud**. It arms the motors, spins
   360° in place, then drives a short forward/back leg. That produces three
   things: a point cloud, a final pose, and a GPS fix with a heading (when
   the baseline was long enough to resolve one). The OCU pulls the cloud and
   pose back, re-origins the cloud on the final pose, and renders it top-
   down into the right pane. Capture progress replaces the pane title and
   the button becomes **Cancel Capture** while it runs.
4. Pick correspondences straight in the two panes. Click a feature on the
   satellite image, then the same feature on the point cloud, and repeat.
   Picking alternates strictly so a pair can never half-form on one side.
   The instruction bar above the panes keeps the tally (`Satellite (n)`,
   `Point Cloud (n)`, `n/N pairs` — green once enough). The footer holds
   **Clear pairs** beside Back and **Align (N pairs)** beside Next. Ctrl+Z
   undoes the last pick.

   You need **3 pairs if the collection got a GPS fix, 5 if it did not**. The
   fix independently pins position and usually heading, which leaves the fit
   only needing scale; without it the fit has to find everything from the
   picks and wants the redundancy. Spread the picks around the site — four
   points clustered in one corner solve badly regardless of count.

5. **Align** solves a 2D similarity (scale, rotation, optional reflection,
   translation) and commits it in the same click: an **Alignment
   Successful / RMSE** card covers the point cloud, the robot's origin is
   drawn on the satellite image, and Next enables. The origin becomes a
   surveyed lat/lon anchor saved with the plan, and every ROI vertex
   exported at Send inherits that accuracy. Check the marker sits where the
   robot actually stood; if it does not, **Clear pairs** and pick again.
6. Leaving the screen (top-bar Back) asks for confirmation and then clears
   the collected map, picks and fit — a saved anchor stays with the plan,
   but a cloud is never reused across visits. Loading a different plan
   clears them too.

### Plans created in the field

A satellite plan saved on site has no cached imagery, and 3D Alignment needs
the site image. Save Plan checks for a connection: if the laptop is online
(a phone hotspot is enough) it runs the same download-and-save dialog as the
office; if not, it saves the geometry and warns that the plan must be
re-saved once with a connection before step 2 can run.

## Send

Send converts the ROI polygon from geographic coordinates into `robot_init`
metres relative to the marker, and SSH-launches
`robot_autonomous_coverage_director.launch.py` with `roi_vertices` and, when
any edge is marked, `roi_edge_flags`.

Vertices and flags travel in **raw corner order**. The robot resolves flags to
segments before `Polygon.buffer(0)`, which may reorder rings — resolving after
would silently attach the setback to the wrong edge.

Autonomy stays hard-blocked until `/data_collection_coordinator/set_parameters`
accepts building, operator and units. That push is the arming gate; a failed
push must not let a scan start, because the data would land in an unlabelled
folder.

## Completing the mission

**Complete Mission** disables autonomy, waits for both axes to actually
report IDLE (up to 6 s — killing the launch tree does not by itself disarm
them), calls `/dc/finalize_mission` so the robot stops continuous GNSS and
stamps `mission_finalized_at`, then tears down both launch trees.

If the link is genuinely offline, you get the offline finalize dialog
instead: wait for reconnect, finalize over SSH, or cancel. SSH-finalize runs
`finalize_mission_local.py` on the robot, which rescans the mission folder
and writes the finalize metadata without needing ROS at all. Cancelling is a
legitimate choice — the robot auto-finalizes after 10 minutes idle.

Merely reconnecting does not pop that dialog. A few seconds of Zenoh
rediscovery is not a reason to make the operator choose a recovery path.

### What happens to the plan

A plan is **PLANNED** until a mission on it finalizes with data on disk.
Launching does not count — an aborted or cancelled mission leaves the plan
where it was. When `/dc/finalize_mission` returns ok (or the SSH fallback
exits 0) the plan is stamped `last_executed_at` and becomes **COMPLETED**:

- Scan Setup lists it under a collapsed **COMPLETED (N)** disclosure below
  SAVED PLANS, newest scan first, with a `LAST RUN <date>` chip. Tapping it
  opens the plan again — the cached imagery is still there.
- The office rail's Saved plan combo lists completed plans after a
  separator.
- Only the **5 most recent** completed plans are kept
  (`JobStore::kCompletedPlansKept`). Older ones are removed — JSON and the
  cached imagery — the moment a newer plan completes. PLANNED plans are
  never pruned.
- Pressing **Save Plan** on a completed plan returns it to PLANNED. A save
  is a statement that this roof is to be scanned again.

Every row in Scan Setup has a trash button. It is the only manual delete
path; it confirms first, then removes the plan and its imagery cache.

## Measured mode

Same screen, same polygon tooling, no imagery. The canvas is an adaptive
metric grid anchored at a fixed reference origin with 0.1 m snapping, for
sites where the operator has real measurements and satellite imagery would
only add error. Address search, tile download and imagery provenance are
hidden because they mean nothing there.

Anchoring works differently and more simply: **Collect Map from Robot** still
runs, but there is no correspondence step. The measured grid origin *is*
`robot_init`, so the collected map already sits in the canvas frame — it is
drawn as a backdrop under the grid, the robot marker is placed at the origin,
and the operator draws the ROI around the cloud. The GPS fix from collection
is recorded with the plan for reference and for **Find Robot**.

## Where things live

```
<AppData>/satellite_jobs/
  <job_id>.json            plan: mode, polygon + roof edges, gps seed,
                           imagery_cache, alignment fit, timestamps
  <job_id>/
    tiles/                 offline pyramid (Esri World Imagery or Clarity)
    site.jpg               stitched max-zoom site image
    imagery.json           SiteManifest: zoom, capture date, resolution,
                           layer, wayback release, stitch bounds
    robot_map/             collected PCD, final pose, re-origined PCD
```

## Known gaps

- Wayback release pinning lives in the Confirm Plan dialog's Advanced
  section — office-only by construction, which is where there is bandwidth to
  compare releases.
- The robot must be running the `cliff-on-autonomy` build for the roof-edge
  setback and the `/coverage/status` state pill to be live.
