#!/usr/bin/env python3

"""
pointcloud_to_f2c_field.py
Convert a 3D point cloud of a roof into a 2D polygon and wrap it
as a Fields2Cover Field object.

Pipeline:
1. Load point cloud (PLY/PCD/XYZ) using Open3D.
2. Optionally filter points by height (z-band around median).
3. Project to 2D (drop Z).
4. Compute a concave hull using alphashape.
5. Simplify polygon with Shapely.
6. Build a Fields2Cover Field from the polygon.
7. Generate a coverage path (Headlands -> Swaths -> Route -> Path).

Usage:
    python pointcloud_to_f2c_field.py input.ply

Optional args:
    --z-band 0.5           # +/- band around median Z to keep
    --alpha 0.8            # alpha parameter for concave hull
    --simplify 0.1         # simplify tolerance (same units as coords)
    --max-points 50000     # subsample if more points than this
    --plot                 # show debug plot of polygon
    --swath-width 1.0      # robot swath width (default: 0.3m)
    --turn-radius 0.2      # robot minimum turning radius (default: 0.2m)
    --headland-width 1.0   # width of headland (turning area) inside the field
    --auto-align           # automatically align swaths to the longest edge of the field
    --align-mode           # Align swaths parallel to long edge ('long') or perpendicular ('perp')
    --output-csv path.csv  # save generated path waypoints to CSV
    --corners-only         # save only the start/end points of swaths to CSV (implies --output-csv)

Requirements:
    pip install open3d shapely alphashape fields2cover matplotlib
"""

import argparse
import sys
from typing import Tuple, Optional, List
import numpy as np
import open3d as o3d
import alphashape
from shapely.geometry import Polygon, MultiPolygon
from shapely.ops import unary_union
import fields2cover as f2c
import math
import csv

def load_point_cloud(path: str) -> np.ndarray:
    """Load point cloud from file and return Nx3 numpy array (x, y, z)."""
    pcd = o3d.io.read_point_cloud(path)
    if pcd.is_empty():
        raise ValueError(f"Point cloud is empty: {path}")
    points = np.asarray(pcd.points)
    if points.shape[1] != 3:
        raise ValueError(f"Expected 3D points, got shape {points.shape}")
    return points

def filter_by_z_band(
    points: np.ndarray,
    z_band: Optional[float],
) -> np.ndarray:
    """Keep points within +/- z_band of median z. If z_band is None, return unchanged."""
    if z_band is None:
        return points
    z = points[:, 2]
    z_med = np.median(z)
    mask = np.abs(z - z_med) <= z_band
    filtered = points[mask]
    if filtered.shape[0] == 0:
        raise ValueError("Z-band filter removed all points. Try a larger --z-band.")
    return filtered

def subsample_points(
    points: np.ndarray,
    max_points: Optional[int],
    rng: Optional[np.random.Generator] = None,
) -> np.ndarray:
    """Randomly subsample points if more than max_points."""
    if max_points is None or points.shape[0] <= max_points:
        return points
    if rng is None:
        rng = np.random.default_rng()
    idx = rng.choice(points.shape[0], size=max_points, replace=False)
    return points[idx]

def compute_concave_hull(
    xy: np.ndarray,
    alpha: float,
) -> Polygon:
    """
    Compute a concave hull (alpha shape) from 2D points.
    Returns a Shapely Polygon (largest polygon if multipolygon).
    """
    if xy.shape[0] < 3:
        raise ValueError("Need at least 3 points to compute a polygon.")

    # alphashape can take iterable of (x, y)
    alpha_geom = alphashape.alphashape(xy, alpha)

    # Handle MultiPolygon / Polygon
    if isinstance(alpha_geom, MultiPolygon):
        # Take the largest polygon by area
        poly = max(alpha_geom.geoms, key=lambda g: g.area)
    elif isinstance(alpha_geom, Polygon):
        poly = alpha_geom
    else:
        # Try union in case it's something else
        unified = unary_union(alpha_geom)
        if isinstance(unified, MultiPolygon):
            poly = max(unified.geoms, key=lambda g: g.area)
        elif isinstance(unified, Polygon):
            poly = unified
        else:
            raise ValueError("Could not obtain a Polygon from alpha shape result.")

    if not poly.is_valid:
        poly = poly.buffer(0)  # fix minor self-intersections
    
    if not isinstance(poly, Polygon):
        raise ValueError("Alpha shape did not produce a valid Polygon.")

    return poly

def simplify_polygon(
    poly: Polygon,
    tolerance: float,
) -> Polygon:
    """Simplify polygon with Douglas–Peucker while preserving topology."""
    if tolerance <= 0:
        return poly
    simplified = poly.simplify(tolerance, preserve_topology=True)
    if not simplified.is_valid:
        simplified = simplified.buffer(0)
    if not isinstance(simplified, Polygon):
        raise ValueError("Simplified geometry is not a Polygon.")
    return simplified

def polygon_to_f2c_field(poly: Polygon) -> f2c.Field:
    """
    Convert a Shapely Polygon into a Fields2Cover Field.
    Only uses the exterior ring (no holes) by default.
    """
    # Exterior ring
    exterior_coords = list(poly.exterior.coords)
    ring = f2c.LinearRing()
    for x, y in exterior_coords:
        ring.addPoint(float(x), float(y))
    
    cell = f2c.Cell()
    cell.addRing(ring)
    
    # If you want to support holes:
    # for interior in poly.interiors:
    #     interior_ring = f2c.LinearRing()
    #     for x, y in interior.coords:
    #         interior_ring.addPoint(float(x), float(y))
    #     cell.addRing(interior_ring)
    
    cells = f2c.Cells()
    cells.addGeometry(cell)
    
    field = f2c.Field(cells)
    return field

def debug_plot_polygon_and_path(poly: Polygon, xy_points: np.ndarray, f2c_path=None, swaths=None) -> None:
    """Plot original 2D points, resulting polygon, and optional F2C path/swaths."""
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(10, 10))
    
    # Points
    ax.scatter(xy_points[:, 0], xy_points[:, 1], s=1, c='gray', alpha=0.5, label="Points")
    
    # Polygon exterior
    x, y = poly.exterior.xy
    ax.plot(x, y, 'k-', linewidth=2, label="Boundary")
    
    # Helper to extract line string coords
    def get_line_coords(line_string):
        # Try different ways to get points
        xs, ys = [], []
        try:
            # Try iterator
            for p in line_string:
                 xs.append(p.getX())
                 ys.append(p.getY())
        except:
            # Try size/getPoint
            try:
                 sz = line_string.size()
                 for i in range(sz):
                     p = line_string.getGeometry(i) # this is for MultiLineString, let's assume LineString
                     # actually usually access points
                     pass 
            except:
                 pass
                 
        # Fallback: direct point access loop if simple LineString
        if not xs:
            try:
                # Assuming it might be iterable of points
                pass
            except:
                pass
        return xs, ys


    # Plot Swaths (if path failed but swaths exist)
    if swaths is not None and f2c_path is None:
         # swaths is a Swaths object (collection of LineStrings)
         count = swaths.size()
         for i in range(count):
             swath = swaths.at(i)
             # Swath is a LineString
             # Extract start/end
             start = swath.startPoint()
             end = swath.endPoint()
             ax.plot([start.getX(), end.getX()], [start.getY(), end.getY()], 'b--', alpha=0.5)
         # Add dummy label
         if count > 0:
             ax.plot([], [], 'b--', alpha=0.5, label="Swaths")


    # Plot Path
    if f2c_path is not None:
        path_x = []
        path_y = []
        
        # Try to iterate over states
        try:
            # Check if we can get states directly
            count = 0
            try:
                count = f2c_path.size()
            except:
                count = len(f2c_path)
            
            for i in range(count):
                state = f2c_path.getState(i) 
                pt = state.point
                path_x.append(pt.getX())
                path_y.append(pt.getY())
                
        except Exception:
            # Newer iterators
            try:
                for state in f2c_path:
                    path_x.append(state.point.getX())
                    path_y.append(state.point.getY())
            except:
                pass
        
        if path_x:
            ax.plot(path_x, path_y, 'r-', linewidth=1.5, label="Coverage Path")
            # Plot start/end
            ax.plot(path_x[0], path_y[0], 'go', label="Start")
            ax.plot(path_x[-1], path_y[-1], 'rx', label="End")

    ax.set_aspect("equal", "box")
    ax.legend()
    ax.set_title("Coverage Path Planning")
    ax.grid(True)
    plt.show()

def compute_swath_angle(poly: Polygon) -> float:
    """
    Compute the dominant angle from the polygon's minimum rotated rectangle (oriented bounding box).
    Returns the angle in radians.
    """
    # Get minimum rotated rectangle
    rect = poly.minimum_rotated_rectangle
    
    # Get coordinates of the rectangle corners
    # The last point is the same as the first, so we ignore it
    coords = list(rect.exterior.coords)[:-1]
    
    # Calculate edge lengths
    edge_lengths = []
    for i in range(len(coords)):
        p1 = coords[i]
        p2 = coords[(i + 1) % len(coords)]
        length = math.hypot(p2[0] - p1[0], p2[1] - p1[1])
        edge_lengths.append(length)
        
    # Find the longest edge index
    max_idx = np.argmax(edge_lengths)
    
    # Calculate the angle of the longest edge
    p1 = coords[max_idx]
    p2 = coords[(max_idx + 1) % len(coords)]
    angle = math.atan2(p2[1] - p1[1], p2[0] - p1[0])
    
    # Normalize to [0, pi] because swath direction is symmetric
    if angle < 0:
        angle += math.pi
    if angle > math.pi:
        angle -= math.pi
        
    return angle

def save_path_to_csv(f2c_path, filename: str) -> None:
    """Save only x,y coordinates from path states to a CSV file."""
    print(f"[INFO] Saving path to {filename}...")
    try:
        import csv
        with open(filename, 'w', newline='') as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow(['x', 'y'])  # Header
            for i in range(f2c_path.size()):
                state = f2c_path.getState(i)  # Use getState instead of at
                point = state.point
                writer.writerow([f"{point.getX():.6f}", f"{point.getY():.6f}"])
            try:
                count = f2c_path.size()
            except:
                try:
                    count = len(f2c_path)
                except:
                    count = 0
        print(f"[INFO] Saved {count} waypoints (x,y only).")
    except Exception as e:
        print(f"[ERROR] Failed to save path to CSV: {e}")

def save_swath_corners_to_csv(f2c_swaths, filename: str) -> None:
    """Save only the start and end points of each swath."""
    print(f"[INFO] Saving swath corners to {filename}...")
    try:
        with open(filename, 'w', newline='') as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow(['x', 'y', 'yaw'])  # Header with yaw placeholder
            
            count = 0
            try:
                count = f2c_swaths.size()
            except:
                try:
                    count = len(f2c_swaths)
                except:
                    pass

            for i in range(count):
                if hasattr(f2c_swaths, 'at'):
                    swath = f2c_swaths.at(i)
                else:
                    swath = f2c_swaths[i]
                
                start = swath.startPoint()
                end = swath.endPoint()
                
                # Calculate yaw for the swath line
                dx = end.getX() - start.getX()
                dy = end.getY() - start.getY()
                yaw = math.atan2(dy, dx)
                
                # Write Start
                writer.writerow([f"{start.getX():.4f}", f"{start.getY():.4f}", f"{yaw:.4f}"])
                # Write End
                writer.writerow([f"{end.getX():.4f}", f"{end.getY():.4f}", f"{yaw:.4f}"])
                
        print(f"[INFO] Saved {count * 2} corner points (start/end pairs).")
    except Exception as e:
        print(f"[ERROR] Failed to save corners: {e}")

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert a 3D point cloud to a Fields2Cover Field via 2D concave hull."
    )
    parser.add_argument(
        "input",
        type=str,
        help="Input point cloud file (PLY/PCD/XYZ supported by Open3D).",
    )
    parser.add_argument(
        "--z-band",
        type=float,
        default=0.5,
        help="Half-height band around median Z to keep (set to 0 or negative to disable).",
    )
    parser.add_argument(
        "--alpha",
        type=float,
        default=1.5,
        help="Alpha parameter for concave hull (larger = more detailed, smaller = smoother).",
    )
    parser.add_argument(
        "--simplify",
        type=float,
        default=0.1,
        help="Polygon simplify tolerance (same units as coordinates, 0 to disable).",
    )
    parser.add_argument(
        "--max-points",
        type=int,
        default=50000,
        help="Randomly subsample if more than this many points (0 or negative to disable).",
    )
    parser.add_argument(
        "--plot",
        action="store_true",
        help="Show debug plot of 2D points and resulting polygon/path.",
    )
    # Coverage Planning Args
    parser.add_argument("--swath-width", type=float, default=1.0, help="Width of the robot's coverage tool (m)")
    parser.add_argument("--turn-radius", type=float, default=0.1, help="Minimum turning radius (m)")
    parser.add_argument("--headland-width", type=float, default=1.0, help="Width of headland (m)")
    parser.add_argument("--auto-align", action="store_true", help="Automatically align swaths to the longest edge of the field")
    parser.add_argument("--align-mode", choices=["long", "perp"], default="perp", help="Align swaths parallel to long edge ('long') or perpendicular ('perp').")
    parser.add_argument("--output-csv", type=str, default=None, help="Save generated path waypoints to CSV file")
    parser.add_argument("--corners-only", action="store_true", help="Save only start/end points of swaths to CSV")


    return parser.parse_args()

def main() -> None:
    args = parse_args()
    
    print(f"[INFO] Loading point cloud from {args.input}")
    points = load_point_cloud(args.input)
    print(f"[INFO] Loaded {points.shape[0]} points")

    # Z-band filtering
    z_band = args.z_band if args.z_band > 0 else None
    if z_band is not None:
        print(f"[INFO] Filtering points within +/- {z_band} of median Z")
    points = filter_by_z_band(points, z_band)
    print(f"[INFO] Points after Z filter: {points.shape[0]}")

    # Subsample
    max_points = args.max_points if args.max_points > 0 else None
    if max_points is not None and points.shape[0] > max_points:
        print(f"[INFO] Subsampling from {points.shape[0]} to {max_points} points")
    points_sub = subsample_points(points, max_points)

    # Project to 2D (drop Z)
    xy = points_sub[:, :2]
    
    print(f"[INFO] Computing concave hull with alpha={args.alpha}")
    poly = compute_concave_hull(xy, args.alpha)
    print(f"[INFO] Initial polygon area: {poly.area:.3f}, vertices: {len(poly.exterior.coords)}")
    
    # Simplify
    if args.simplify > 0:
        print(f"[INFO] Simplifying polygon with tolerance={args.simplify}")
        poly = simplify_polygon(poly, args.simplify)
        print(f"[INFO] Simplified polygon area: {poly.area:.3f}, vertices: {len(poly.exterior.coords)}")


    # Build Fields2Cover field
    print("[INFO] Converting polygon to Fields2Cover Field")
    field = polygon_to_f2c_field(poly)
    
    cells = field.getCellsAbsPosition()
    n_cells = cells.size()
    print(f"[INFO] Fields2Cover Field created with {n_cells} cell(s)")
    
    # --- COVERAGE PLANNING ---
    print("\n[INFO] Starting Coverage Path Planning...")
    
    robot = f2c.Robot()
    robot.cruise_speed = 0.5
    robot.cov_width = args.swath_width
    robot.min_turning_radius = args.turn_radius
    
    # 1. Headlands (optional, but recommended)
    # If headland_width > 0, we shrink the field to create a turning area
    working_area = field.getCellsAbsPosition() # Default is full area
    
    if args.headland_width > 0:
        print(f"[INFO] Generating headlands (width={args.headland_width}m)")
        try:
            hl_gen = f2c.HG_Const_gen()
            # The result is a Cells object representing the inner area
            no_headlands = hl_gen.generateHeadlands(field.getCellsAbsPosition(), args.headland_width)
            if no_headlands.size() > 0:
                working_area = no_headlands
                print(f"[INFO] Headlands generated. Inner area cells: {working_area.size()}")
            else:
                print("[WARN] Headland generation resulted in empty area! Using full field.")
        except Exception as e:
             print(f"[WARN] Headland generation failed: {e}. Using full field.")

    # 2. Swath Generation (Brute Force for optimal angle)
    print(f"[INFO] Generating swaths (width={args.swath_width}m)")
    sg = f2c.SG_BruteForce()

    # Determine swath direction
    if args.auto_align:
        print("[INFO] Auto-aligning swaths to the longest edge...")
        angle_main = compute_swath_angle(poly)
        
        if args.align_mode == "long":
            direction = angle_main
            print(f"[INFO] Aligning parallel to long edge ({math.degrees(direction):.2f} deg)")
        else:
            direction = angle_main + math.pi / 2.0
            print(f"[INFO] Aligning perpendicular to long edge ({math.degrees(direction):.2f} deg)")
            
    else:
        # Manual or default (aligned with X axis, possibly with manual offset in code)
        # You can customize this default if needed, e.g. math.pi
        direction = math.pi
        print(f"[INFO] Using default swath direction: {math.degrees(direction):.2f} degrees (parallel to X-axis)")

    try:
        swaths = sg.generateSwaths(direction, robot.cov_width, working_area.getGeometry(0))
    except Exception:
        # Fallback for different API signatures or if getGeometry(0) fails
        try:
             # Try iterating over all cells if multiple
             swaths = f2c.Swaths()
             for i in range(working_area.size()):
                 s = sg.generateSwaths(direction, robot.cov_width, working_area.getGeometry(i))
                 for j in range(s.size()):
                     swaths.push_back(s.at(j))
        except Exception as e:
            print(f"[ERROR] Swath generation failed: {e}")
            sys.exit(1)

    print(f"[INFO] Generated {swaths.size()} swaths.")

    # 3. Route Planning (Boustrophedon - Snake pattern)
    print("[INFO] Planning route (Boustrophedon)")
    # 1. Sort the swaths (this is what Boustrophedon does: sorts them to minimize travel)
    rp = f2c.RP_Boustrophedon()
    if hasattr(rp, "genSortedSwaths"):
        swaths = rp.genSortedSwaths(swaths)
    
    path = None
    if not args.corners_only:
        # 4. Path Planning (Dubins Curves for smooth turns) - only if we want full path
        print("[INFO] Generating smooth path (Dubins)")
        pp = f2c.PP_PathPlanning()
        
        # Set turn radius on robot
        robot.setMinTurningRadius(args.turn_radius)
        
        try:
            # Try Dubins curves
            dubins = f2c.PP_DubinsCurves()
            path = pp.planPath(robot, swaths, dubins)
        except (AttributeError, TypeError) as e:
            print(f"[WARN] Dubins path planning failed: {e}")
            try:
                path = pp.planPath(robot, swaths)
            except Exception:
                pass

    if path is not None:
        # Estimate length
        path_length = path.length() if hasattr(path, 'length') else 0.0
        print(f"[INFO] Final Path Length: {path_length:.2f} m")
        
        # Save to CSV if requested
        if args.output_csv:
            save_path_to_csv(path, args.output_csv)
            
    elif args.corners_only:
        print("[INFO] Full path generation skipped (corners only requested).")
        if args.output_csv:
            save_swath_corners_to_csv(swaths, args.output_csv)
            
    elif swaths is not None:
        print("[INFO] Path planning skipped (using sorted swaths).")

    # Optional plot
    if args.plot:
        try:
            debug_plot_polygon_and_path(poly, xy, path, swaths)
        except ImportError:
            print("[WARN] matplotlib not installed; cannot plot.")

    print("[INFO] Done.")

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"[ERROR] {e}", file=sys.stderr)
        sys.exit(1)
