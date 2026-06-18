/**
 * @file coverage_pipeline.cpp
 * @brief Implementation of coverage planning pipeline
 */

#include "coverage_pipeline.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <random>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <set>
#include <map>
#include <limits>

// Robust polygon ops (ROI intersection, obstacle clipping, validity checks)
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/policies/is_valid/failing_reason_policy.hpp>

#include <pcl/io/pcd_io.h>
#include <pcl/io/ply_io.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/statistical_outlier_removal.h>

#ifdef HAVE_FIELDS2COVER
#include <fields2cover.h>
#endif

#ifdef HAVE_CGAL
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Alpha_shape_2.h>
#include <CGAL/Alpha_shape_vertex_base_2.h>
#include <CGAL/Alpha_shape_face_base_2.h>
#include <CGAL/Delaunay_triangulation_2.h>
#include <CGAL/Triangulation_data_structure_2.h>
#include <CGAL/algorithm.h>
#include <CGAL/convex_hull_2.h>
#endif

namespace f2c_cpp {

// Global progress callback
static ProgressCallback g_progressCallback = nullptr;

void setProgressCallback(ProgressCallback callback) {
    g_progressCallback = callback;
}

static void reportProgress(int percent, const std::string& message) {
    if (g_progressCallback) {
        g_progressCallback(percent, message);
    }
}

namespace {
namespace bg = boost::geometry;

// We want outer rings CCW and inner rings CW (matches common GIS conventions and our F2C usage).
using BgPoint = bg::model::d2::point_xy<double>;
using BgPolygon = bg::model::polygon<BgPoint, /*ClockWise=*/false, /*Closed=*/true>;
using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;

constexpr double kGeomEps = 1e-9;
constexpr double kMinValidArea = 1e-10;  // m^2-ish (depends on input units)

static bool nearEqual(double a, double b, double eps = kGeomEps) {
    return std::fabs(a - b) <= eps;
}

static bool nearPoint(const Point2D& a, const Point2D& b, double eps = kGeomEps) {
    return nearEqual(a.x, b.x, eps) && nearEqual(a.y, b.y, eps);
}

static Polygon2D sanitizePolygon2D(const Polygon2D& in, double eps = kGeomEps) {
    Polygon2D out;
    out.reserve(in.size());
    for (const auto& p : in) {
        if (out.empty() || !nearPoint(out.back(), p, eps)) {
            out.push_back(p);
        }
    }
    // Drop duplicate closing point if present
    if (out.size() >= 2 && nearPoint(out.front(), out.back(), eps)) {
        out.pop_back();
    }
    return out;
}

static double signedAreaLocal(const Polygon2D& poly) {
    if (poly.size() < 3) return 0.0;
    double area = 0.0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const auto& p1 = poly[i];
        const auto& p2 = poly[(i + 1) % poly.size()];
        area += (p1.x * p2.y - p2.x * p1.y);
    }
    return area / 2.0;
}

static bool isClockwiseLocal(const Polygon2D& poly) {
    return signedAreaLocal(poly) < 0.0;
}

static BgPolygon toBgPolygon(const Polygon2D& in) {
    BgPolygon poly;
    auto cleaned = sanitizePolygon2D(in);
    for (const auto& p : cleaned) {
        poly.outer().push_back(BgPoint(p.x, p.y));
    }
    bg::correct(poly);
    return poly;
}

static BgPolygon toBgPolygon(const Obstacle2D& obs) {
    BgPolygon poly;
    auto outer_clean = sanitizePolygon2D(obs.outer);
    for (const auto& p : outer_clean) {
        poly.outer().push_back(BgPoint(p.x, p.y));
    }
    for (const auto& hole : obs.holes) {
        auto hole_clean = sanitizePolygon2D(hole);
        if (hole_clean.size() < 3) {
            continue;
        }
        poly.inners().emplace_back();
        auto& inner = poly.inners().back();
        for (const auto& p : hole_clean) {
            inner.push_back(BgPoint(p.x, p.y));
        }
    }
    bg::correct(poly);
    return poly;
}

static bool bgValidate(const BgPolygon& poly, std::string& reason) {
    bg::validity_failure_type failure;
    if (!bg::is_valid(poly, failure)) {
        reason = bg::validity_failure_type_message(failure);
        return false;
    }
    // Boost considers some degenerate polygons "valid" depending on failure modes; guard area too.
    if (std::fabs(bg::area(poly)) <= kMinValidArea) {
        reason = "area is too small";
        return false;
    }
    return true;
}

template <typename RingT>
static Polygon2D bgRingToPolygon2D(const RingT& ring) {
    Polygon2D out;
    out.reserve(ring.size());
    for (const auto& pt : ring) {
        out.emplace_back(bg::get<0>(pt), bg::get<1>(pt));
    }
    // Boost rings are closed; drop trailing duplicate for our representation.
    if (out.size() >= 2 && nearPoint(out.front(), out.back())) {
        out.pop_back();
    }
    return out;
}

static BgMultiPolygon bgIntersection(const BgPolygon& a, const BgPolygon& b) {
    BgMultiPolygon out;
    bg::intersection(a, b, out);
    for (auto& p : out) {
        bg::correct(p);
    }
    return out;
}

static BgMultiPolygon bgDifference(const BgMultiPolygon& in, const BgPolygon& sub) {
    BgMultiPolygon out;
    for (const auto& p : in) {
        BgMultiPolygon tmp;
        bg::difference(p, sub, tmp);
        for (auto& t : tmp) {
            bg::correct(t);
            if (std::fabs(bg::area(t)) > kMinValidArea) {
                out.push_back(t);
            }
        }
    }
    return out;
}

static double pathLengthMeters(const std::vector<Point2D>& pts) {
    if (pts.size() < 2) return 0.0;
    double len = 0.0;
    for (size_t i = 1; i < pts.size(); ++i) {
        len += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
    }
    return len;
}

static PathStateList resamplePathStates(const PathStateList& in, double spacing_m) {
    if (spacing_m <= 0.0 || in.size() < 2) {
        return in;
    }

    // Dedupe consecutive duplicates and work in Point2D space
    std::vector<Point2D> pts;
    pts.reserve(in.size());
    for (const auto& st : in) {
        if (pts.empty() || std::hypot(st.point.x - pts.back().x, st.point.y - pts.back().y) > 1e-9) {
            pts.push_back(st.point);
        }
    }
    if (pts.size() < 2) {
        return in;
    }

    // Clamp spacing to avoid producing an excessive number of waypoints
    constexpr size_t kMaxWaypoints = 20000;
    double total_len = pathLengthMeters(pts);
    if (total_len > 0.0) {
        double min_spacing = total_len / std::max<double>(1.0, static_cast<double>(kMaxWaypoints - 1));
        if (spacing_m < min_spacing) {
            spacing_m = min_spacing;
        }
    }

    std::vector<Point2D> out_pts;
    out_pts.reserve(static_cast<size_t>(std::ceil(total_len / spacing_m)) + 2);
    out_pts.push_back(pts.front());

    double remaining = spacing_m;
    Point2D cur = pts.front();

    for (size_t i = 1; i < pts.size(); ++i) {
        Point2D next = pts[i];
        double seg_len = std::hypot(next.x - cur.x, next.y - cur.y);
        if (seg_len < 1e-12) {
            continue;
        }

        while (seg_len + 1e-12 >= remaining) {
            double t = remaining / seg_len;
            Point2D p{cur.x + t * (next.x - cur.x), cur.y + t * (next.y - cur.y)};
            out_pts.push_back(p);
            cur = p;
            seg_len = std::hypot(next.x - cur.x, next.y - cur.y);
            remaining = spacing_m;
            if (seg_len < 1e-12) {
                break;
            }
        }

        // Consume the remainder of this segment
        remaining -= seg_len;
        cur = next;
    }

    // Ensure we end exactly at the final point
    if (out_pts.empty() ||
        std::hypot(out_pts.back().x - pts.back().x, out_pts.back().y - pts.back().y) > 1e-6) {
        out_pts.push_back(pts.back());
    }

    // Convert back to PathStateList with headings and unit direction vectors
    PathStateList out;
    out.reserve(out_pts.size());
    for (size_t i = 0; i < out_pts.size(); ++i) {
        double heading = 0.0;
        if (i + 1 < out_pts.size()) {
            heading = std::atan2(out_pts[i + 1].y - out_pts[i].y, out_pts[i + 1].x - out_pts[i].x);
        } else if (!out.empty()) {
            heading = out.back().heading;
        }
        out.emplace_back(out_pts[i], heading);
    }
    return out;
}

#ifdef HAVE_FIELDS2COVER
static void addBgPolygonToF2CCells(const BgPolygon& poly, F2CCells& cells) {
    // Exterior ring (ensure CCW)
    Polygon2D outer = bgRingToPolygon2D(poly.outer());
    if (outer.size() < 3) return;
    if (isClockwiseLocal(outer)) {
        std::reverse(outer.begin(), outer.end());
    }
    F2CLinearRing outer_ring;
    for (const auto& p : outer) {
        outer_ring.addPoint(p.x, p.y);
    }

    F2CCell cell;
    cell.addRing(outer_ring);

    // Interior rings (holes) must be clockwise
    for (const auto& inner_bg : poly.inners()) {
        Polygon2D inner = bgRingToPolygon2D(inner_bg);
        if (inner.size() < 3) continue;
        if (!isClockwiseLocal(inner)) {
            std::reverse(inner.begin(), inner.end());
        }
        F2CLinearRing inner_ring;
        for (const auto& p : inner) {
            inner_ring.addPoint(p.x, p.y);
        }
        cell.addRing(inner_ring);
    }

    cells.addGeometry(cell);
}

static bool buildEffectiveCellsFromROIAndObstacles(
    const Polygon2D& boundary,
    const Polygon2D* roi,
    const std::vector<Obstacle2D>* obstacles,
    double obstacle_clearance,
    double min_region_area_m2,
    F2CCells& out_cells,
    Polygon2D& out_primary_outer,
    double& out_effective_area_m2,
    int& out_skipped_obstacles,
    int& out_region_count,
    double& out_uncovered_area_m2,
    std::vector<Obstacle2D>& out_free_space,
    std::string& error) {

    FreeSpaceResult fs = buildFreeSpacePolygons(boundary, roi, obstacles,
                                                obstacle_clearance, min_region_area_m2);
    out_skipped_obstacles = fs.skipped_obstacles;
    if (!fs.success) {
        error = fs.error_message;
        return false;
    }

    // Cover only the largest connected component. Obstacles interior to it stay
    // as holes (the router weaves around them); other substantial components are
    // genuinely unreachable without crossing an obstacle, so they are reported
    // as uncovered rather than stitched in with unsafe straight transits.
    double best_area = -1.0;
    const Obstacle2D* best = nullptr;
    for (const auto& reg : fs.regions) {
        const double a = std::fabs(signedAreaLocal(reg.outer));
        if (a > best_area) {
            best_area = a;
            best = &reg;
        }
    }
    if (!best) {
        error = "Effective area is empty";
        return false;
    }

    out_region_count = static_cast<int>(fs.regions.size());
    out_uncovered_area_m2 = std::max(0.0, fs.effective_area_m2 - best_area);
    out_effective_area_m2 = best_area;
    out_primary_outer = best->outer;
    out_free_space = {*best};

    out_cells = F2CCells();
    addBgPolygonToF2CCells(toBgPolygon(*best), out_cells);
    if (out_cells.size() == 0) {
        error = "Failed to build Fields2Cover cells from effective area";
        return false;
    }
    return true;
}
#endif  // HAVE_FIELDS2COVER

}  // namespace

// =============================================================================
// Point Cloud Processing
// =============================================================================

PointCloudPtr loadPointCloudFile(const std::string& path) {
    PointCloudPtr cloud(new PointCloud);
    
    // Determine file type by extension
    std::string ext = path.substr(path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    int result = -1;
    if (ext == "pcd") {
        result = pcl::io::loadPCDFile<pcl::PointXYZ>(path, *cloud);
    } else if (ext == "ply") {
        result = pcl::io::loadPLYFile<pcl::PointXYZ>(path, *cloud);
    } else if (ext == "xyz") {
        // Simple XYZ text format
        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open file: " + path);
        }
        double x, y, z;
        while (file >> x >> y >> z) {
            cloud->push_back(pcl::PointXYZ(x, y, z));
        }
        result = cloud->empty() ? -1 : 0;
    } else {
        throw std::runtime_error("Unsupported file format: " + ext);
    }
    
    if (result < 0 || cloud->empty()) {
        throw std::runtime_error("Failed to load point cloud: " + path);
    }
    
    return cloud;
}

PointCloudPtr filterByZBand(const PointCloudPtr& cloud, double z_band) {
    if (z_band <= 0 || cloud->empty()) {
        return cloud;
    }
    
    // Compute median Z
    std::vector<float> z_values;
    z_values.reserve(cloud->size());
    for (const auto& pt : cloud->points) {
        z_values.push_back(pt.z);
    }
    std::sort(z_values.begin(), z_values.end());
    float z_median = z_values[z_values.size() / 2];
    
    // Filter points within band
    PointCloudPtr filtered(new PointCloud);
    filtered->reserve(cloud->size());
    
    for (const auto& pt : cloud->points) {
        if (std::abs(pt.z - z_median) <= z_band) {
            filtered->push_back(pt);
        }
    }
    
    if (filtered->empty()) {
        throw std::runtime_error("Z-band filter removed all points");
    }
    
    return filtered;
}

PointCloudPtr filterByZRange(const PointCloudPtr& cloud, double z_min, double z_max) {
    if (cloud->empty()) {
        return cloud;
    }
    
    // Swap if min > max
    if (z_min > z_max) {
        std::swap(z_min, z_max);
    }
    
    // Filter points within [z_min, z_max] range (relative to origin Z=0)
    PointCloudPtr filtered(new PointCloud);
    filtered->reserve(cloud->size());
    
    for (const auto& pt : cloud->points) {
        if (pt.z >= z_min && pt.z <= z_max) {
            filtered->push_back(pt);
        }
    }
    
    if (filtered->empty()) {
        throw std::runtime_error("Z-range filter removed all points. Try adjusting Z min/max values.");
    }
    
    return filtered;
}

PointCloudPtr subsampleRandom(const PointCloudPtr& cloud, size_t max_points) {
    if (max_points == 0 || cloud->size() <= max_points) {
        return cloud;
    }
    
    std::vector<size_t> indices(cloud->size());
    std::iota(indices.begin(), indices.end(), 0);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(indices.begin(), indices.end(), gen);
    
    PointCloudPtr sampled(new PointCloud);
    sampled->reserve(max_points);
    for (size_t i = 0; i < max_points; ++i) {
        sampled->push_back(cloud->points[indices[i]]);
    }
    
    return sampled;
}

PointCloudPtr downsampleVoxel(const PointCloudPtr& cloud, double voxel_size) {
    if (voxel_size <= 0) {
        return cloud;
    }
    
    PointCloudPtr filtered(new PointCloud);
    pcl::VoxelGrid<pcl::PointXYZ> voxel_filter;
    voxel_filter.setInputCloud(cloud);
    voxel_filter.setLeafSize(voxel_size, voxel_size, voxel_size);
    voxel_filter.filter(*filtered);
    
    return filtered;
}

PointCloudPtr downsampleStatistical(const PointCloudPtr& cloud, int mean_k, double std_ratio) {
    if (mean_k <= 0 || std_ratio <= 0) {
        return cloud;
    }
    
    PointCloudPtr filtered(new PointCloud);
    pcl::StatisticalOutlierRemoval<pcl::PointXYZ> sor;
    sor.setInputCloud(cloud);
    sor.setMeanK(mean_k);
    sor.setStddevMulThresh(std_ratio);
    sor.filter(*filtered);
    
    return filtered;
}

// =============================================================================
// Geometry Operations
// =============================================================================

Polygon2D computeConvexHull(const std::vector<Point2D>& points) {
    if (points.size() < 3) {
        throw std::runtime_error("Need at least 3 points for convex hull");
    }
    
    // Simple Graham scan implementation
    std::vector<Point2D> sorted_points = points;
    
    // Find lowest point
    auto lowest = std::min_element(sorted_points.begin(), sorted_points.end(),
        [](const Point2D& a, const Point2D& b) {
            return (a.y < b.y) || (a.y == b.y && a.x < b.x);
        });
    std::swap(*sorted_points.begin(), *lowest);
    Point2D pivot = sorted_points[0];
    
    // Sort by polar angle
    std::sort(sorted_points.begin() + 1, sorted_points.end(),
        [&pivot](const Point2D& a, const Point2D& b) {
            double angle_a = std::atan2(a.y - pivot.y, a.x - pivot.x);
            double angle_b = std::atan2(b.y - pivot.y, b.x - pivot.x);
            return angle_a < angle_b;
        });
    
    // Build hull
    Polygon2D hull;
    for (const auto& p : sorted_points) {
        while (hull.size() >= 2) {
            Point2D& p1 = hull[hull.size() - 2];
            Point2D& p2 = hull[hull.size() - 1];
            double cross = (p2.x - p1.x) * (p.y - p1.y) - (p2.y - p1.y) * (p.x - p1.x);
            if (cross <= 0) {
                hull.pop_back();
            } else {
                break;
            }
        }
        hull.push_back(p);
    }
    
    return hull;
}

// Forward declarations for CGAL functions
#ifdef HAVE_CGAL
static Polygon2D computeAlphaShapeImpl(const std::vector<Point2D>& points, double alpha);
static Polygon2D computeDelaunayBoundaryImpl(const std::vector<Point2D>& points);
static Polygon2D computeGridBoundaryImpl(const std::vector<Point2D>& points, double grid_size);
#endif

Polygon2D computeConcaveHull(const std::vector<Point2D>& points, 
                             double alpha,
                             const std::string& method) {
    if (points.size() < 3) {
        throw std::runtime_error("Need at least 3 points for hull computation");
    }
    
    reportProgress(10, "Computing hull using " + method + "...");
    
    std::string m = method;
    std::transform(m.begin(), m.end(), m.begin(), ::tolower);
    
#ifdef HAVE_CGAL
    if (m == "alphashape" || m == "alpha") {
        return computeAlphaShapeImpl(points, alpha);
    } else if (m == "delaunay") {
        return computeDelaunayBoundaryImpl(points);
    } else if (m == "grid") {
        return computeGridBoundaryImpl(points, alpha);
    }
#endif
    
    // Fallback to convex hull
    reportProgress(50, "Using convex hull fallback");
    Polygon2D result = computeConvexHull(points);
    reportProgress(100, "Hull computed");
    
    return result;
}

#ifdef HAVE_CGAL
// CGAL type aliases (inside namespace)
using CGALKernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using CGALPoint = CGALKernel::Point_2;
using CGALVb = CGAL::Alpha_shape_vertex_base_2<CGALKernel>;
using CGALFb = CGAL::Alpha_shape_face_base_2<CGALKernel>;
using CGALTds = CGAL::Triangulation_data_structure_2<CGALVb, CGALFb>;
using CGALTriangulation = CGAL::Delaunay_triangulation_2<CGALKernel, CGALTds>;
using CGALAlphaShape = CGAL::Alpha_shape_2<CGALTriangulation>;

static Polygon2D computeAlphaShapeImpl(const std::vector<Point2D>& points, double alpha) {
    
    reportProgress(20, "Building alpha shape...");
    
    // Convert points to CGAL format
    std::vector<CGALPoint> cgal_points;
    cgal_points.reserve(points.size());
    for (const auto& p : points) {
        cgal_points.emplace_back(p.x, p.y);
    }
    
    // Build alpha shape - alpha parameter: smaller = more detail
    // CGAL uses alpha^2 internally, so we need to adjust
    double alpha_value = 1.0 / (alpha * alpha);  // Smaller alpha param = larger alpha_value = more detail
    
    CGALAlphaShape A(cgal_points.begin(), cgal_points.end(), alpha_value, CGALAlphaShape::GENERAL);
    
    reportProgress(50, "Extracting boundary...");
    
    // Extract boundary edges
    std::vector<std::pair<CGALPoint, CGALPoint>> edges;
    for (auto it = A.alpha_shape_edges_begin(); it != A.alpha_shape_edges_end(); ++it) {
        auto face = it->first;
        int i = it->second;
        CGALPoint p1 = face->vertex((i + 1) % 3)->point();
        CGALPoint p2 = face->vertex((i + 2) % 3)->point();
        edges.emplace_back(p1, p2);
    }
    
    if (edges.empty()) {
        reportProgress(100, "Alpha shape empty, using convex hull");
        return computeConvexHull(points);
    }
    
    reportProgress(70, "Ordering boundary points...");
    
    // Build adjacency map and order boundary
    std::map<std::pair<double, double>, std::vector<std::pair<double, double>>> adj;
    for (const auto& e : edges) {
        double x1 = CGAL::to_double(e.first.x());
        double y1 = CGAL::to_double(e.first.y());
        double x2 = CGAL::to_double(e.second.x());
        double y2 = CGAL::to_double(e.second.y());
        auto p1 = std::make_pair(x1, y1);
        auto p2 = std::make_pair(x2, y2);
        adj[p1].push_back(p2);
        adj[p2].push_back(p1);
    }
    
    // Find the longest connected boundary
    Polygon2D best_boundary;
    std::set<std::pair<double, double>> global_visited;
    
    for (const auto& start_pair : adj) {
        if (global_visited.count(start_pair.first)) continue;
        
        Polygon2D current_boundary;
        auto current = start_pair.first;
        std::set<std::pair<double, double>> local_visited;
        
        while (local_visited.find(current) == local_visited.end()) {
            local_visited.insert(current);
            global_visited.insert(current);
            current_boundary.emplace_back(current.first, current.second);
            
            bool found = false;
            for (const auto& next : adj[current]) {
                if (local_visited.find(next) == local_visited.end()) {
                    current = next;
                    found = true;
                    break;
                }
            }
            if (!found) break;
        }
        
        if (current_boundary.size() > best_boundary.size()) {
            best_boundary = current_boundary;
        }
    }
    
    if (best_boundary.size() < 3) {
        reportProgress(100, "Alpha boundary too small, using convex hull");
        return computeConvexHull(points);
    }
    
    reportProgress(100, "Alpha shape computed");
    return best_boundary;
}

static Polygon2D computeDelaunayBoundaryImpl(const std::vector<Point2D>& points) {
    
    reportProgress(20, "Building Delaunay triangulation...");
    
    // Convert points to CGAL format
    std::vector<CGALPoint> cgal_points;
    cgal_points.reserve(points.size());
    for (const auto& p : points) {
        cgal_points.emplace_back(p.x, p.y);
    }
    
    // Build Delaunay triangulation
    CGALTriangulation dt(cgal_points.begin(), cgal_points.end());
    
    reportProgress(50, "Finding boundary edges...");
    
    // Count edge occurrences - boundary edges appear once
    std::map<std::pair<std::pair<double,double>, std::pair<double,double>>, int> edge_count;
    
    for (auto fit = dt.finite_faces_begin(); fit != dt.finite_faces_end(); ++fit) {
        for (int i = 0; i < 3; ++i) {
            auto p1 = fit->vertex((i + 1) % 3)->point();
            auto p2 = fit->vertex((i + 2) % 3)->point();
            
            double x1 = CGAL::to_double(p1.x());
            double y1 = CGAL::to_double(p1.y());
            double x2 = CGAL::to_double(p2.x());
            double y2 = CGAL::to_double(p2.y());
            
            auto key1 = std::make_pair(x1, y1);
            auto key2 = std::make_pair(x2, y2);
            
            if (key1 > key2) std::swap(key1, key2);
            auto edge_key = std::make_pair(key1, key2);
            edge_count[edge_key]++;
        }
    }
    
    // Extract boundary edges (those that appear only once)
    std::map<std::pair<double, double>, std::vector<std::pair<double, double>>> adj;
    for (const auto& ec : edge_count) {
        if (ec.second == 1) {
            adj[ec.first.first].push_back(ec.first.second);
            adj[ec.first.second].push_back(ec.first.first);
        }
    }
    
    if (adj.empty()) {
        return computeConvexHull(points);
    }
    
    reportProgress(70, "Ordering boundary...");
    
    // Order boundary vertices
    Polygon2D boundary;
    auto start = adj.begin()->first;
    auto current = start;
    std::set<std::pair<double, double>> visited;
    
    while (visited.find(current) == visited.end()) {
        visited.insert(current);
        boundary.emplace_back(current.first, current.second);
        
        bool found = false;
        for (const auto& next : adj[current]) {
            if (visited.find(next) == visited.end()) {
                current = next;
                found = true;
                break;
            }
        }
        if (!found) break;
    }
    
    reportProgress(100, "Delaunay boundary computed");
    
    if (boundary.size() < 3) {
        return computeConvexHull(points);
    }
    
    return boundary;
}

static Polygon2D computeGridBoundaryImpl(const std::vector<Point2D>& points, double grid_size) {
    reportProgress(20, "Computing grid boundary...");
    
    if (points.size() < 3) {
        return computeConvexHull(points);
    }
    
    // Find bounds
    double min_x = points[0].x, max_x = points[0].x;
    double min_y = points[0].y, max_y = points[0].y;
    for (const auto& p : points) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
    }
    
    // Auto grid size if needed
    if (grid_size <= 0) {
        double range = std::max(max_x - min_x, max_y - min_y);
        grid_size = range / 100.0;
    }
    
    // Add padding
    double padding = grid_size * 2;
    min_x -= padding; min_y -= padding;
    max_x += padding; max_y += padding;
    
    int grid_w = static_cast<int>((max_x - min_x) / grid_size) + 1;
    int grid_h = static_cast<int>((max_y - min_y) / grid_size) + 1;
    
    reportProgress(40, "Building occupancy grid...");
    
    // Build occupancy grid
    std::vector<std::vector<bool>> grid(grid_h, std::vector<bool>(grid_w, false));
    for (const auto& p : points) {
        int xi = static_cast<int>((p.x - min_x) / grid_size);
        int yi = static_cast<int>((p.y - min_y) / grid_size);
        xi = std::clamp(xi, 0, grid_w - 1);
        yi = std::clamp(yi, 0, grid_h - 1);
        grid[yi][xi] = true;
        // Also fill neighbors for robustness
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = xi + dx, ny = yi + dy;
                if (nx >= 0 && nx < grid_w && ny >= 0 && ny < grid_h) {
                    grid[ny][nx] = true;
                }
            }
        }
    }
    
    reportProgress(60, "Finding boundary cells...");
    
    // Find boundary cells (occupied cells with at least one empty neighbor)
    std::vector<Point2D> boundary_points;
    for (int y = 0; y < grid_h; ++y) {
        for (int x = 0; x < grid_w; ++x) {
            if (grid[y][x]) {
                bool is_boundary = false;
                for (int dy = -1; dy <= 1 && !is_boundary; ++dy) {
                    for (int dx = -1; dx <= 1 && !is_boundary; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || nx >= grid_w || ny < 0 || ny >= grid_h || !grid[ny][nx]) {
                            is_boundary = true;
                        }
                    }
                }
                if (is_boundary) {
                    boundary_points.emplace_back(min_x + x * grid_size, min_y + y * grid_size);
                }
            }
        }
    }
    
    if (boundary_points.size() < 3) {
        return computeConvexHull(points);
    }
    
    reportProgress(80, "Computing boundary hull...");
    
    // Use convex hull of boundary points
    Polygon2D result = computeConvexHull(boundary_points);
    
    reportProgress(100, "Grid boundary computed");
    return result;
}

#endif // HAVE_CGAL

Polygon2D simplifyPolygon(const Polygon2D& poly, double tolerance) {
    if (tolerance <= 0 || poly.size() < 3) {
        return poly;
    }
    
    // Douglas-Peucker algorithm
    std::function<void(int, int, std::vector<bool>&)> douglasPeucker;
    douglasPeucker = [&](int start, int end, std::vector<bool>& keep) {
        if (end - start < 2) return;
        
        double max_dist = 0;
        int max_idx = start;
        
        const Point2D& p1 = poly[start];
        const Point2D& p2 = poly[end];
        double dx = p2.x - p1.x;
        double dy = p2.y - p1.y;
        double len_sq = dx * dx + dy * dy;
        
        for (int i = start + 1; i < end; ++i) {
            double dist;
            if (len_sq < 1e-10) {
                dist = std::hypot(poly[i].x - p1.x, poly[i].y - p1.y);
            } else {
                double t = ((poly[i].x - p1.x) * dx + (poly[i].y - p1.y) * dy) / len_sq;
                t = std::max(0.0, std::min(1.0, t));
                double proj_x = p1.x + t * dx;
                double proj_y = p1.y + t * dy;
                dist = std::hypot(poly[i].x - proj_x, poly[i].y - proj_y);
            }
            
            if (dist > max_dist) {
                max_dist = dist;
                max_idx = i;
            }
        }
        
        if (max_dist > tolerance) {
            keep[max_idx] = true;
            douglasPeucker(start, max_idx, keep);
            douglasPeucker(max_idx, end, keep);
        }
    };
    
    std::vector<bool> keep(poly.size(), false);
    keep[0] = true;
    keep[poly.size() - 1] = true;
    douglasPeucker(0, poly.size() - 1, keep);
    
    Polygon2D simplified;
    for (size_t i = 0; i < poly.size(); ++i) {
        if (keep[i]) {
            simplified.push_back(poly[i]);
        }
    }
    
    return simplified;
}

double computeSwathAngle(const Polygon2D& poly) {
    if (poly.size() < 3) return 0.0;
    
    double best_angle = 0.0;
    double min_area = std::numeric_limits<double>::max();
    
    // Try angles from each edge
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point2D& p1 = poly[i];
        const Point2D& p2 = poly[(i + 1) % poly.size()];
        
        double angle = std::atan2(p2.y - p1.y, p2.x - p1.x);
        
        // Rotate points and find bounding box
        double cos_a = std::cos(-angle);
        double sin_a = std::sin(-angle);
        
        double min_x = std::numeric_limits<double>::max();
        double max_x = std::numeric_limits<double>::lowest();
        double min_y = std::numeric_limits<double>::max();
        double max_y = std::numeric_limits<double>::lowest();
        
        for (const auto& p : poly) {
            double rx = p.x * cos_a - p.y * sin_a;
            double ry = p.x * sin_a + p.y * cos_a;
            min_x = std::min(min_x, rx);
            max_x = std::max(max_x, rx);
            min_y = std::min(min_y, ry);
            max_y = std::max(max_y, ry);
        }
        
        double area = (max_x - min_x) * (max_y - min_y);
        double width = max_x - min_x;
        double height = max_y - min_y;
        
        if (area < min_area) {
            min_area = area;
            best_angle = (width > height) ? angle : angle + M_PI / 2.0;
        }
    }
    
    // Normalize to [0, pi)
    while (best_angle < 0) best_angle += M_PI;
    while (best_angle >= M_PI) best_angle -= M_PI;
    
    return best_angle;
}

bool isPolygonConcave(const Polygon2D& poly) {
    if (poly.size() < 3) return false;
    
    bool has_positive = false;
    bool has_negative = false;
    
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point2D& p1 = poly[i];
        const Point2D& p2 = poly[(i + 1) % poly.size()];
        const Point2D& p3 = poly[(i + 2) % poly.size()];
        
        double cross = (p2.x - p1.x) * (p3.y - p1.y) - (p2.y - p1.y) * (p3.x - p1.x);
        
        if (cross > 1e-10) has_positive = true;
        if (cross < -1e-10) has_negative = true;
    }
    
    return has_positive && has_negative;
}

double polygonArea(const Polygon2D& poly) {
    if (poly.size() < 3) return 0.0;
    
    double area = 0.0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point2D& p1 = poly[i];
        const Point2D& p2 = poly[(i + 1) % poly.size()];
        area += (p1.x * p2.y - p2.x * p1.y);
    }
    
    return std::abs(area) / 2.0;
}

bool isPolygonValid(const Polygon2D& poly) {
    if (poly.size() < 3) return false;
    
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point2D& p1 = poly[i];
        const Point2D& p2 = poly[(i + 1) % poly.size()];
        if (std::hypot(p2.x - p1.x, p2.y - p1.y) < 1e-10) {
            return false;
        }
    }
    
    return polygonArea(poly) > 1e-10;
}

void polygonBounds(const Polygon2D& poly, 
                   double& min_x, double& min_y, 
                   double& max_x, double& max_y) {
    if (poly.empty()) {
        min_x = min_y = max_x = max_y = 0;
        return;
    }
    
    min_x = max_x = poly[0].x;
    min_y = max_y = poly[0].y;
    
    for (const auto& p : poly) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
    }
}

// clipObstacleToPolygon + buildFreeSpacePolygons live in obstacle_geometry.cpp
// (boost-only core, unit-tested standalone).

// =============================================================================
// Coverage Generation
// =============================================================================

PathStateList swathsToAxialTurnPath(const SwathList& swaths) {
    PathStateList path;
    
    for (const auto& swath : swaths) {
        double heading = swath.heading;
        double vx = std::cos(heading);
        double vy = std::sin(heading);
        
        PathState start_state;
        start_state.point = swath.start;
        start_state.heading = heading;
        start_state.vx = vx;
        start_state.vy = vy;
        path.push_back(start_state);
        
        PathState end_state;
        end_state.point = swath.end;
        end_state.heading = heading;
        end_state.vx = vx;
        end_state.vy = vy;
        path.push_back(end_state);
    }
    
    return path;
}

// Helper: Calculate signed area of polygon (positive = CCW, negative = CW)
static double signedPolygonArea(const Polygon2D& poly) {
    if (poly.size() < 3) return 0.0;
    double area = 0.0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point2D& p1 = poly[i];
        const Point2D& p2 = poly[(i + 1) % poly.size()];
        area += (p1.x * p2.y - p2.x * p1.y);
    }
    return area / 2.0;
}

// Helper: Check if polygon is clockwise (for interior ring)
static bool isClockwise(const Polygon2D& poly) {
    return signedPolygonArea(poly) < 0;
}

// Helper: Check if point is inside polygon (ray casting)
static bool pointInPolygon(const Point2D& p, const Polygon2D& poly) {
    if (poly.size() < 3) return false;
    
    int crossings = 0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point2D& p1 = poly[i];
        const Point2D& p2 = poly[(i + 1) % poly.size()];
        
        if ((p1.y <= p.y && p2.y > p.y) || (p2.y <= p.y && p1.y > p.y)) {
            double x_intersect = p1.x + (p.y - p1.y) / (p2.y - p1.y) * (p2.x - p1.x);
            if (p.x < x_intersect) {
                crossings++;
            }
        }
    }
    return (crossings % 2) == 1;
}

// Helper: Check if line segment intersects polygon edge
static bool segmentIntersectsPolygon(const Point2D& a, const Point2D& b, const Polygon2D& poly) {
    // Check if either endpoint is inside the polygon
    if (pointInPolygon(a, poly) || pointInPolygon(b, poly)) {
        return true;
    }
    
    // Check if line segment crosses any edge
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point2D& p1 = poly[i];
        const Point2D& p2 = poly[(i + 1) % poly.size()];
        
        // Line segment intersection test
        double d1x = b.x - a.x, d1y = b.y - a.y;
        double d2x = p2.x - p1.x, d2y = p2.y - p1.y;
        double cross = d1x * d2y - d1y * d2x;
        
        if (std::abs(cross) < 1e-10) continue;  // Parallel
        
        double t = ((p1.x - a.x) * d2y - (p1.y - a.y) * d2x) / cross;
        double u = ((p1.x - a.x) * d1y - (p1.y - a.y) * d1x) / cross;
        
        if (t >= 0 && t <= 1 && u >= 0 && u <= 1) {
            return true;
        }
    }
    return false;
}

// Filter path to avoid obstacles - removes points inside obstacles
// and adds intermediate points to go around them
static PathStateList filterPathAroundObstacles(
    const PathStateList& original_path,
    const std::vector<Obstacle2D>& obstacles) {
    
    if (obstacles.empty() || original_path.empty()) {
        return original_path;
    }
    
    PathStateList filtered;
    filtered.reserve(original_path.size());
    
    for (size_t i = 0; i < original_path.size(); ++i) {
        const PathState& state = original_path[i];
        
        // Check if this point is inside any obstacle
        bool inside_obstacle = false;
        for (const auto& obs : obstacles) {
            if (pointInPolygon(state.point, obs.outer)) {
                bool in_hole = false;
                for (const auto& hole : obs.holes) {
                    if (pointInPolygon(state.point, hole)) {
                        in_hole = true;
                        break;
                    }
                }
                if (!in_hole) {
                inside_obstacle = true;
                break;
                }
            }
        }
        
        if (!inside_obstacle) {
            // Check if segment from previous point crosses obstacle
            if (!filtered.empty()) {
                const Point2D& prev = filtered.back().point;
                bool crosses_obstacle = false;
                
                for (const auto& obs : obstacles) {
                    if (segmentIntersectsPolygon(prev, state.point, obs.outer)) {
                        crosses_obstacle = true;
                        break;
                    }
                }
                
                if (crosses_obstacle) {
                    // Skip this point - it creates a path through obstacle
                    // The axial turn path will be used as fallback
                    continue;
                }
            }
            filtered.push_back(state);
        }
    }
    
    return filtered;
}

#ifdef HAVE_FIELDS2COVER

// Recompute per-waypoint headings/velocity from consecutive points (axial model).
static void recomputeAxialHeadings(PathStateList& path) {
    for (size_t i = 0; i < path.size(); ++i) {
        double heading;
        if (i + 1 < path.size()) {
            heading = std::atan2(path[i + 1].point.y - path[i].point.y,
                                 path[i + 1].point.x - path[i].point.x);
        } else if (i > 0) {
            heading = path[i - 1].heading;
        } else {
            heading = 0.0;
        }
        path[i].heading = heading;
        path[i].vx = std::cos(heading);
        path[i].vy = std::sin(heading);
    }
}

// Re-route only the obstacle-crossing connectors of an existing route, leaving
// its boustrophedon ordering and every clear segment untouched. Per segment:
// a grid line-of-sight check keeps clear connectors straight (the common case,
// O(segment)); a breached one is routed via JPS and spliced in; an unreachable
// pair stays a straight transit that the Layer-2 gate flags. Order never changes.
static PathStateList buildObstacleAwarePath(const PathStateList& route,
                                            const FreeSpaceConnectorRouter& router,
                                            int& unroutable_out) {
    PathStateList path;
    unroutable_out = 0;
    if (route.empty()) return path;

    int routed = 0, breached = 0, unroutable = 0;
    const auto t0 = std::chrono::steady_clock::now();

    path.push_back(route.front());
    for (size_t i = 1; i < route.size(); ++i) {
        const Point2D a = route[i - 1].point;
        const Point2D b = route[i].point;

        // route() returns {a,b} when direct LOS is clear, intermediate corner
        // vertices when it had to go around an obstacle (piecewise-straight,
        // sharp corners), or empty when no safe connector exists.
        const std::vector<Point2D> connector = router.route(a, b);
        if (connector.size() > 2) {
            ++breached;
            ++routed;
            for (size_t k = 1; k + 1 < connector.size(); ++k) {
                path.push_back(PathState{connector[k], 0.0});
            }
        } else if (connector.empty()) {
            // No safe route (endpoints in disconnected free-space components).
            // Fall back to the straight segment; Layer-2 will flag the breach.
            ++breached;
            ++unroutable;
        }
        path.push_back(PathState{b, 0.0});
    }
    recomputeAxialHeadings(path);

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::cerr << "[Coverage] Connector routing: " << (route.size() - 1)
              << " segments, " << breached << " breached, " << routed
              << " rerouted, " << unroutable << " unroutable in " << ms << " ms\n";
    unroutable_out = unroutable;
    return path;
}

CoverageResult generateCoverage(const Polygon2D& boundary,
                                const CoverageConfig& config,
                                const Polygon2D* roi,
                                const std::vector<Obstacle2D>* obstacles) {
    CoverageResult result;
    
    try {
        reportProgress(5, "Building field...");

        // Build robust effective geometry: (boundary ∩ ROI) − obstacles
        reportProgress(8, "Clipping ROI / obstacles...");
        F2CCells cells;
        Polygon2D effective_outer;
        double effective_area_m2 = 0.0;
        std::string geom_error;
        int skipped_obstacles = 0;
        int region_count = 1;
        double uncovered_area_m2 = 0.0;
        std::vector<Obstacle2D> free_space_regions;
        if (!buildEffectiveCellsFromROIAndObstacles(
                boundary, roi, obstacles, config.obstacle_clearance,
                config.min_coverage_region_area_m2, cells, effective_outer,
                effective_area_m2, skipped_obstacles, region_count,
                uncovered_area_m2, free_space_regions, geom_error)) {
            result.error_message = geom_error;
            return result;
        }
        result.effective_area_m2 = effective_area_m2;
        result.skipped_obstacles = skipped_obstacles;
        result.free_space_regions = region_count;
        result.uncovered_area_m2 = uncovered_area_m2;

        F2CField field(cells);

        // Compute swath direction early (also used as a sensible decomposition split angle)
        double direction = M_PI;
        if (config.auto_align) {
            double base_angle = computeSwathAngle(effective_outer);
            direction = (config.align_mode == "long") ? base_angle : base_angle + M_PI / 2.0;
        }

        // Optional decomposition for concave fields (and/or fields with obstacles)
        if (config.use_decomposition) {
            bool is_concave = isPolygonConcave(effective_outer);
            bool has_obstacles = (obstacles && !obstacles->empty());
            if (is_concave || has_obstacles || cells.size() > 1) {
                reportProgress(10, "Decomposing field...");
                try {
                    if (config.decomposition_type == "trapezoidal") {
                        f2c::decomp::TrapezoidalDecomp decomp;
                        decomp.setSplitAngle(direction);
                        auto decomposed = decomp.decompose(cells);
                        if (decomposed.size() > 0) {
                            cells = decomposed;
                            field = F2CField(cells);
                        }
                    } else {
                        // Default to boustrophedon decomposition
                        f2c::decomp::BoustrophedonDecomp decomp;
                        decomp.setSplitAngle(direction);
                        auto decomposed = decomp.decompose(cells);
                        if (decomposed.size() > 0) {
                            cells = decomposed;
                            field = F2CField(cells);
                        }
                    }
                } catch (...) {
                    // Keep original cells if decomposition fails
                }
            }
        }
        
        // Generate headlands
        reportProgress(15, "Generating headlands...");
        F2CCells working_area = cells;
        if (config.headland_width > 0) {
            try {
                f2c::hg::ConstHL hl_gen;
                auto cropped = hl_gen.generateHeadlands(cells, config.headland_width);
                if (cropped.size() > 0) {
                    working_area = cropped;
                }
            } catch (...) {
                // Keep original cells if headland generation fails
            }
        }
        
        // Generate swaths
        reportProgress(30, "Generating swaths...");
        f2c::sg::BruteForce sg;

        // Generate swaths per cell (critical for decomposed/multi-cell fields)
        F2CSwathsByCells swaths_by_cells_raw;
        for (size_t i = 0; i < working_area.size(); ++i) {
            try {
                auto part = sg.generateSwaths(direction, config.swath_width, working_area.getGeometry(i));
                swaths_by_cells_raw.push_back(part);
            } catch (...) {
                // If one cell fails, skip it
                swaths_by_cells_raw.push_back(F2CSwaths());
            }
        }

        // Flatten swaths into our format for visualization/stats
        size_t total_swaths = 0;
        for (size_t i = 0; i < swaths_by_cells_raw.size(); ++i) {
            total_swaths += swaths_by_cells_raw.at(i).size();
        }
        if (total_swaths == 0) {
            result.error_message = "No swaths generated";
            return result;
        }
        
        for (size_t ci = 0; ci < swaths_by_cells_raw.size(); ++ci) {
            auto& cell_swaths = swaths_by_cells_raw.at(ci);
            for (size_t si = 0; si < cell_swaths.size(); ++si) {
                auto& sw = cell_swaths.at(si);
                Swath swath;
                swath.start = Point2D(sw.startPoint().getX(), sw.startPoint().getY());
                swath.end = Point2D(sw.endPoint().getX(), sw.endPoint().getY());
                swath.heading = std::atan2(swath.end.y - swath.start.y, swath.end.x - swath.start.x);
                result.swaths.push_back(swath);
            }
        }
        
        // Generate route
        reportProgress(50, "Generating route...");
        try {
            // Sort swaths within each cell according to selected pattern
            F2CSwathsByCells swaths_by_cells_sorted;

            for (size_t ci = 0; ci < swaths_by_cells_raw.size(); ++ci) {
                const auto& cell_swaths = swaths_by_cells_raw.at(ci);
                if (cell_swaths.size() == 0) {
                    swaths_by_cells_sorted.push_back(F2CSwaths());
                    continue;
                }

                std::string pattern = config.route_pattern;
                std::transform(pattern.begin(), pattern.end(), pattern.begin(), ::tolower);

                F2CSwaths sorted_swaths = cell_swaths;
                if (pattern == "snake") {
                    f2c::rp::SnakeOrder sorter;
                    sorted_swaths = sorter.genSortedSwaths(cell_swaths);
                } else if (pattern == "spiral") {
                    f2c::rp::SpiralOrder sorter;
                    sorted_swaths = sorter.genSortedSwaths(cell_swaths);
                } else {
                    // Default: boustrophedon
                    f2c::rp::BoustrophedonOrder sorter;
                    sorted_swaths = sorter.genSortedSwaths(cell_swaths);
                }
                swaths_by_cells_sorted.push_back(sorted_swaths);
            }

            f2c::rp::RoutePlannerBase route_planner;
            if (config.start_point.has_value()) {
                route_planner.setStartAndEndPoint(
                    F2CPoint(config.start_point->x, config.start_point->y));
            }
            F2CRoute f2c_route = route_planner.genRoute(working_area, swaths_by_cells_sorted);
            
            // Update swaths with sorted order
            result.swaths.clear();
            for (size_t ci = 0; ci < swaths_by_cells_sorted.size(); ++ci) {
                auto& cell_swaths = swaths_by_cells_sorted.at(ci);
                for (size_t si = 0; si < cell_swaths.size(); ++si) {
                    auto& sw = cell_swaths.at(si);
                    Swath swath;
                    swath.start = Point2D(sw.startPoint().getX(), sw.startPoint().getY());
                    swath.end = Point2D(sw.endPoint().getX(), sw.endPoint().getY());
                    swath.heading = std::atan2(swath.end.y - swath.start.y, swath.end.x - swath.start.x);
                    result.swaths.push_back(swath);
                }
            }
            
            // Extract route waypoints
            auto route_line = f2c_route.asLineString();
            for (size_t i = 0; i < route_line.size(); ++i) {
                PathState state;
                state.point = Point2D(route_line.getX(i), route_line.getY(i));
                result.route.push_back(state);
            }
            
            // Generate path
            reportProgress(70, "Generating path...");
            
            // When obstacles are present, use axial turns because:
            // 1. Dubins/smooth curves may cross obstacles during turns
            // 2. Swaths already avoid obstacles (via interior rings)
            // 3. Axial turns go directly between swath endpoints
            bool use_axial = config.use_axial_turns || (obstacles && !obstacles->empty());
            
            if (use_axial) {
                // Layer 1: with obstacles, reroute each breached inter-swath
                // connector around the sharp-cornered (miter-offset) obstacle
                // free space, keeping every connector a piecewise-straight leg
                // with sharp corners (no arc bowing). The visibility graph is
                // built once and reused across all connectors.
                std::unique_ptr<FreeSpaceConnectorRouter> connector_router;
                if (obstacles && !obstacles->empty() && !result.route.empty()) {
                    FreeSpaceResult fs = buildFreeSpacePolygons(
                        boundary, roi, obstacles, config.obstacle_clearance,
                        config.min_coverage_region_area_m2, /*sharp_corners=*/true);
                    if (fs.success && !fs.regions.empty()) {
                        connector_router = std::make_unique<FreeSpaceConnectorRouter>(
                            fs.regions, /*simplify_tol_m=*/0.10);
                    }
                }
                if (connector_router && connector_router->valid()) {
                    int unroutable = 0;
                    result.path = buildObstacleAwarePath(result.route, *connector_router,
                                                         unroutable);
                    result.connector_unroutable = unroutable;
                } else if (!result.route.empty()) {
                    // Route waypoints are already in correct order - use them as path
                    result.path = result.route;
                    
                    // Calculate headings between consecutive waypoints
                    for (size_t i = 0; i < result.path.size(); ++i) {
                        double heading;
                        if (i + 1 < result.path.size()) {
                            // Use direction to next point
                            double dx = result.path[i+1].point.x - result.path[i].point.x;
                            double dy = result.path[i+1].point.y - result.path[i].point.y;
                            heading = std::atan2(dy, dx);
                        } else if (i > 0) {
                            // Last point - use same heading as arrival
                            heading = result.path[i-1].heading;
                        } else {
                            heading = 0;
                        }
                        result.path[i].heading = heading;
                        result.path[i].vx = std::cos(heading);
                        result.path[i].vy = std::sin(heading);
                    }
                } else {
                    // Fallback: use stored swaths
                    result.path = swathsToAxialTurnPath(result.swaths);
                }
            } else {
                // Use path planner based on config
                // "none" means straight lines (use route directly like axial turns)
                if (config.path_planner == "none") {
                    // Straight path - use route waypoints directly
                    if (!result.route.empty()) {
                        result.path = result.route;
                        // Calculate headings between consecutive waypoints
                        for (size_t i = 0; i < result.path.size(); ++i) {
                            double heading;
                            if (i + 1 < result.path.size()) {
                                double dx = result.path[i+1].point.x - result.path[i].point.x;
                                double dy = result.path[i+1].point.y - result.path[i].point.y;
                                heading = std::atan2(dy, dx);
                            } else if (i > 0) {
                                heading = result.path[i-1].heading;
                            } else {
                                heading = 0;
                            }
                            result.path[i].heading = heading;
                            result.path[i].vx = std::cos(heading);
                            result.path[i].vy = std::sin(heading);
                        }
                    } else {
                        result.path = swathsToAxialTurnPath(result.swaths);
                    }
                } else {
                    // Use smooth curves (Dubins or Reeds-Shepp)
                    try {
                        F2CRobot robot(config.swath_width, config.swath_width);
                        robot.setMinTurningRadius(config.turn_radius);
                        
                        f2c::pp::PathPlanning pp;
                        F2CPath f2c_path;
                        
                        // Select path planner based on config
                        if (config.path_planner == "dubins") {
                            f2c::pp::DubinsCurves planner;
                            f2c_path = pp.planPath(robot, f2c_route, planner);
                        } else if (config.path_planner == "dubins_cc") {
                            f2c::pp::DubinsCurvesCC planner;
                            f2c_path = pp.planPath(robot, f2c_route, planner);
                        } else if (config.path_planner == "reeds") {
                            f2c::pp::ReedsSheppCurves planner;
                            f2c_path = pp.planPath(robot, f2c_route, planner);
                        } else if (config.path_planner == "reeds_hc") {
                            f2c::pp::ReedsSheppCurvesHC planner;
                            f2c_path = pp.planPath(robot, f2c_route, planner);
                        } else {
                            // Default to Dubins
                            f2c::pp::DubinsCurves planner;
                            f2c_path = pp.planPath(robot, f2c_route, planner);
                        }
                        
                        // Extract path states from F2C path
                        for (size_t i = 0; i < f2c_path.size(); ++i) {
                            auto state = f2c_path.getState(i);
                            PathState ps;
                            ps.point = Point2D(state.point.getX(), state.point.getY());
                            ps.heading = state.angle;
                            ps.vx = std::cos(ps.heading);
                            ps.vy = std::sin(ps.heading);
                            result.path.push_back(ps);
                        }
                        
                        // IMPORTANT: Ensure path ends at the last route waypoint
                        // Smooth curve planners may not sample exactly at endpoints
                        if (!result.path.empty() && !result.route.empty()) {
                            const auto& last_route = result.route.back();
                            const auto& last_path = result.path.back();
                            double dx = last_route.point.x - last_path.point.x;
                            double dy = last_route.point.y - last_path.point.y;
                            double dist = std::sqrt(dx*dx + dy*dy);
                            
                            // If last path point differs from last route point by > 1cm, append it
                            if (dist > 0.01) {
                                PathState end_state;
                                end_state.point = last_route.point;
                                // Use heading from last path point
                                end_state.heading = last_path.heading;
                                end_state.vx = std::cos(end_state.heading);
                                end_state.vy = std::sin(end_state.heading);
                                result.path.push_back(end_state);
                            }
                        }
                        
                    } catch (const std::exception& e) {
                        std::cerr << "Path planning warning: " << e.what() << std::endl;
                        result.path = swathsToAxialTurnPath(result.swaths);
                    }
                }
            }

            // Optional controller-facing polish: resample path to fixed spacing
            if (config.waypoint_spacing > 0.0 && !result.path.empty()) {
                reportProgress(85, "Resampling path...");
                result.path = resamplePathStates(result.path, config.waypoint_spacing);
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Route generation warning: " << e.what() << std::endl;
        }

        // Layer-2 safety gate: confirm no path segment drives into an obstacle.
        // Planning already keeps the path obstacle_clearance away, and the
        // Layer-1 router intentionally hugs that clearance boundary when going
        // around holes. Validating at the SAME clearance would flag that
        // legitimate boundary-hugging as a breach, so the gate checks against a
        // reduced clearance: it still catches a path eating into the safety
        // margin / driving over an obstacle, but tolerates headland-hugging.
        if (obstacles && !obstacles->empty() && !result.path.empty()) {
            std::vector<Point2D> pts;
            pts.reserve(result.path.size());
            for (const auto& st : result.path) pts.push_back(st.point);
            const double validation_clearance = config.obstacle_clearance * 0.5;
            PathValidation pv =
                validatePathClearsObstacles(pts, obstacles, validation_clearance);
            result.path_valid = pv.valid;
            if (!pv.valid) {
                std::cerr << "[Coverage] Path crosses obstacles: "
                          << pv.crossing_segments << " segment(s), "
                          << pv.breach_length_m << " m through clearance\n";
            }
        }

        reportProgress(100, "Coverage generation complete");
        result.success = true;
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
    }
    
    return result;
}

#else // No Fields2Cover

CoverageResult generateCoverage(const Polygon2D& boundary,
                                const CoverageConfig& config,
                                const Polygon2D* roi,
                                const std::vector<Obstacle2D>* obstacles) {
    CoverageResult result;
    
    // Note: obstacles are not handled in the simple fallback version
    (void)obstacles;
    
    reportProgress(5, "Generating simple coverage...");
    
    // Simple coverage without F2C - generate parallel swaths
    const Polygon2D& effective_poly = (roi && !roi->empty()) ? *roi : boundary;
    
    double min_x, min_y, max_x, max_y;
    polygonBounds(effective_poly, min_x, min_y, max_x, max_y);
    
    // Add headland
    min_x += config.headland_width;
    min_y += config.headland_width;
    max_x -= config.headland_width;
    max_y -= config.headland_width;
    
    if (min_x >= max_x || min_y >= max_y) {
        result.error_message = "Area too small after headland";
        return result;
    }
    
    // Compute direction
    double direction = M_PI / 2.0;  // Default: vertical
    if (config.auto_align) {
        double base_angle = computeSwathAngle(effective_poly);
        direction = (config.align_mode == "long") ? base_angle : base_angle + M_PI / 2.0;
    }
    
    // Generate swaths along direction
    reportProgress(30, "Generating swaths...");
    
    double cos_d = std::cos(direction);
    double sin_d = std::sin(direction);
    double perp_x = -sin_d;
    double perp_y = cos_d;
    
    // Project bounds onto direction axis
    double center_x = (min_x + max_x) / 2;
    double center_y = (min_y + max_y) / 2;
    double width = std::max(max_x - min_x, max_y - min_y);
    
    int num_swaths = static_cast<int>(width / config.swath_width) + 1;
    double start_offset = -width / 2;
    
    for (int i = 0; i < num_swaths; ++i) {
        double offset = start_offset + i * config.swath_width;
        
        double base_x = center_x + offset * perp_x;
        double base_y = center_y + offset * perp_y;
        
        Swath swath;
        swath.start = Point2D(base_x - width * cos_d, base_y - width * sin_d);
        swath.end = Point2D(base_x + width * cos_d, base_y + width * sin_d);
        swath.heading = direction;
        
        // Reverse direction for boustrophedon pattern
        if (i % 2 == 1) {
            std::swap(swath.start, swath.end);
            swath.heading = direction + M_PI;
        }
        
        result.swaths.push_back(swath);
    }
    
    // Generate path from swaths
    reportProgress(70, "Generating path...");
    result.path = swathsToAxialTurnPath(result.swaths);
    
    // Route is same as path endpoints
    for (const auto& sw : result.swaths) {
        PathState s;
        s.point = sw.start;
        s.heading = sw.heading;
        result.route.push_back(s);
        
        PathState e;
        e.point = sw.end;
        e.heading = sw.heading;
        result.route.push_back(e);
    }
    
    reportProgress(100, "Coverage complete");
    result.success = true;
    
    return result;
}

#endif // HAVE_FIELDS2COVER

// =============================================================================
// Export Functions
// =============================================================================

namespace {

constexpr double kWaypointDuplicateEpsilon = 1e-6;

PathStateList dedupePathStates(const PathStateList& path) {
    PathStateList filtered;
    filtered.reserve(path.size());
    for (const auto& state : path) {
        if (!filtered.empty()) {
            double dx = state.point.x - filtered.back().point.x;
            double dy = state.point.y - filtered.back().point.y;
            if (std::fabs(dx) <= kWaypointDuplicateEpsilon &&
                std::fabs(dy) <= kWaypointDuplicateEpsilon) {
                continue;
            }
        }
        filtered.push_back(state);
    }
    return filtered;
}

} // namespace

bool savePathToCSV(const PathStateList& path, const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }
    
    file << "x,y\n";
    PathStateList deduped_path = dedupePathStates(path);
    for (const auto& state : deduped_path) {
        file << std::fixed << std::setprecision(6) 
             << state.point.x << "," << state.point.y << "\n";
    }
    
    return true;
}

} // namespace f2c_cpp
