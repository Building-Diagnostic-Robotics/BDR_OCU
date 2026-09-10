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

## Measured mode

Same screen, same polygon tooling, no imagery. The canvas is an adaptive
metric grid anchored at a fixed reference origin with 0.1 m snapping, for
sites where the operator has real measurements and satellite imagery would
only add error. Address search, tile download and imagery provenance are
hidden because they mean nothing there.

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

- Mission end relies on the robot's 10-minute auto-finalize watchdog; the OCU
  does not call `/dc/finalize_mission` from this stage yet.
- Wayback release pinning is exposed but only useful in the office, where
  there is bandwidth to compare releases.
