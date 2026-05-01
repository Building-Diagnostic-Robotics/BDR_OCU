#!/usr/bin/env python3
"""Reusable backend helpers for the Fields2Cover coverage planner GUI/CLI."""

from __future__ import annotations

import math
from typing import Dict, List, Optional, Tuple

import numpy as np
import open3d as o3d
from shapely.geometry import Polygon, GeometryCollection, MultiPolygon
from shapely.ops import unary_union
import fields2cover as f2c


# =============================================================================
# Basic point-cloud utilities
# =============================================================================

def load_point_cloud(path: str) -> np.ndarray:
    """Load a PCD/PLY/XYZ file and return Nx3 numpy array."""
    pcd = o3d.io.read_point_cloud(path)
    if pcd.is_empty():
        raise ValueError(f"Point cloud is empty: {path}")
    pts = np.asarray(pcd.points)
    if pts.shape[1] != 3:
        raise ValueError(f"Expected 3D points, got shape {pts.shape}")
    return pts


def filter_by_z_band(points: np.ndarray, z_band: Optional[float]) -> np.ndarray:
    """Keep points within +/- z_band of median z."""
    if z_band is None or z_band <= 0:
        return points
    z = points[:, 2]
    z_med = np.median(z)
    mask = np.abs(z - z_med) <= z_band
    filtered = points[mask]
    if filtered.size == 0:
        raise ValueError(
            "Z-band filter removed all points. Try a larger z-band value."
        )
    return filtered


def subsample_points(
    points: np.ndarray,
    max_points: Optional[int],
    rng: Optional[np.random.Generator] = None,
) -> np.ndarray:
    """Randomly subsample points if there are more than max_points."""
    if max_points is None or max_points <= 0 or points.shape[0] <= max_points:
        return points
    if rng is None:
        rng = np.random.default_rng()
    idx = rng.choice(points.shape[0], size=max_points, replace=False)
    return points[idx]


# =============================================================================
# Polygon / hull helpers
# =============================================================================

def compute_concave_hull(
    xy: np.ndarray,
    alpha: float,
    method: str = "alphashape",
) -> Polygon:
    """
    Compute a concave hull from 2D points using various methods.

    Args
    ----
    xy:
        (N, 2) array of 2D points.
    alpha:
        For "alphashape": alpha parameter (smaller = more detailed, larger = smoother).
        For "grid": grid cell size.
    method:
        "alphashape" (recommended), "delaunay", or "grid".
    """
    if xy.shape[0] < 3:
        raise ValueError("Need at least 3 points to compute a polygon.")

    method = method.lower()
    if method == "grid":
        return _compute_grid_boundary(xy, grid_size=alpha)
    elif method == "delaunay":
        return _compute_delaunay_boundary(xy)
    else:  # "alphashape"
        return _compute_alphashape_hull(xy, alpha)


def _compute_alphashape_hull(xy: np.ndarray, alpha: float) -> Polygon:
    """Alpha-shape based concave hull (most accurate for highly concave roofs)."""
    import alphashape

    hull = alphashape.alphashape(xy, alpha)
    hull = unary_union(hull)

    if isinstance(hull, MultiPolygon):
        hull = max(hull.geoms, key=lambda g: g.area)

    if not isinstance(hull, Polygon):
        raise ValueError("Alpha shape did not produce a Polygon.")

    if not hull.is_valid:
        hull = hull.buffer(0)

    return hull


def _compute_convex_hull(xy: np.ndarray) -> Polygon:
    """Fast convex hull for fallback cases."""
    from scipy.spatial import ConvexHull

    if xy.shape[0] < 3:
        raise ValueError("Need at least 3 points for convex hull.")
    hull = ConvexHull(xy)
    vertices = xy[hull.vertices]
    poly = Polygon(vertices)
    if not poly.is_valid:
        poly = poly.buffer(0)
    return poly


def _compute_grid_boundary(xy: np.ndarray, grid_size: Optional[float] = None) -> Polygon:
    """
    Grid-based boundary extraction (fast but less accurate than alphashape).

    Converts points to a grid, finds boundary pixels, and converts back to a polygon.
    """
    if xy.shape[0] < 3:
        raise ValueError("Need at least 3 points for grid boundary.")

    if grid_size is None:
        x_range = xy[:, 0].max() - xy[:, 0].min()
        y_range = xy[:, 1].max() - xy[:, 1].min()
        max_range = max(x_range, y_range)
        grid_size = max_range / 2000.0 if max_range > 0 else 1.0

    x_min, y_min = xy.min(axis=0)
    x_max, y_max = xy.max(axis=0)
    padding = grid_size * 2
    x_min -= padding
    y_min -= padding
    x_max += padding
    y_max += padding

    x_indices = ((xy[:, 0] - x_min) / grid_size).astype(int)
    y_indices = ((xy[:, 1] - y_min) / grid_size).astype(int)

    grid_width = int((x_max - x_min) / grid_size) + 1
    grid_height = int((y_max - y_min) / grid_size) + 1

    grid = np.zeros((grid_height, grid_width), dtype=bool)
    grid[y_indices, x_indices] = True

    from scipy import ndimage

    eroded = ndimage.binary_erosion(grid, structure=np.ones((3, 3)))
    boundary = grid & (~eroded)

    boundary_y, boundary_x = np.where(boundary)
    if len(boundary_x) == 0:
        return _compute_convex_hull(xy)

    boundary_points = np.column_stack(
        [boundary_x * grid_size + x_min, boundary_y * grid_size + y_min]
    )

    from scipy.spatial import ConvexHull

    hull = ConvexHull(boundary_points)
    vertices = boundary_points[hull.vertices]

    poly = Polygon(vertices)
    if not poly.is_valid:
        poly = poly.buffer(0)
    return poly


def _compute_delaunay_boundary(xy: np.ndarray) -> Polygon:
    """Delaunay triangulation + boundary extraction (good for mild concavities)."""
    from scipy.spatial import Delaunay, ConvexHull

    if xy.shape[0] < 3:
        raise ValueError("Need at least 3 points for Delaunay triangulation.")

    tri = Delaunay(xy)
    edge_count: Dict[Tuple[int, int], int] = {}

    for simplex in tri.simplices:
        for i in range(3):
            edge = tuple(sorted([simplex[i], simplex[(i + 1) % 3]]))
            edge_count[edge] = edge_count.get(edge, 0) + 1

    boundary_edges = [edge for edge, count in edge_count.items() if count == 1]
    if not boundary_edges:
        return _compute_convex_hull(xy)

    edge_dict = {}
    for u, v in boundary_edges:
        edge_dict.setdefault(u, []).append(v)
        edge_dict.setdefault(v, []).append(u)

    start = boundary_edges[0][0]
    current = start
    vertices = [current]
    visited = set([current])

    while True:
        neighbors = [n for n in edge_dict.get(current, []) if n not in visited]
        if not neighbors:
            break
        nxt = neighbors[0]
        vertices.append(nxt)
        visited.add(nxt)
        current = nxt
        if current == start:
            break

    if len(vertices) < 3:
        return _compute_convex_hull(xy)

    poly = Polygon(xy[vertices])
    if not poly.is_valid:
        poly = poly.buffer(0)
    return poly


def simplify_polygon(poly: Polygon, tolerance: float) -> Polygon:
    """Simplify polygon while keeping topology."""
    if tolerance <= 0:
        return poly
    simplified = poly.simplify(tolerance, preserve_topology=True)
    if not simplified.is_valid:
        simplified = simplified.buffer(0)
    if not isinstance(simplified, Polygon):
        raise ValueError("Simplified geometry is not a Polygon.")
    return simplified


def compute_swath_angle(poly: Polygon) -> float:
    """Dominant angle from polygon's minimum rotated rectangle (0–pi)."""
    rect = poly.minimum_rotated_rectangle
    coords = list(rect.exterior.coords)[:-1]
    if len(coords) < 2:
        return 0.0

    edges = []
    for i in range(len(coords)):
        p1 = coords[i]
        p2 = coords[(i + 1) % len(coords)]
        length = math.hypot(p2[0] - p1[0], p2[1] - p1[1])
        edges.append((p1, p2, length))

    p1, p2, _ = max(edges, key=lambda e: e[2])
    angle = math.atan2(p2[1] - p1[1], p2[0] - p1[0])
    if angle < 0:
        angle += math.pi
    if angle > math.pi:
        angle -= math.pi
    return angle


# =============================================================================
# Fields2Cover polygon conversion
# =============================================================================

def polygon_to_f2c_field(poly: Polygon) -> f2c.Field:
    """Convert a shapely Polygon (with holes) into a Fields2Cover Field."""
    ring = f2c.LinearRing()
    for x, y in list(poly.exterior.coords):
        ring.addPoint(float(x), float(y))

    cell = f2c.Cell()
    cell.addRing(ring)

    for interior in poly.interiors:
        inner_ring = f2c.LinearRing()
        for x, y in interior.coords:
            inner_ring.addPoint(float(x), float(y))
        cell.addRing(inner_ring)

    cells = f2c.Cells()
    cells.addGeometry(cell)
    return f2c.Field(cells)


def f2c_cell_to_polygon(cell: f2c.Cell) -> Polygon:
    """Convert a Fields2Cover Cell into a Shapely Polygon."""
    if cell.size() == 0:
        raise ValueError("Cell has no rings")

    exterior_ring = cell.getGeometry(0)
    if exterior_ring.size() == 0:
        raise ValueError("Exterior ring is empty")

    exterior_coords = [
        (exterior_ring.getX(i), exterior_ring.getY(i))
        for i in range(exterior_ring.size())
    ]
    if exterior_coords and exterior_coords[0] != exterior_coords[-1]:
        exterior_coords.append(exterior_coords[0])

    interiors = []
    for ring_idx in range(1, cell.size()):
        interior_ring = cell.getGeometry(ring_idx)
        if interior_ring.size() == 0:
            continue
        coords = [(interior_ring.getX(i), interior_ring.getY(i))
                  for i in range(interior_ring.size())]
        if coords and coords[0] != coords[-1]:
            coords.append(coords[0])
        if len(coords) >= 4:
            interiors.append(coords)

    poly = Polygon(exterior_coords, interiors) if interiors else Polygon(exterior_coords)
    if not poly.is_valid:
        poly = poly.buffer(0)
    return poly


# =============================================================================
# Concavity & decomposition
# =============================================================================

def _is_field_concave(poly: Polygon) -> bool:
    """Heuristic concavity test (holes or clearly smaller than convex hull)."""
    if poly.is_empty or not poly.is_valid:
        return False
    if len(poly.interiors) > 0:
        return True
    convex = poly.convex_hull
    return poly.area < convex.area * 0.95


def _decompose_field(
    field: f2c.Field,
    decomposition_type: str = "boustrophedon",
) -> f2c.Cells:
    """
    Decompose a concave field into convex sub-fields (if F2C decomposition is available).
    """
    cells = field.getCellsAbsPosition()
    decomp = None

    if decomposition_type.lower() == "trapezoidal" and hasattr(f2c, "DC_Trapezoidal"):
        try:
            decomp = f2c.DC_Trapezoidal()
        except (AttributeError, TypeError):
            decomp = None
    elif decomposition_type.lower() == "boustrophedon" and hasattr(f2c, "DC_Boustrophedon"):
        try:
            decomp = f2c.DC_Boustrophedon()
        except (AttributeError, TypeError):
            decomp = None

    if decomp is None:
        return cells

    try:
        if hasattr(decomp, "splitField"):
            decomposed = decomp.splitField(field)
            if hasattr(decomposed, "getCellsAbsPosition"):
                c = decomposed.getCellsAbsPosition()
                if c.size() > 0:
                    return c
            elif hasattr(decomposed, "getGeometry"):
                return decomposed  # already Cells
        elif hasattr(decomp, "splitCells"):
            decomposed = decomp.splitCells(cells)
            if hasattr(decomposed, "size") and decomposed.size() > 0:
                return decomposed
    except Exception as e:
        import warnings
        warnings.warn(f"Decomposition failed: {e}. Using original field.", RuntimeWarning)

    return cells


# =============================================================================
# Working area / headlands
# =============================================================================

def _generate_working_area(
    field: f2c.Field, headland_width: float
) -> Tuple[f2c.Cells, f2c.Cells]:
    """Return (original_cells, headland-cropped working area cells)."""
    cells = field.getCellsAbsPosition()
    working_area = cells
    if headland_width > 0:
        try:
            hl_gen = f2c.HG_Const_gen()
            cropped = hl_gen.generateHeadlands(cells, headland_width)
            if cropped.size() > 0:
                working_area = cropped
        except Exception:
            pass
    return cells, working_area


# =============================================================================
# Swath generation
# =============================================================================

def _generate_swaths(
    working_area: f2c.Cells,
    poly: Polygon,
    swath_width: float,
    cfg: Dict,
) -> f2c.Swaths:
    """
    Generate swaths for coverage planning.

    Respects auto-align, objective functions, and multi-cell working areas.
    """
    sg = f2c.SG_BruteForce()

    # Objective function (if supported by this F2C build)
    obj_function_type = cfg.get("obj_function", "nswath_modified").lower()
    try:
        obj = None
        if obj_function_type == "nswath_modified" and hasattr(f2c, "ObjNSwathModified"):
            obj = f2c.ObjNSwathModified()
        elif obj_function_type == "nswath" and hasattr(f2c, "ObjNSwath"):
            obj = f2c.ObjNSwath()
        elif obj_function_type == "swath_length" and hasattr(f2c, "ObjSwathLength"):
            obj = f2c.ObjSwathLength()
        elif hasattr(f2c, "ObjNSwathModified"):
            obj = f2c.ObjNSwathModified()

        if obj is not None and hasattr(sg, "setObjFunction"):
            sg.setObjFunction(obj)
    except (AttributeError, TypeError):
        pass

    auto_align = cfg.get("auto_align", False)
    align_mode = cfg.get("align_mode", "perp")

    if auto_align:
        base_angle = compute_swath_angle(poly)
        direction = base_angle if align_mode == "long" else base_angle + math.pi / 2.0
    else:
        direction = math.pi  # global X-axis (can be changed)

    # Generate swaths (single- or multi-cell)
    try:
        swaths = sg.generateSwaths(direction, swath_width, working_area.getGeometry(0))
    except Exception:
        swaths = f2c.Swaths()
        for i in range(working_area.size()):
            part = sg.generateSwaths(direction, swath_width, working_area.getGeometry(i))
            for j in range(part.size()):
                swaths.push_back(part.at(j))

    return swaths


# =============================================================================
# Swath iteration helper
# =============================================================================

def _iter_swaths(container):
    """
    Yield swath objects from a Fields2Cover swath container.

    Supports:
      - Swaths with .size() and .at(i)
      - VectorSwaths with .size() and __getitem__
      - Plain Python sequences (len + [i])
    """
    if container is None:
        return
    try:
        n = container.size()
    except AttributeError:
        n = len(container)
    for i in range(n):
        try:
            if hasattr(container, "at"):
                swath = container.at(i)
            else:
                swath = container[i]
            
            # Verify that we got a Swath, not a Swaths collection
            # If it's a Swaths collection, iterate through it instead
            if hasattr(swath, "startPoint") and hasattr(swath, "endPoint"):
                # This is a single Swath object
                yield swath
            elif hasattr(swath, "size"):
                # This is a Swaths collection, iterate through it
                for j in range(swath.size()):
                    if hasattr(swath, "at"):
                        yield swath.at(j)
                    else:
                        yield swath[j]
            else:
                # Try to use it as-is (might work)
                yield swath
        except Exception as e:
            import warnings
            warnings.warn(f"Failed to extract swath at index {i}: {e}", RuntimeWarning)
            continue


# =============================================================================
# Axial-turn path (for robots that can turn in place)
# =============================================================================

def swaths_to_axial_turn_path(swaths) -> f2c.Path:
    """
    Convert an ordered set of swaths into a path for an axial-turn robot.

    - Each swath becomes a straight segment (start -> end).
    - Path has instantaneous heading changes at swath endpoints.
    - No Dubins / curvature constraints are applied.
    """
    path = f2c.Path()
    if swaths is None:
        return path

    try:
        for swath in _iter_swaths(swaths):
            # Verify swath has required methods
            if not hasattr(swath, "startPoint") or not hasattr(swath, "endPoint"):
                import warnings
                warnings.warn(
                    f"Swath object missing startPoint/endPoint methods. Type: {type(swath)}. Skipping.",
                    RuntimeWarning
                )
                continue
            
            start = swath.startPoint()
            end = swath.endPoint()

            dx = end.getX() - start.getX()
            dy = end.getY() - start.getY()
            heading = math.atan2(dy, dx)
            vx = math.cos(heading)
            vy = math.sin(heading)

            p_start = f2c.Point(start.getX(), start.getY())
            p_end = f2c.Point(end.getX(), end.getY())

            path.addState(p_start, vx, vy)
            path.addState(p_end, vx, vy)
    except Exception as e:
        import warnings
        warnings.warn(
            f"Error in swaths_to_axial_turn_path: {e}. Type: {type(swaths)}",
            RuntimeWarning
        )
        raise

    return path


# =============================================================================
# Coverage generation (main entry points)
# =============================================================================

def generate_coverage(
    field: f2c.Field,
    boundary_poly: Polygon,
    cfg: Dict,
    roi_polygon: Optional[Polygon] = None,
    corners_only: bool = False,
    use_axial_turns: Optional[bool] = None,
) -> Tuple[Optional[f2c.Swaths], Optional[f2c.Route], Optional[f2c.Path]]:
    """
    Generate swaths, route, and path for a single field (with optional ROI).

    cfg keys (all optional except swath/headland/turn):
      - swath_width: float
      - headland_width: float
      - turn_radius: float
      - auto_align: bool
      - align_mode: "long" or "perp"
      - route_pattern: "boustro", "snake", "spiral"
      - path_planner: "dubins", "dubins_cc", "reeds", "reeds_hc", "none", "axial"
      - obj_function: "nswath_modified", "nswath", "swath_length"
      - start_point: (x, y)
      - end_point: (x, y)
      - use_decomposition: bool
      - decomposition_type: "boustrophedon" or "trapezoidal"
      - use_axial_turns: bool   # alternative way to request axial path
    """
    if field is None:
        raise ValueError("Field cannot be None")
    if boundary_poly is None or boundary_poly.is_empty:
        raise ValueError("Boundary polygon is None or empty")

    # Resolve axial-turn flag: explicit arg wins, otherwise from cfg
    cfg_use_axial = bool(cfg.get("use_axial_turns", False))
    if use_axial_turns is not None:
        cfg_use_axial = bool(use_axial_turns)

    poly = boundary_poly
    if roi_polygon is not None:
        if roi_polygon.is_empty:
            raise ValueError("ROI polygon is empty")
        inter = boundary_poly.intersection(roi_polygon)
        if inter.is_empty:
            raise ValueError("ROI does not intersect main polygon.")
        if isinstance(inter, MultiPolygon):
            inter = max(inter.geoms, key=lambda g: g.area)
        poly = inter
        field = polygon_to_f2c_field(poly)

    # Optional decomposition for concave fields
    use_decomposition = cfg.get("use_decomposition", False)
    decomposition_type = cfg.get("decomposition_type", "boustrophedon").lower()

    if use_decomposition and (_is_field_concave(poly) or len(poly.interiors) > 0):
        decomposed_cells = _decompose_field(field, decomposition_type)
        if decomposed_cells.size() > 1:
            return _generate_coverage_decomposed(
                field,
                poly,
                decomposed_cells,
                cfg,
                corners_only=corners_only,
                use_axial_turns=cfg_use_axial,
            )

    # Working area with headlands
    full_cells, working_area = _generate_working_area(field, cfg.get("headland_width", 0.0))
    if working_area.size() == 0:
        raise ValueError("Working area is empty after headland generation")

    swath_width = cfg.get("swath_width", 1.0)
    if swath_width <= 0:
        raise ValueError(f"Swath width must be positive, got {swath_width}")

    swaths = _generate_swaths(working_area, poly, swath_width, cfg)
    if swaths is None or swaths.size() == 0:
        return None, None, None

    # Route planning
    route: Optional[f2c.Route] = None
    generate_route = not corners_only

    if generate_route:
        try:
            swaths_by_cells = f2c.SwathsByCells()
            swaths_by_cells.push_back(swaths)

            pattern = (cfg.get("route_pattern", "boustro") or "boustro").lower()
            sorted_swaths = swaths

            if pattern == "snake" and hasattr(f2c, "RP_Snake"):
                sorter = f2c.RP_Snake()
                if hasattr(sorter, "genSortedSwaths"):
                    sorted_swaths = sorter.genSortedSwaths(swaths)
            elif pattern == "spiral" and hasattr(f2c, "RP_Spiral"):
                sorter = f2c.RP_Spiral()
                if hasattr(sorter, "genSortedSwaths"):
                    sorted_swaths = sorter.genSortedSwaths(swaths)
            else:
                sorter = f2c.RP_Boustrophedon()
                if hasattr(sorter, "genSortedSwaths"):
                    sorted_swaths = sorter.genSortedSwaths(swaths)

            swaths_by_cells = f2c.SwathsByCells()
            swaths_by_cells.push_back(sorted_swaths)

            # Route optimizer if available, else base planner
            route_planner = None
            if hasattr(f2c, "RP_RouteOptimizer"):
                try:
                    route_planner = f2c.RP_RouteOptimizer()
                    start_point = cfg.get("start_point")
                    end_point = cfg.get("end_point")
                    if start_point and end_point:
                        if (isinstance(start_point, (tuple, list)) and
                                isinstance(end_point, (tuple, list)) and
                                len(start_point) == 2 and len(end_point) == 2):
                            sp = f2c.Point(float(start_point[0]), float(start_point[1]))
                            ep = f2c.Point(float(end_point[0]), float(end_point[1]))
                            if hasattr(route_planner, "setStartPoint"):
                                route_planner.setStartPoint(sp)
                            if hasattr(route_planner, "setEndPoint"):
                                route_planner.setEndPoint(ep)
                except (AttributeError, TypeError):
                    route_planner = None

            if route_planner is None:
                route_planner = f2c.RP_RoutePlannerBase()

            route = route_planner.genRoute(working_area, swaths_by_cells)
            swaths = sorted_swaths

        except Exception as e:
            import warnings
            warnings.warn(f"Route generation failed: {e}", RuntimeWarning)
            route = None

    if corners_only:
        return swaths, route, None

    # Path planning
    path: Optional[f2c.Path] = None

    # 1) Axial-turn robots: path is just ordered swath segments
    if cfg_use_axial:
        swaths_for_path = swaths
        # If route exposes an ordered swath vector, prefer that ordering
        if route is not None and hasattr(route, "getVectorSwaths"):
            try:
                route_swaths = route.getVectorSwaths()
                if route_swaths.size() > 0:
                    swaths_for_path = route_swaths
            except Exception:
                pass
        path = swaths_to_axial_turn_path(swaths_for_path)

    # 2) Non-axial robots: use Dubins / Reeds / etc.
    else:
        path = _generate_dubins_like_path(swaths, route, cfg)

    return swaths, route, path


def _generate_dubins_like_path(
    swaths: f2c.Swaths,
    route: Optional[f2c.Route],
    cfg: Dict,
) -> Optional[f2c.Path]:
    """Helper: Dubins/Reeds path planning part of generate_coverage."""
    try:
        pp = f2c.PP_PathPlanning()
    except Exception as e:
        import warnings
        warnings.warn(f"PathPlanning construction failed: {e}", RuntimeWarning)
        return None

    robot = f2c.Robot()
    robot.cov_width = cfg.get("swath_width", 1.0)
    robot.setMinTurningRadius(cfg.get("turn_radius", 0.1))

    planner_choice = (cfg.get("path_planner", "dubins") or "dubins").lower()
    turn_model = None

    if planner_choice == "dubins_cc" and hasattr(f2c, "PP_DubinsCurvesCC"):
        turn_model = f2c.PP_DubinsCurvesCC()
    elif planner_choice == "reeds" and hasattr(f2c, "PP_ReedsSheppCurves"):
        turn_model = f2c.PP_ReedsSheppCurves()
    elif planner_choice == "reeds_hc" and hasattr(f2c, "PP_ReedsSheppCurvesHC"):
        turn_model = f2c.PP_ReedsSheppCurvesHC()
    elif planner_choice == "none":
        turn_model = None
    else:
        if hasattr(f2c, "PP_DubinsCurves"):
            turn_model = f2c.PP_DubinsCurves()

    if turn_model is None and hasattr(f2c, "PP_DubinsCurves"):
        turn_model = f2c.PP_DubinsCurves()

    path: Optional[f2c.Path] = None

    try:
        if route is not None and turn_model is not None:
            try:
                path = pp.planPath(robot, route, turn_model)
            except Exception as e:
                import warnings
                warnings.warn(f"Route-based path planning failed: {e}. Falling back to swaths.", RuntimeWarning)
                path = None

        if path is None or path.size() == 0:
            if turn_model is not None:
                path = pp.planPath(robot, swaths, turn_model)
            else:
                try:
                    path = pp.planPath(robot, swaths)
                except (TypeError, AttributeError):
                    if hasattr(f2c, "PP_DubinsCurves"):
                        default_turn_model = f2c.PP_DubinsCurves()
                        path = pp.planPath(robot, swaths, default_turn_model)
                    else:
                        path = None
    except Exception as e:
        import warnings
        warnings.warn(f"Path planning failed: {e}", RuntimeWarning)
        path = None

    return path


def _generate_coverage_decomposed(
    field: f2c.Field,
    boundary_poly: Polygon,
    decomposed_cells: f2c.Cells,
    cfg: Dict,
    corners_only: bool = False,
    use_axial_turns: bool = False,
) -> Tuple[Optional[f2c.Swaths], Optional[f2c.Route], Optional[f2c.Path]]:
    """Coverage planning for decomposed (multi-cell) fields."""
    all_swaths = f2c.Swaths()
    swath_width = cfg.get("swath_width", 1.0)

    for i in range(decomposed_cells.size()):
        try:
            sub_cell = decomposed_cells.getGeometry(i)
            temp_cells = f2c.Cells()
            temp_cells.addGeometry(sub_cell)
            temp_field = f2c.Field(temp_cells)

            _, working_sub_area = _generate_working_area(
                temp_field, cfg.get("headland_width", 0.0)
            )
            if working_sub_area.size() == 0:
                continue

            try:
                sub_poly = f2c_cell_to_polygon(sub_cell)
            except Exception as e:
                import warnings
                warnings.warn(
                    f"Failed to convert sub-cell {i} to polygon for auto-align: {e}. "
                    "Using full boundary polygon.",
                    RuntimeWarning,
                )
                sub_poly = boundary_poly

            sub_swaths = _generate_swaths(working_sub_area, sub_poly, swath_width, cfg)
            if sub_swaths is not None and sub_swaths.size() > 0:
                for j in range(sub_swaths.size()):
                    all_swaths.push_back(sub_swaths.at(j))
        except Exception as e:
            import warnings
            warnings.warn(f"Failed to process decomposed sub-field {i}: {e}", RuntimeWarning)
            continue

    if all_swaths.size() == 0:
        return None, None, None

    # Route for combined swaths
    route: Optional[f2c.Route] = None
    if not corners_only:
        try:
            swaths_by_cells = f2c.SwathsByCells()
            swaths_by_cells.push_back(all_swaths)

            pattern = (cfg.get("route_pattern", "boustro") or "boustro").lower()
            sorted_swaths = all_swaths

            if pattern == "snake" and hasattr(f2c, "RP_Snake"):
                sorter = f2c.RP_Snake()
                if hasattr(sorter, "genSortedSwaths"):
                    sorted_swaths = sorter.genSortedSwaths(all_swaths)
            elif pattern == "spiral" and hasattr(f2c, "RP_Spiral"):
                sorter = f2c.RP_Spiral()
                if hasattr(sorter, "genSortedSwaths"):
                    sorted_swaths = sorter.genSortedSwaths(all_swaths)
            else:
                sorter = f2c.RP_Boustrophedon()
                if hasattr(sorter, "genSortedSwaths"):
                    sorted_swaths = sorter.genSortedSwaths(all_swaths)

            swaths_by_cells = f2c.SwathsByCells()
            swaths_by_cells.push_back(sorted_swaths)

            _, route_working_area = _generate_working_area(field, cfg.get("headland_width", 0.0))
            if route_working_area.size() == 0:
                import warnings
                warnings.warn(
                    "Working area is empty after headland generation for route; using full field.",
                    RuntimeWarning,
                )
                route_working_area = field.getCellsAbsPosition()

            route_planner = None
            if hasattr(f2c, "RP_RouteOptimizer"):
                try:
                    route_planner = f2c.RP_RouteOptimizer()
                except (AttributeError, TypeError):
                    route_planner = None
            if route_planner is None:
                route_planner = f2c.RP_RoutePlannerBase()

            route = route_planner.genRoute(route_working_area, swaths_by_cells)
            all_swaths = sorted_swaths

        except Exception as e:
            import warnings
            warnings.warn(f"Route generation failed for decomposed field: {e}", RuntimeWarning)
            route = None

    if corners_only:
        return all_swaths, route, None

    # Path
    path: Optional[f2c.Path] = None

    if use_axial_turns:
        swaths_for_path = all_swaths
        if route is not None and hasattr(route, "getVectorSwaths"):
            try:
                route_swaths = route.getVectorSwaths()
                if route_swaths.size() > 0:
                    swaths_for_path = route_swaths
            except Exception:
                pass
        path = swaths_to_axial_turn_path(swaths_for_path)
    else:
        path = _generate_dubins_like_path(all_swaths, route, cfg)

    return all_swaths, route, path


# =============================================================================
# Export helpers
# =============================================================================

def save_swath_corners_to_csv(f2c_swaths, filename: str) -> None:
    """Save the start/end point of each swath with heading."""
    import csv

    with open(filename, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(["x", "y", "yaw", "type"])

        for swath in _iter_swaths(f2c_swaths):
            start = swath.startPoint()
            end = swath.endPoint()
            dx = end.getX() - start.getX()
            dy = end.getY() - start.getY()
            yaw = math.atan2(dy, dx)
            writer.writerow([f"{start.getX():.4f}", f"{start.getY():.4f}", f"{yaw:.6f}", "START"])
            writer.writerow([f"{end.getX():.4f}", f"{end.getY():.4f}", f"{yaw:.6f}", "END"])


# =============================================================================
# Downsampling helpers
# =============================================================================

def downsample_random(points: np.ndarray, max_points: int) -> np.ndarray:
    return subsample_points(points, max_points)


def downsample_voxel(points: np.ndarray, voxel_size: float) -> np.ndarray:
    if voxel_size <= 0:
        return points
    cloud = o3d.geometry.PointCloud()
    cloud.points = o3d.utility.Vector3dVector(points)
    down = cloud.voxel_down_sample(voxel_size=voxel_size)
    return np.asarray(down.points)


def downsample_statistical(points: np.ndarray, mean_k: int, std_ratio: float) -> np.ndarray:
    if mean_k <= 0 or std_ratio <= 0:
        return points
    cloud = o3d.geometry.PointCloud()
    cloud.points = o3d.utility.Vector3dVector(points)
    filtered, _ = cloud.remove_statistical_outlier(nb_neighbors=mean_k, std_ratio=std_ratio)
    return np.asarray(filtered.points)


def apply_downsample(points: np.ndarray, method: str, params: Dict) -> np.ndarray:
    """Dispatch to the chosen downsampling method."""
    method = method.lower()
    if method in ("random", "uniform"):
        max_points = int(params.get("max_points", 0))
        return downsample_random(points, max_points)
    if method == "voxel":
        voxel_size = float(params.get("voxel_size", 0.05))
        return downsample_voxel(points, voxel_size)
    if method == "statistical":
        mean_k = int(params.get("mean_k", 20))
        std_ratio = float(params.get("std_ratio", 1.0))
        return downsample_statistical(points, mean_k, std_ratio)
    return points
