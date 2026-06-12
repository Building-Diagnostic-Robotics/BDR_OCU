/**
 * @file coverage_pipeline.hpp
 * @brief Coverage planning pipeline using Fields2Cover
 * 
 * This is the C++ equivalent of f2c_pipeline.py
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>
#include <cmath>

#include <Eigen/Dense>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

// Forward declare F2C types - we'll handle API differences in the .cpp file
#ifdef HAVE_FIELDS2COVER
namespace f2c {
    namespace types {
        class Field;
        class Cell;
        class Swaths;
        class Route;
        class Path;
    }
}
#endif

namespace f2c_cpp {

// =============================================================================
// Basic Types
// =============================================================================

struct Point2D {
    double x, y;
    Point2D() : x(0), y(0) {}
    Point2D(double x_, double y_) : x(x_), y(y_) {}
};

struct Point3D {
    double x, y, z;
    Point3D() : x(0), y(0), z(0) {}
    Point3D(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
};

using Polygon2D = std::vector<Point2D>;
struct Obstacle2D {
    // Outer ring (boundary of obstacle area)
    Polygon2D outer;
    // Optional holes (free space inside the outer ring)
    std::vector<Polygon2D> holes;
};
using PointCloud = pcl::PointCloud<pcl::PointXYZ>;
using PointCloudPtr = pcl::PointCloud<pcl::PointXYZ>::Ptr;

// =============================================================================
// Coverage Configuration
// =============================================================================

struct CoverageConfig {
    double swath_width = 1.0;
    double headland_width = 1.0;
    double turn_radius = 0.5;
    bool auto_align = false;
    std::string align_mode = "perp";      // "perp" or "long"
    std::string route_pattern = "boustro"; // "boustro", "snake", "spiral"
    std::string path_planner = "dubins";  // "dubins", "dubins_cc", "reeds", "reeds_hc", "none"
    std::string obj_function = "nswath_modified";
    bool use_decomposition = false;
    std::string decomposition_type = "boustrophedon";
    bool use_axial_turns = false;
    // Optional controller-facing post-processing:
    // If > 0, the returned path will be resampled to approximately this spacing (meters).
    // This helps controllers that expect more uniform waypoint spacing.
    double waypoint_spacing = 0.0;
    // Safety standoff (m) inflated around the obstacle union before it is
    // subtracted from the work area. 0 = no clearance (raw obstacle footprint).
    double obstacle_clearance = 0.0;
    std::optional<Point2D> start_point;
    std::optional<Point2D> end_point;
};

// =============================================================================
// Swath and Path Types
// =============================================================================

struct Swath {
    Point2D start;
    Point2D end;
    double heading;  // radians
    
    Swath() : heading(0) {}
    Swath(const Point2D& s, const Point2D& e) : start(s), end(e) {
        heading = std::atan2(e.y - s.y, e.x - s.x);
    }
};

struct PathState {
    Point2D point;
    double vx, vy;  // velocity components (direction)
    double heading;
    
    PathState() : vx(1), vy(0), heading(0) {}
    PathState(const Point2D& p, double h) : point(p), heading(h) {
        vx = std::cos(h);
        vy = std::sin(h);
    }
};

using SwathList = std::vector<Swath>;
using PathStateList = std::vector<PathState>;

// =============================================================================
// Coverage Result
// =============================================================================

struct CoverageResult {
    SwathList swaths;
    PathStateList route;  // Ordered waypoints from route
    PathStateList path;   // Smooth path with all states
    double effective_area_m2 = 0.0;  // Area of (boundary ∩ ROI) − obstacles
    bool success = false;
    std::string error_message;
};

// =============================================================================
// Point Cloud Processing
// =============================================================================

/**
 * @brief Load a point cloud from file (PCD, PLY, XYZ)
 */
PointCloudPtr loadPointCloudFile(const std::string& path);

/**
 * @brief Filter points within +/- z_band of median Z (legacy)
 */
PointCloudPtr filterByZBand(const PointCloudPtr& cloud, double z_band);

/**
 * @brief Filter points within [z_min, z_max] range relative to origin (Z=0)
 * @param cloud Input point cloud
 * @param z_min Minimum Z value (can be negative, e.g., -0.5 for below robot)
 * @param z_max Maximum Z value (e.g., 1.0 for 1m above robot)
 * @return Filtered point cloud with only points where z_min <= Z <= z_max
 * 
 * This is useful for robot applications where Z=0 is the robot's position
 * and you want to filter points relative to the robot, not the median height.
 */
PointCloudPtr filterByZRange(const PointCloudPtr& cloud, double z_min, double z_max);

/**
 * @brief Random subsampling
 */
PointCloudPtr subsampleRandom(const PointCloudPtr& cloud, size_t max_points);

/**
 * @brief Voxel grid downsampling
 */
PointCloudPtr downsampleVoxel(const PointCloudPtr& cloud, double voxel_size);

/**
 * @brief Statistical outlier removal
 */
PointCloudPtr downsampleStatistical(const PointCloudPtr& cloud, int mean_k, double std_ratio);

// =============================================================================
// Geometry Operations
// =============================================================================

/**
 * @brief Compute concave hull from 2D points
 * @param points 2D points
 * @param alpha Alpha parameter (smaller = more detailed)
 * @param method "alphashape", "delaunay", or "grid"
 */
Polygon2D computeConcaveHull(const std::vector<Point2D>& points, 
                             double alpha = 1.5,
                             const std::string& method = "alphashape");

/**
 * @brief Compute convex hull (fast fallback)
 */
Polygon2D computeConvexHull(const std::vector<Point2D>& points);

/**
 * @brief Simplify polygon using Douglas-Peucker algorithm
 */
Polygon2D simplifyPolygon(const Polygon2D& poly, double tolerance);

/**
 * @brief Compute dominant angle from minimum rotated rectangle
 */
double computeSwathAngle(const Polygon2D& poly);

/**
 * @brief Check if polygon is concave
 */
bool isPolygonConcave(const Polygon2D& poly);

/**
 * @brief Compute polygon area
 */
double polygonArea(const Polygon2D& poly);

/**
 * @brief Check if polygon is valid (closed, no self-intersection)
 */
bool isPolygonValid(const Polygon2D& poly);

/**
 * @brief Get polygon bounding box
 */
void polygonBounds(const Polygon2D& poly, 
                   double& min_x, double& min_y, 
                   double& max_x, double& max_y);

/**
 * @brief Clip an obstacle to a (possibly concave) clip polygon.
 *
 * Returns the obstacle pieces that fall inside @p clip (interior holes
 * preserved). May return zero pieces (obstacle entirely outside the clip)
 * or multiple pieces (obstacle split by a concave clip boundary). If @p clip
 * has < 3 vertices the obstacle is returned unchanged. Concave-safe (uses
 * Boost.Geometry intersection, not convex-only clipping).
 */
std::vector<Obstacle2D> clipObstacleToPolygon(const Obstacle2D& obstacle,
                                              const Polygon2D& clip);

/**
 * @brief Subtract a region from an obstacle (boolean difference).
 *
 * Returns the obstacle pieces remaining after @p cutter is removed. May return
 * zero pieces (cutter fully covers the obstacle), one piece (possibly with a
 * hole when the cutter is interior), or several (cutter bisects the obstacle).
 * If @p cutter has < 3 vertices the obstacle is returned unchanged. Inputs that
 * fail validation pass through untouched. Concave-safe.
 */
std::vector<Obstacle2D> subtractPolygonFromObstacle(const Obstacle2D& obstacle,
                                                    const Polygon2D& cutter);

/**
 * @brief Free-space result: (boundary ∩ ROI) − inflated obstacle union.
 *
 * @c regions are disjoint, valid polygons (holes preserved). On failure
 * @c success is false and @c error_message is set. @c skipped_obstacles counts
 * obstacles dropped because they were degenerate/unrepairable (never aborts the
 * whole build).
 */
struct FreeSpaceResult {
    std::vector<Obstacle2D> regions;
    double effective_area_m2 = 0.0;
    int skipped_obstacles = 0;
    bool success = false;
    std::string error_message;
};

/**
 * @brief Build the robust free-space geometry (no Fields2Cover dependency).
 *
 * Pipeline: validate boundary → intersect with ROI → per-obstacle
 * normalize/repair → union all obstacles → inflate by @p obstacle_clearance →
 * single difference from the work area → sliver-filter. Invalid obstacles are
 * skipped (counted) rather than failing the build.
 */
FreeSpaceResult buildFreeSpacePolygons(const Polygon2D& boundary,
                                       const Polygon2D* roi,
                                       const std::vector<Obstacle2D>* obstacles,
                                       double obstacle_clearance = 0.0);

// =============================================================================
// Coverage Generation
// =============================================================================

/**
 * @brief Generate coverage (swaths, route, path) using Fields2Cover
 * @param boundary Main field boundary
 * @param config Coverage configuration
 * @param roi Optional region of interest (restricts coverage area)
 * @param obstacles List of obstacle polygons to avoid
 */
CoverageResult generateCoverage(const Polygon2D& boundary,
                                const CoverageConfig& config,
                                const Polygon2D* roi = nullptr,
                                const std::vector<Obstacle2D>* obstacles = nullptr);

/**
 * @brief Generate swaths for axial-turn robots (direct swath corners)
 */
PathStateList swathsToAxialTurnPath(const SwathList& swaths);

// =============================================================================
// Export Functions
// =============================================================================

/**
 * @brief Save path to CSV file (x, y coordinates only)
 */
bool savePathToCSV(const PathStateList& path, const std::string& filename);

// =============================================================================
// Progress Callback
// =============================================================================

using ProgressCallback = std::function<void(int percent, const std::string& message)>;

/**
 * @brief Set global progress callback for long operations
 */
void setProgressCallback(ProgressCallback callback);

} // namespace f2c_cpp
