# Fields2Cover GUI - Quick Start Guide

## Prerequisites

Make sure you have the required dependencies installed:

```bash
# Python packages
pip install PySide6 numpy open3d shapely alphashape fields2cover matplotlib
```

## Running the GUI

### Method 1: Direct Python Execution

```bash
cd /home/avenblake/pilot_ws/src/pilot_control/scripts/F2C
python3 f2c_gui.py
```

### Method 2: Make Executable and Run

```bash
cd /home/avenblake/pilot_ws/src/pilot_control/scripts/F2C
chmod +x f2c_gui.py
./f2c_gui.py
```

### Method 3: Run from Anywhere

```bash
python3 /home/avenblake/pilot_ws/src/pilot_control/scripts/F2C/f2c_gui.py
```

### Method 4: As a Module (if installed)

```bash
python3 -m pilot_control.scripts.F2C.f2c_gui
```

## Quick Usage Workflow

1. **Launch GUI**: Run one of the commands above

2. **Load Point Cloud**:
   - Click "Load PCD / PLY / XYZ"
   - Select your point cloud file (.pcd, .ply, or .xyz)

3. **Preprocess** (optional):
   - Adjust "Z-band ±" and click "Apply Height Crop & View 3D"
   - Optionally downsample if point cloud is too large
   - Click "Project to 2D & Compute Hull" to create boundary
   - Optionally simplify polygon

4. **Configure Coverage**:
   - Set swath width, headland width, turn radius
   - Enable "Auto-align to building" if desired
   - Select route pattern (Boustrophedon, Snake, Spiral)
   - Select path planner (Dubins, Reeds-Shepp, etc.)
   - Enable decomposition if field is concave

5. **Generate Coverage**:
   - Click "Generate Swaths" to see coverage lines
   - Click "Generate Route" to see ordered path
   - Click "Generate Path" to see smooth Dubins path

6. **Export**:
   - Click "Export Path CSV" to save waypoints
   - Click "Export Swath Corners CSV" to save swath endpoints

## Troubleshooting

### GUI Won't Start

**Error: "No module named 'PySide6'"**
```bash
pip install PySide6
```

**Error: "No module named 'fields2cover'"**
```bash
pip install fields2cover
```

**Error: "No module named 'open3d'"**
```bash
pip install open3d
```

### GUI Starts But Shows Errors

**Check Python Version:**
```bash
python3 --version  # Should be 3.8+
```

**Check Dependencies:**
```bash
python3 -c "import PySide6; import fields2cover; import open3d; import shapely; print('All dependencies OK')"
```

### Display Issues

If GUI doesn't appear or has display issues:
```bash
# Set display (if using X11 forwarding)
export DISPLAY=:0

# Or try with Qt platform plugin
QT_QPA_PLATFORM=xcb python3 f2c_gui.py
```

## Example Session

```bash
# 1. Navigate to directory
cd /home/avenblake/pilot_ws/src/pilot_control/scripts/F2C

# 2. Run GUI
python3 f2c_gui.py

# 3. In GUI:
#    - Load: PCD/tilt_corrected_pcd.section.pcd (example file in PCD folder)
#    - Apply height crop
#    - Compute hull
#    - Generate swaths
#    - Export results
```

## Command Line Alternative

If you prefer command-line interface, use:
```bash
python3 pointcloud_to_f2c_field.py input.pcd --swath-width 1.0 --plot
```

## GUI Features

- **Interactive 2D Visualization**: Matplotlib-based plot with zoom/pan
- **ROI Selection**: Draw polygons to restrict coverage area
- **Obstacle Marking**: Mark no-go zones
- **Real-time Preview**: See changes as you adjust parameters
- **Export Options**: Save paths and swath corners to CSV

## Tips

1. **Start Simple**: Load a small point cloud first to test
2. **Adjust Alpha**: For concave hull, try alpha values 0.5-2.0
3. **Use Decomposition**: Enable for concave fields or fields with obstacles
4. **Auto-align**: Helps align swaths to building orientation
5. **Export Early**: Save intermediate results as you work

## Getting Help

- Check `OPTIMIZATION_ANALYSIS.md` for technical details
- Check `DECOMPOSITION_GUIDE.md` for decomposition usage
- Check `CHANGELOG_OPTIMIZATIONS.md` for recent changes

