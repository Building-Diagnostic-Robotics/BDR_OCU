/**
 * @file coverage_pipeline.cpp
 * @brief Implementation of coverage planning pipeline
 */

#include "coverage_pipeline.hpp"

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
#include <queue>
#include <deque>
#include <stdexcept>
#include <unordered_set>

// Robust polygon ops (ROI intersection, obstacle clipping, validity checks)
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/policies/is_valid/failing_reason_policy.hpp>
#include <boost/geometry/algorithms/buffer.hpp>

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
static CancelCheckCallback g_cancelCheckCallback = nullptr;

void setProgressCallback(ProgressCallback callback) {
    g_progressCallback = callback;
}

void setCancelCheckCallback(CancelCheckCallback callback) {
    g_cancelCheckCallback = callback;
}

static void throwIfCancelled() {
    if (g_cancelCheckCallback && g_cancelCheckCallback()) {
        throw std::runtime_error("Operation cancelled");
    }
}

static void reportProgress(int percent, const std::string& message) {
    throwIfCancelled();
    if (g_progressCallback) {
        g_progressCallback(percent, message);
    }
    throwIfCancelled();
}

namespace {
namespace bg = boost::geometry;

// We want outer rings CCW and inner rings CW (matches common GIS conventions and our F2C usage).
using BgPoint = bg::model::d2::point_xy<double>;
using BgPolygon = bg::model::polygon<BgPoint, /*ClockWise=*/false, /*Closed=*/true>;
using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;
using BgLineString = bg::model::linestring<BgPoint>;

constexpr double kGeomEps = 1e-9;
constexpr double kMinValidArea = 1e-10;  // m^2-ish (depends on input units)
constexpr double kEdgeSampleMinLen = 1.0;
constexpr double kEdgeSampleLongLen = 3.0;
constexpr double kPathSearchCostEps = 1e-9;

static PathStateList resamplePathStates(const PathStateList& in, double spacing_m);
static PathStateList dedupePathStates(const PathStateList& path);

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

static bool buildEffectiveFreeSpace(
    const Polygon2D& boundary,
    const Polygon2D* roi,
    const std::vector<Obstacle2D>* obstacles,
    BgMultiPolygon& out_free,
    Polygon2D& out_primary_outer,
    std::string& error,
    double obstacle_clearance = 0.0) {

    Polygon2D boundary_clean = sanitizePolygon2D(boundary);
    if (boundary_clean.size() < 3) {
        error = "Boundary polygon is too small (need >= 3 vertices)";
        return false;
    }

    BgPolygon boundary_bg = toBgPolygon(boundary_clean);
    {
        std::string why;
        if (!bgValidate(boundary_bg, why)) {
            error = "Boundary polygon is invalid: " + why;
            return false;
        }
    }

    BgMultiPolygon work;
    if (roi && !roi->empty()) {
        Polygon2D roi_clean = sanitizePolygon2D(*roi);
        if (roi_clean.size() < 3) {
            error = "ROI polygon is too small (need >= 3 vertices)";
            return false;
        }
        BgPolygon roi_bg = toBgPolygon(roi_clean);
        {
            std::string why;
            if (!bgValidate(roi_bg, why)) {
                error = "ROI polygon is invalid: " + why;
                return false;
            }
        }
        work = bgIntersection(boundary_bg, roi_bg);
    } else {
        work.push_back(boundary_bg);
    }

    // Drop tiny pieces (helps after intersection/difference)
    BgMultiPolygon filtered;
    for (auto& p : work) {
        bg::correct(p);
        if (std::fabs(bg::area(p)) > kMinValidArea) {
            filtered.push_back(p);
        }
    }
    work = filtered;
    if (work.empty()) {
        error = "ROI does not intersect the boundary (effective area is empty)";
        return false;
    }

    auto bufferObstacle = [](const BgPolygon& poly, double dist) -> BgMultiPolygon {
        BgMultiPolygon out;
        if (dist <= 0.0) {
            out.push_back(poly);
            return out;
        }
        bg::strategy::buffer::distance_symmetric<double> distance_strategy(dist);
        bg::strategy::buffer::join_round join_strategy(12);
        bg::strategy::buffer::end_round end_strategy(12);
        bg::strategy::buffer::point_circle circle_strategy(12);
        bg::strategy::buffer::side_straight side_strategy;
        bg::buffer(poly, out, distance_strategy, side_strategy, join_strategy, end_strategy, circle_strategy);
        for (auto& p : out) {
            bg::correct(p);
        }
        return out;
    };

    if (obstacles && !obstacles->empty()) {
        for (size_t i = 0; i < obstacles->size(); ++i) {
            const auto& obs_in = obstacles->at(i);
            Polygon2D obs_outer_clean = sanitizePolygon2D(obs_in.outer);
            if (obs_outer_clean.size() < 3) {
                error = "Obstacle polygon #" + std::to_string(i + 1) + " is too small (need >= 3 vertices)";
                return false;
            }
            Obstacle2D obs_clean{obs_outer_clean, obs_in.holes};
            BgPolygon obs_bg = toBgPolygon(obs_clean);
            {
                std::string why;
                if (!bgValidate(obs_bg, why)) {
                    error = "Obstacle polygon #" + std::to_string(i + 1) + " is invalid: " + why;
                    return false;
                }
            }
            const double inflate = std::max(0.0, obstacle_clearance) + 1e-3;
            BgMultiPolygon obs_polys = bufferObstacle(obs_bg, inflate);
            for (const auto& obs_poly : obs_polys) {
                work = bgDifference(work, obs_poly);
                if (work.empty()) {
                    error = "Obstacles removed all usable area";
                    return false;
                }
            }
            if (work.empty()) {
                error = "Obstacles removed all usable area";
                return false;
            }
        }
    }

    // Choose a primary polygon for node generation (largest area)
    double best_area = -1.0;
    BgPolygon const* best = nullptr;
    for (const auto& p : work) {
        double a = std::fabs(bg::area(p));
        if (a > best_area) {
            best_area = a;
            best = &p;
        }
    }
    if (!best) {
        error = "Effective area is empty";
        return false;
    }
    out_primary_outer = bgRingToPolygon2D(best->outer());
    out_free = work;
    return true;
}

static bool pointInFreeSpace(const Point2D& p, const BgMultiPolygon& free_space) {
    BgPoint bp(p.x, p.y);
    for (const auto& poly : free_space) {
        if (bg::covered_by(bp, poly)) {
            return true;
        }
    }
    return false;
}

static bool segmentInFreeSpace(const Point2D& a, const Point2D& b, const BgMultiPolygon& free_space) {
    BgLineString line;
    line.push_back(BgPoint(a.x, a.y));
    line.push_back(BgPoint(b.x, b.y));
    for (const auto& poly : free_space) {
        if (bg::covered_by(line, poly)) {
            return true;
        }
    }
    return false;
}

static void appendRingVertices(
    const BgPolygon& poly,
    std::vector<Point2D>& nodes,
    std::unordered_set<uint64_t>& seen,
    double quant = 1e-6) {
    auto add_point = [&](double x, double y) {
        const int64_t qx = static_cast<int64_t>(std::llround(x / quant));
        const int64_t qy = static_cast<int64_t>(std::llround(y / quant));
        const uint64_t key = (static_cast<uint64_t>(qx) << 32) ^ static_cast<uint64_t>(qy);
        if (seen.insert(key).second) {
            nodes.emplace_back(x, y);
        }
    };

    auto add_ring = [&](const auto& ring) {
        const size_t n = ring.size();
        if (n < 2) return;
        for (const auto& pt : ring) {
            add_point(bg::get<0>(pt), bg::get<1>(pt));
        }
        for (size_t i = 0; i < n; ++i) {
            const auto& a = ring[i];
            const auto& b = ring[(i + 1) % n];
            const double ax = bg::get<0>(a);
            const double ay = bg::get<1>(a);
            const double bx = bg::get<0>(b);
            const double by = bg::get<1>(b);
            const double len = std::hypot(bx - ax, by - ay);
            if (len < 1e-9) {
                continue;
            }
            if (len >= kEdgeSampleMinLen) {
                if (len >= kEdgeSampleLongLen) {
                    add_point(ax + (bx - ax) / 3.0, ay + (by - ay) / 3.0);
                    add_point(ax + 2.0 * (bx - ax) / 3.0, ay + 2.0 * (by - ay) / 3.0);
                } else {
                    add_point((ax + bx) * 0.5, (ay + by) * 0.5);
                }
            }
        }
    };

    add_ring(poly.outer());
    for (const auto& inner : poly.inners()) {
        add_ring(inner);
    }
}

static PathStateList pointsToPathStates(const std::vector<Point2D>& pts) {
    PathStateList out;
    if (pts.empty()) {
        return out;
    }
    out.reserve(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        double heading = 0.0;
        if (i + 1 < pts.size()) {
            heading = std::atan2(pts[i + 1].y - pts[i].y, pts[i + 1].x - pts[i].x);
        } else if (!out.empty()) {
            heading = out.back().heading;
        }
        out.emplace_back(pts[i], heading);
    }
    return out;
}

static PathStateList planObstacleAvoidingPathInternal(
    const Point2D& start,
    const Point2D& goal,
    const BgMultiPolygon& free_space,
    double waypoint_spacing) {

    PathStateList empty;
    if (free_space.empty()) {
        return empty;
    }
    if (!pointInFreeSpace(start, free_space) || !pointInFreeSpace(goal, free_space)) {
        return empty;
    }

    if (segmentInFreeSpace(start, goal, free_space)) {
        std::vector<Point2D> pts{start, goal};
        PathStateList path = pointsToPathStates(pts);
        return resamplePathStates(path, waypoint_spacing);
    }

    struct Cost {
        int turns;
        double length;
    };
    constexpr int kInfTurns = std::numeric_limits<int>::max() / 4;
    const Cost kInfCost{kInfTurns, std::numeric_limits<double>::infinity()};
    auto better = [](const Cost& a, const Cost& b) {
        if (a.turns != b.turns) return a.turns < b.turns;
        return a.length + kPathSearchCostEps < b.length;
    };
    auto equalCost = [](const Cost& a, const Cost& b) {
        return a.turns == b.turns && std::fabs(a.length - b.length) <= kPathSearchCostEps;
    };
    auto lexicographicMax = [&](const Cost& a, const Cost& b) {
        return better(a, b) ? b : a;
    };

    auto simplifyByVisibility = [&](const std::vector<Point2D>& pts) {
        if (pts.size() <= 2) return pts;
        std::vector<Point2D> out;
        out.reserve(pts.size());
        size_t i = 0;
        out.push_back(pts[i]);
        while (i + 1 < pts.size()) {
            size_t j = pts.size() - 1;
            for (size_t k = pts.size() - 1; k > i; --k) {
                if (segmentInFreeSpace(pts[i], pts[k], free_space)) {
                    j = k;
                    break;
                }
            }
            if (j == i) {
                // Should not happen, but avoid infinite loop.
                j = i + 1;
            }
            out.push_back(pts[j]);
            i = j;
        }
        return out;
    };

    // Build a grid over the free-space island and run bidirectional heuristic A*.
    constexpr size_t kMaxGridCells = 250000;
    double grid_res = (waypoint_spacing > 0.0) ? waypoint_spacing : 0.2;
    grid_res = std::clamp(grid_res, 0.05, 1.0);

    bg::model::box<BgPoint> bbox;
    bool has_box = false;
    for (const auto& poly : free_space) {
        bg::model::box<BgPoint> b;
        bg::envelope(poly, b);
        if (!has_box) {
            bbox = b;
            has_box = true;
        } else {
            bg::expand(bbox, b);
        }
    }
    if (!has_box) {
        return empty;
    }

    double min_x = bg::get<bg::min_corner, 0>(bbox);
    double min_y = bg::get<bg::min_corner, 1>(bbox);
    double max_x = bg::get<bg::max_corner, 0>(bbox);
    double max_y = bg::get<bg::max_corner, 1>(bbox);

    while (true) {
        // Pad by one cell so boundary points fall inside.
        double pad = grid_res;
        double dx = (max_x - min_x) + 2.0 * pad;
        double dy = (max_y - min_y) + 2.0 * pad;
        const int w = static_cast<int>(std::ceil(dx / grid_res));
        const int h = static_cast<int>(std::ceil(dy / grid_res));
        const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
        if (total <= kMaxGridCells || grid_res >= 1.0) {
            min_x -= pad;
            min_y -= pad;
            max_x += pad;
            max_y += pad;
            break;
        }
        grid_res *= 1.5;
    }

    const int width = static_cast<int>(std::ceil((max_x - min_x) / grid_res));
    const int height = static_cast<int>(std::ceil((max_y - min_y) / grid_res));
    if (width <= 1 || height <= 1) {
        return empty;
    }

    auto cellIndex = [&](int ix, int iy) {
        return iy * width + ix;
    };
    auto cellCenter = [&](int ix, int iy) {
        return Point2D(min_x + (ix + 0.5) * grid_res, min_y + (iy + 0.5) * grid_res);
    };

    std::vector<uint8_t> free_mask(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    for (int y = 0; y < height; ++y) {
        if ((y & 0x7F) == 0) {
            throwIfCancelled();
        }
        for (int x = 0; x < width; ++x) {
            Point2D p = cellCenter(x, y);
            if (pointInFreeSpace(p, free_space)) {
                free_mask[static_cast<size_t>(cellIndex(x, y))] = 1;
            }
        }
    }

    auto clampCell = [&](int ix, int iy) {
        ix = std::clamp(ix, 0, width - 1);
        iy = std::clamp(iy, 0, height - 1);
        return std::pair<int, int>(ix, iy);
    };

    auto nearestCell = [&](const Point2D& p) {
        int ix = static_cast<int>(std::floor((p.x - min_x) / grid_res));
        int iy = static_cast<int>(std::floor((p.y - min_y) / grid_res));
        return clampCell(ix, iy);
    };

    auto findNearestFree = [&](int sx, int sy, int& out_x, int& out_y) {
        std::deque<int> q;
        std::vector<uint8_t> visited(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
        int start_idx = cellIndex(sx, sy);
        q.push_back(start_idx);
        visited[static_cast<size_t>(start_idx)] = 1;
        const int dirs4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        while (!q.empty()) {
            int idx = q.front();
            q.pop_front();
            if (free_mask[static_cast<size_t>(idx)]) {
                out_x = idx % width;
                out_y = idx / width;
                return true;
            }
            int cx = idx % width;
            int cy = idx / width;
            for (const auto& d : dirs4) {
                int nx = cx + d[0];
                int ny = cy + d[1];
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                    continue;
                }
                int nidx = cellIndex(nx, ny);
                if (visited[static_cast<size_t>(nidx)]) {
                    continue;
                }
                visited[static_cast<size_t>(nidx)] = 1;
                q.push_back(nidx);
            }
        }
        return false;
    };

    auto start_cell = nearestCell(start);
    auto goal_cell = nearestCell(goal);
    int sx = start_cell.first;
    int sy = start_cell.second;
    int gx = goal_cell.first;
    int gy = goal_cell.second;
    if (!findNearestFree(sx, sy, sx, sy)) {
        return empty;
    }
    if (!findNearestFree(gx, gy, gx, gy)) {
        return empty;
    }

    const int start_idx = cellIndex(sx, sy);
    const int goal_idx = cellIndex(gx, gy);
    if (start_idx == goal_idx) {
        std::vector<Point2D> pts{start, goal};
        PathStateList path = pointsToPathStates(pts);
        return resamplePathStates(path, waypoint_spacing);
    }

    const Point2D start_cell_center = cellCenter(sx, sy);
    const Point2D goal_cell_center = cellCenter(gx, gy);
    const int dirs8[8][2] = {
        {1,0},{-1,0},{0,1},{0,-1},
        {1,1},{1,-1},{-1,1},{-1,-1}
    };
    const double diag = std::sqrt(2.0);

    auto canStep = [&](int cx, int cy, int nx, int ny) {
        if (nx < 0 || ny < 0 || nx >= width || ny >= height) return false;
        int nidx = cellIndex(nx, ny);
        if (!free_mask[static_cast<size_t>(nidx)]) return false;
        Point2D a = cellCenter(cx, cy);
        Point2D b = cellCenter(nx, ny);
        return segmentInFreeSpace(a, b, free_space);
    };

    const int dir_states = 9;  // 8 directions + 1 for "none"
    const size_t cell_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t state_count = cell_count * dir_states;
    std::vector<int> best_turns_f(state_count, kInfTurns);
    std::vector<double> best_len_f(state_count, std::numeric_limits<double>::infinity());
    std::vector<int> parent_state_f(state_count, -1);
    std::vector<int> best_turns_r(state_count, kInfTurns);
    std::vector<double> best_len_r(state_count, std::numeric_limits<double>::infinity());
    std::vector<int> parent_state_r(state_count, -1);

    auto stateIndex = [&](int dir_state, int cell_idx) {
        return (dir_state + 1) * static_cast<int>(cell_count) + cell_idx;
    };
    auto stateCost = [&](const std::vector<int>& turns, const std::vector<double>& lengths, int state) {
        return Cost{turns[static_cast<size_t>(state)], lengths[static_cast<size_t>(state)]};
    };
    auto hasState = [&](const std::vector<int>& turns, const std::vector<double>& lengths, int state) {
        return turns[static_cast<size_t>(state)] < kInfTurns &&
               std::isfinite(lengths[static_cast<size_t>(state)]);
    };
    auto heuristicLength = [&](int cell_idx, const Point2D& target_center) {
        int cx = cell_idx % width;
        int cy = cell_idx / width;
        Point2D c = cellCenter(cx, cy);
        return std::hypot(c.x - target_center.x, c.y - target_center.y);
    };

    struct Node {
        int dir_state;
        int cell_idx;
        Cost g;
        double h_length;
    };
    struct Cmp {
        bool operator()(const Node& a, const Node& b) const {
            if (a.g.turns != b.g.turns) return a.g.turns > b.g.turns;
            const double af = a.g.length + a.h_length;
            const double bf = b.g.length + b.h_length;
            if (std::fabs(af - bf) > kPathSearchCostEps) return af > bf;
            return a.g.length > b.g.length;
        }
    };

    using Frontier = std::priority_queue<Node, std::vector<Node>, Cmp>;
    Frontier forward_open;
    Frontier reverse_open;

    auto isStale = [&](const Node& node, const std::vector<int>& turns, const std::vector<double>& lengths) {
        const int state = stateIndex(node.dir_state, node.cell_idx);
        return node.g.turns != turns[static_cast<size_t>(state)] ||
               std::fabs(node.g.length - lengths[static_cast<size_t>(state)]) > kPathSearchCostEps;
    };
    auto pruneFrontier = [&](Frontier& frontier, const std::vector<int>& turns, const std::vector<double>& lengths) {
        while (!frontier.empty() && isStale(frontier.top(), turns, lengths)) {
            frontier.pop();
        }
    };
    auto frontierLowerBound = [&](Frontier& frontier,
                                  const std::vector<int>& turns,
                                  const std::vector<double>& lengths) -> std::optional<Cost> {
        pruneFrontier(frontier, turns, lengths);
        if (frontier.empty()) {
            return std::nullopt;
        }
        const Node& top = frontier.top();
        return Cost{top.g.turns, top.g.length + top.h_length};
    };
    auto pushState = [&](Frontier& frontier,
                         std::vector<int>& turns,
                         std::vector<double>& lengths,
                         std::vector<int>& parents,
                         int dir_state,
                         int cell_idx,
                         const Cost& g,
                         int parent_state,
                         double h_length) {
        const int state = stateIndex(dir_state, cell_idx);
        if (!better(g, stateCost(turns, lengths, state))) {
            return false;
        }
        turns[static_cast<size_t>(state)] = g.turns;
        lengths[static_cast<size_t>(state)] = g.length;
        parents[static_cast<size_t>(state)] = parent_state;
        frontier.push(Node{dir_state, cell_idx, g, h_length});
        return true;
    };
    auto combineMeetingCost = [&](int forward_dir,
                                  const Cost& forward_cost,
                                  int reverse_dir,
                                  const Cost& reverse_cost) {
        Cost total{forward_cost.turns + reverse_cost.turns,
                   forward_cost.length + reverse_cost.length};
        if (forward_dir >= 0 && reverse_dir >= 0 && forward_dir != reverse_dir) {
            total.turns += 1;
        }
        return total;
    };

    const int start_state = stateIndex(-1, start_idx);
    best_turns_f[static_cast<size_t>(start_state)] = 0;
    best_len_f[static_cast<size_t>(start_state)] = 0.0;
    parent_state_f[static_cast<size_t>(start_state)] = -1;
    forward_open.push(Node{-1, start_idx, {0, 0.0}, heuristicLength(start_idx, goal_cell_center)});

    const int goal_state = stateIndex(-1, goal_idx);
    best_turns_r[static_cast<size_t>(goal_state)] = 0;
    best_len_r[static_cast<size_t>(goal_state)] = 0.0;
    parent_state_r[static_cast<size_t>(goal_state)] = -1;
    reverse_open.push(Node{-1, goal_idx, {0, 0.0}, heuristicLength(goal_idx, start_cell_center)});

    Cost best_solution = kInfCost;
    int best_forward_state = -1;
    int best_reverse_state = -1;

    auto considerForwardMeeting = [&](int forward_dir, int cell_idx) {
        const int f_state = stateIndex(forward_dir, cell_idx);
        if (!hasState(best_turns_f, best_len_f, f_state)) {
            return;
        }
        const Cost forward_cost = stateCost(best_turns_f, best_len_f, f_state);
        for (int reverse_dir = -1; reverse_dir < 8; ++reverse_dir) {
            const int r_state = stateIndex(reverse_dir, cell_idx);
            if (!hasState(best_turns_r, best_len_r, r_state)) {
                continue;
            }
            const Cost candidate =
                combineMeetingCost(forward_dir, forward_cost,
                                   reverse_dir, stateCost(best_turns_r, best_len_r, r_state));
            if (better(candidate, best_solution)) {
                best_solution = candidate;
                best_forward_state = f_state;
                best_reverse_state = r_state;
            }
        }
    };
    auto considerReverseMeeting = [&](int reverse_dir, int cell_idx) {
        const int r_state = stateIndex(reverse_dir, cell_idx);
        if (!hasState(best_turns_r, best_len_r, r_state)) {
            return;
        }
        const Cost reverse_cost = stateCost(best_turns_r, best_len_r, r_state);
        for (int forward_dir = -1; forward_dir < 8; ++forward_dir) {
            const int f_state = stateIndex(forward_dir, cell_idx);
            if (!hasState(best_turns_f, best_len_f, f_state)) {
                continue;
            }
            const Cost candidate =
                combineMeetingCost(forward_dir, stateCost(best_turns_f, best_len_f, f_state),
                                   reverse_dir, reverse_cost);
            if (better(candidate, best_solution)) {
                best_solution = candidate;
                best_forward_state = f_state;
                best_reverse_state = r_state;
            }
        }
    };

    size_t expansion_count = 0;
    while (true) {
        if ((expansion_count++ & 0x0FFFu) == 0u) {
            throwIfCancelled();
        }
        const auto forward_lb = frontierLowerBound(forward_open, best_turns_f, best_len_f);
        const auto reverse_lb = frontierLowerBound(reverse_open, best_turns_r, best_len_r);
        if (!forward_lb.has_value() || !reverse_lb.has_value()) {
            break;
        }

        const Cost lower_bound = lexicographicMax(*forward_lb, *reverse_lb);
        if (best_forward_state >= 0 && !better(lower_bound, best_solution)) {
            break;
        }

        const bool expand_forward =
            better(*forward_lb, *reverse_lb) || equalCost(*forward_lb, *reverse_lb);

        if (expand_forward) {
            Node cur = forward_open.top();
            forward_open.pop();
            if (isStale(cur, best_turns_f, best_len_f)) {
                continue;
            }

            const int cur_state = stateIndex(cur.dir_state, cur.cell_idx);
            considerForwardMeeting(cur.dir_state, cur.cell_idx);

            const int cx = cur.cell_idx % width;
            const int cy = cur.cell_idx / width;
            for (int dir = 0; dir < 8; ++dir) {
                const int nx = cx + dirs8[dir][0];
                const int ny = cy + dirs8[dir][1];
                if (!canStep(cx, cy, nx, ny)) {
                    continue;
                }
                const int nidx = cellIndex(nx, ny);
                const int add_turn = (cur.dir_state >= 0 && dir != cur.dir_state) ? 1 : 0;
                const double step_len = (dir >= 4) ? diag * grid_res : grid_res;
                const Cost ng{cur.g.turns + add_turn, cur.g.length + step_len};
                pushState(forward_open, best_turns_f, best_len_f, parent_state_f,
                          dir, nidx, ng, cur_state, heuristicLength(nidx, goal_cell_center));
            }
        } else {
            Node cur = reverse_open.top();
            reverse_open.pop();
            if (isStale(cur, best_turns_r, best_len_r)) {
                continue;
            }

            const int cur_state = stateIndex(cur.dir_state, cur.cell_idx);
            considerReverseMeeting(cur.dir_state, cur.cell_idx);

            const int cx = cur.cell_idx % width;
            const int cy = cur.cell_idx / width;
            for (int forward_dir = 0; forward_dir < 8; ++forward_dir) {
                const int nx = cx - dirs8[forward_dir][0];
                const int ny = cy - dirs8[forward_dir][1];
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                    continue;
                }
                if (!canStep(nx, ny, cx, cy)) {
                    continue;
                }
                const int nidx = cellIndex(nx, ny);
                const int add_turn = (cur.dir_state >= 0 && forward_dir != cur.dir_state) ? 1 : 0;
                const double step_len = (forward_dir >= 4) ? diag * grid_res : grid_res;
                const Cost ng{cur.g.turns + add_turn, cur.g.length + step_len};
                pushState(reverse_open, best_turns_r, best_len_r, parent_state_r,
                          forward_dir, nidx, ng, cur_state, heuristicLength(nidx, start_cell_center));
            }
        }
    }

    if (best_forward_state < 0 || best_reverse_state < 0) {
        return empty;
    }

    auto reconstructForward = [&](int state) {
        std::vector<Point2D> pts;
        while (state >= 0) {
            const int cell_idx = state % static_cast<int>(cell_count);
            const int cx = cell_idx % width;
            const int cy = cell_idx / width;
            pts.push_back(cellCenter(cx, cy));
            state = parent_state_f[static_cast<size_t>(state)];
        }
        std::reverse(pts.begin(), pts.end());
        return pts;
    };
    auto reconstructReverse = [&](int state) {
        std::vector<Point2D> pts;
        while (state >= 0) {
            const int cell_idx = state % static_cast<int>(cell_count);
            const int cx = cell_idx % width;
            const int cy = cell_idx / width;
            pts.push_back(cellCenter(cx, cy));
            state = parent_state_r[static_cast<size_t>(state)];
        }
        return pts;
    };

    std::vector<Point2D> forward_pts = reconstructForward(best_forward_state);
    std::vector<Point2D> reverse_pts = reconstructReverse(best_reverse_state);
    if (forward_pts.empty() || reverse_pts.empty()) {
        return empty;
    }

    std::vector<Point2D> raw_pts = forward_pts;
    if (reverse_pts.size() > 1) {
        raw_pts.insert(raw_pts.end(), reverse_pts.begin() + 1, reverse_pts.end());
    }

    Point2D start_center = raw_pts.front();
    Point2D goal_center = raw_pts.back();
    raw_pts.front() = start;
    raw_pts.back() = goal;
    if (raw_pts.size() >= 2) {
        if (!segmentInFreeSpace(raw_pts.front(), raw_pts[1], free_space)) {
            raw_pts.insert(raw_pts.begin() + 1, start_center);
        }
        if (!segmentInFreeSpace(raw_pts[raw_pts.size() - 2], raw_pts.back(), free_space)) {
            raw_pts.insert(raw_pts.end() - 1, goal_center);
        }
    }

    std::vector<Point2D> simplified = simplifyByVisibility(raw_pts);
    PathStateList path = pointsToPathStates(simplified);
    return resamplePathStates(path, waypoint_spacing);
}

static PathStateList connectAnchorsWithFreeSpace(
    const std::vector<Point2D>& anchors,
    const BgMultiPolygon& free_space,
    double waypoint_spacing) {
    PathStateList combined;
    if (anchors.size() < 2) {
        return combined;
    }
    for (size_t i = 0; i + 1 < anchors.size(); ++i) {
        PathStateList seg = planObstacleAvoidingPathInternal(
            anchors[i], anchors[i + 1], free_space, waypoint_spacing);
        if (seg.empty()) {
            return PathStateList();
        }
        if (!combined.empty() && !seg.empty()) {
            const auto& last = combined.back().point;
            const auto& first = seg.front().point;
            if (std::hypot(last.x - first.x, last.y - first.y) < 1e-6) {
                seg.erase(seg.begin());
            }
        }
        combined.insert(combined.end(), seg.begin(), seg.end());
    }
    return dedupePathStates(combined);
}

static double pathLengthMeters(const std::vector<Point2D>& pts) {
    if (pts.size() < 2) return 0.0;
    double len = 0.0;
    for (size_t i = 1; i < pts.size(); ++i) {
        len += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
    }
    return len;
}

static PathStateList dedupePathStates(const PathStateList& path);

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
    F2CCells& out_cells,
    Polygon2D& out_primary_outer,
    double& out_effective_area_m2,
    std::string& error,
    double obstacle_clearance = 0.0) {

    Polygon2D boundary_clean = sanitizePolygon2D(boundary);
    if (boundary_clean.size() < 3) {
        error = "Boundary polygon is too small (need >= 3 vertices)";
        return false;
    }

    BgPolygon boundary_bg = toBgPolygon(boundary_clean);
    {
        std::string why;
        if (!bgValidate(boundary_bg, why)) {
            error = "Boundary polygon is invalid: " + why;
            return false;
        }
    }

    BgMultiPolygon work;
    if (roi && !roi->empty()) {
        Polygon2D roi_clean = sanitizePolygon2D(*roi);
        if (roi_clean.size() < 3) {
            error = "ROI polygon is too small (need >= 3 vertices)";
            return false;
        }
        BgPolygon roi_bg = toBgPolygon(roi_clean);
        {
            std::string why;
            if (!bgValidate(roi_bg, why)) {
                error = "ROI polygon is invalid: " + why;
                return false;
            }
        }
        work = bgIntersection(boundary_bg, roi_bg);
    } else {
        work.push_back(boundary_bg);
    }

    // Drop tiny pieces (helps after intersection/difference)
    BgMultiPolygon filtered;
    for (auto& p : work) {
        bg::correct(p);
        if (std::fabs(bg::area(p)) > kMinValidArea) {
            filtered.push_back(p);
        }
    }
    work = filtered;
    if (work.empty()) {
        error = "ROI does not intersect the boundary (effective area is empty)";
        return false;
    }

    auto bufferObstacle = [](const BgPolygon& poly, double dist) -> BgMultiPolygon {
        BgMultiPolygon out;
        if (dist <= 0.0) {
            out.push_back(poly);
            return out;
        }
        bg::strategy::buffer::distance_symmetric<double> distance_strategy(dist);
        bg::strategy::buffer::join_round join_strategy(12);
        bg::strategy::buffer::end_round end_strategy(12);
        bg::strategy::buffer::point_circle circle_strategy(12);
        bg::strategy::buffer::side_straight side_strategy;
        bg::buffer(poly, out, distance_strategy, side_strategy, join_strategy, end_strategy, circle_strategy);
        for (auto& p : out) {
            bg::correct(p);
        }
        return out;
    };

    if (obstacles && !obstacles->empty()) {
        for (size_t i = 0; i < obstacles->size(); ++i) {
            const auto& obs_in = obstacles->at(i);
            Polygon2D obs_outer_clean = sanitizePolygon2D(obs_in.outer);
            if (obs_outer_clean.size() < 3) {
                error = "Obstacle polygon #" + std::to_string(i + 1) + " is too small (need >= 3 vertices)";
                return false;
            }
            Obstacle2D obs_clean{obs_outer_clean, obs_in.holes};
            BgPolygon obs_bg = toBgPolygon(obs_clean);
            {
                std::string why;
                if (!bgValidate(obs_bg, why)) {
                    error = "Obstacle polygon #" + std::to_string(i + 1) + " is invalid: " + why;
                    return false;
                }
            }
            const double inflate = std::max(0.0, obstacle_clearance) + 1e-3;
            BgMultiPolygon obs_polys = bufferObstacle(obs_bg, inflate);
            for (const auto& obs_poly : obs_polys) {
                work = bgDifference(work, obs_poly);
                if (work.empty()) {
                    error = "Obstacles removed all usable area";
                    return false;
                }
            }
            if (work.empty()) {
                error = "Obstacles removed all usable area";
                return false;
            }
        }
    }

    out_effective_area_m2 = 0.0;
    for (const auto& p : work) {
        out_effective_area_m2 += std::fabs(bg::area(p));
    }

    // Choose a primary polygon (largest area) for swath alignment / concavity checks
    double best_area = -1.0;
    BgPolygon const* best = nullptr;
    for (const auto& p : work) {
        double a = std::fabs(bg::area(p));
        if (a > best_area) {
            best_area = a;
            best = &p;
        }
    }
    if (!best) {
        error = "Effective area is empty";
        return false;
    }
    out_primary_outer = bgRingToPolygon2D(best->outer());

    // Convert to F2C cells (multi-polygons become multiple cells; holes become interior rings)
    out_cells = F2CCells();
    for (const auto& p : work) {
        addBgPolygonToF2CCells(p, out_cells);
    }

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
    throwIfCancelled();
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
        size_t line_count = 0;
        while (file >> x >> y >> z) {
            cloud->push_back(pcl::PointXYZ(x, y, z));
            if ((++line_count & 0x0FFFu) == 0u) {
                throwIfCancelled();
            }
        }
        result = cloud->empty() ? -1 : 0;
    } else {
        throw std::runtime_error("Unsupported file format: " + ext);
    }
    
    if (result < 0 || cloud->empty()) {
        throw std::runtime_error("Failed to load point cloud: " + path);
    }
    
    throwIfCancelled();
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
    
    size_t pt_idx = 0;
    for (const auto& pt : cloud->points) {
        if ((pt_idx++ & 0x3FFFu) == 0u) {
            throwIfCancelled();
        }
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
    
    size_t pt_idx = 0;
    for (const auto& pt : cloud->points) {
        if ((pt_idx++ & 0x3FFFu) == 0u) {
            throwIfCancelled();
        }
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

#ifdef HAVE_FIELDS2COVER

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
        if (!buildEffectiveCellsFromROIAndObstacles(
                boundary, roi, obstacles, cells, effective_outer, effective_area_m2, geom_error,
                config.headland_width)) {
            result.error_message = geom_error;
            return result;
        }
        result.effective_area_m2 = effective_area_m2;

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
            // Use headland-cropped working area for routing so borders respect headland.
            F2CRoute f2c_route = route_planner.genRoute(working_area, swaths_by_cells_sorted);
            auto route_line = f2c_route.asLineString();
            // If the route is empty, fall back to the original cells (more permissive).
            if (route_line.size() == 0 && working_area.size() != cells.size()) {
                f2c_route = route_planner.genRoute(cells, swaths_by_cells_sorted);
                route_line = f2c_route.asLineString();
            }
            
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
            for (size_t i = 0; i < route_line.size(); ++i) {
                PathState state;
                state.point = Point2D(route_line.getX(i), route_line.getY(i));
                result.route.push_back(state);
            }
            
            // Generate path
            reportProgress(70, "Generating path...");
            
            // Use path planner based on config
            // "none" means straight lines (use route directly like axial turns)
            const bool force_axial = config.use_axial_turns;
            if (config.path_planner == "none" || force_axial) {
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

            // Optional controller-facing polish: resample path to fixed spacing
            if (config.waypoint_spacing > 0.0 && !result.path.empty()) {
                reportProgress(85, "Resampling path...");
                result.path = resamplePathStates(result.path, config.waypoint_spacing);
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Route generation warning: " << e.what() << std::endl;
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

PathStateList planObstacleAvoidingPath(
    const Point2D& start,
    const Point2D& goal,
    const Polygon2D& boundary,
    const Polygon2D* roi,
    const std::vector<Obstacle2D>* obstacles,
    double waypoint_spacing,
    double obstacle_clearance) {

    BgMultiPolygon free_space;
    Polygon2D primary_outer;
    std::string error;
    if (!buildEffectiveFreeSpace(boundary, roi, obstacles, free_space, primary_outer, error,
                                 std::max(0.0, obstacle_clearance))) {
        return {};
    }

    // Restrict to the connected island that contains the start position.
    // This prevents paths from "jumping" across obstacles to other islands.
    BgMultiPolygon start_island;
    BgPoint bs(start.x, start.y);
    for (const auto& poly : free_space) {
        if (bg::covered_by(bs, poly)) {
            start_island.push_back(poly);
        }
    }
    if (start_island.empty()) {
        return {};
    }

    return planObstacleAvoidingPathInternal(start, goal, start_island, waypoint_spacing);
}

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
