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

## The step model

The screen is a five-step flow. The step header names the steps, the left
rail shows only the active step's controls, and a footer button advances to
the next one.

| # | Step | Complete when | Unavailable when |
|---|------|---------------|------------------|
| 1 | Satellite Map | a job is named and the canvas has been aimed at the building | — |
| 2 | 3D Alignment | measured: a map has been collected · satellite: the fit is confirmed | planning-only trim |
| 3 | ROI Definition | the operator confirms the ROI against the aligned map | — |
| 4 | Edge Review | the operator acknowledges having reviewed the edges | — |
| 5 | Autonomous Scan | the mission is finalized | planning-only trim |

Each step carries two separate predicates: whether it is **available** in
this trim, and whether its own work is **complete**. Keeping them apart is
what lets the office trim run 1 → 3 → 4 without the robot-dependent steps
blocking anything. The footer advances to the next available incomplete step;
completed steps stay clickable, because re-aligning or redrawing after seeing
the result is normal rather than exceptional.

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
- **Imagery is advisory, not a gate.** Requiring cached tiles to leave step 1
  would force a tile download on someone merely drafting, and would hard-block
  a satellite plan started in the field, where there is no internet to cache
  from. Heading to the field uncached earns a warning instead.
- **Measured mode reduces steps, it never skips them.** Step 1 keeps the job
  name and drops the imagery tools; step 2 keeps Collect Map and drops the
  correspondence picker. Step numbering means the same thing in both modes.

Teleop lives only in step 5, reached by clicking the FPV view as in the
Stage 5 scan surface. The log is a collapsible canvas overlay on every step,
because SSH and launch failures surface during Collect Map and Send, not
only during the scan.

## Office: plan the job

1. Dashboard **Plan Job** opens `SatelliteScreen` in planning-only trim
   (mission and teleop cards hidden, Send unavailable).
2. Type a building name and address, hit **Go** to geocode, and pan to the
   roof.
3. **Download Area…** caches the tile pyramid for a radius around the view,
   stitches a max-zoom `site.jpg`, and writes `imagery.json`. All three live
   under the job's assets folder, so the imagery is part of the plan rather
   than part of the laptop.

   Zoom selection picks **the highest native zoom whose source imagery is
   within the configured age** (default 3 years). It never falls back to
   older-but-sharper tiles: planning a roof against a decade-old flight is a
   real failure mode, and unknown provenance is treated as too old rather
   than fresh.

4. **Add ROI** drops a rectangle, or **Draw Shape** takes a click per roof
   corner and closes on right-click. Drag a vertex to move it; click an edge's
   dimension chip to type an exact length; tap an edge to mark it as a **roof
   edge** (it turns solid red).
5. **Save Plan.**

Roof edges are the ones with a fall hazard on the far side. The robot applies
`roof_edge_clearance` (0.5 m default) as a planning setback on those edges
only — an unmarked interior edge gets the normal treatment. Physical keep-out
still wins; the setback is not a safety system.

## Field: anchor the plan to the robot

1. Start New Scan, pick the saved plan, fill in the metadata modal.
2. **Find Robot** recentres the canvas on the best position known so far.
3. **Collect Map from Robot** arms the motors, spins 360° in place, then
   drives a short forward/back leg. That produces three things: a point
   cloud, a final pose, and a GPS fix with a heading (when the baseline was
   long enough to resolve one). The OCU pulls the cloud and pose back, re-
   origins the cloud on the final pose, and renders it top-down.
4. **Pick Correspondences** puts the site image and the point-cloud raster
   side by side. Click a feature on the satellite image, then the same
   feature on the map, and repeat. Picking alternates strictly so a pair can
   never half-form on one side.

   You need **3 pairs if the collection got a GPS fix, 5 if it did not**. The
   fix independently pins position and usually heading, which leaves the fit
   only needing scale; without it the fit has to find everything from the
   picks and wants the redundancy. Spread the picks around the site — four
   points clustered in one corner solve badly regardless of count.

5. **Align** solves a 2D similarity (scale, rotation, optional reflection,
   translation) and shows the robot's origin drawn on the satellite image.
   Check the marker sits where the robot actually stood. If it does not,
   **Reselect correspondences**.
6. **Confirm alignment** commits the fit. The robot's origin becomes a
   surveyed lat/lon anchor, and every ROI vertex exported at Send inherits
   that accuracy.

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

- Wayback release pinning is exposed but only useful in the office, where
  there is bandwidth to compare releases.
- The robot must be running the `cliff-on-autonomy` build for the roof-edge
  setback and the `/coverage/status` state pill to be live.
