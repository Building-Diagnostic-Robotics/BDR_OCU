<<<<<<< HEAD
# BDR_OCU
=======
# BDR Coverage Planning Suite (V2 workspace)

This folder is the **new project home** for the BDR Coverage Planning software.

## Structure

- `cpp/`: C++ Qt/ROS2 desktop application (transferred from the previous development workspace)
  - Contains the **new staged flow** (Stage 1 login → Stage 2 startup → …) and the existing planner code.
- `App resource/`: design/exported SVG assets used during UI development.

## Build & run

From the repo root:

```bash
cd cpp
./build.sh
./build/bdr_coverage_planner
```

## Current stage flow

- **Stage 1**: Setup/Login (Robot ID + Access Code + view/hide + gated arrow)
- **Stage 2**: Startup/Calibration (placeholder behavior; to be implemented next)
- **Stage 3**: Placeholder (until the 3rd screen is designed/implemented)

>>>>>>> bee8d2f (Initial import of the BDR Operator Controller Unit(Upgrade from legacy planner))
