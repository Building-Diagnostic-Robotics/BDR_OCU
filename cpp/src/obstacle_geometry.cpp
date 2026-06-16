/**
 * @file obstacle_geometry.cpp
 * @brief Boost.Geometry obstacle/free-space core (no PCL / Fields2Cover deps).
 *
 * Unit-tested in isolation via tests/obstacle_geometry_tests.cpp.
 */

#include "coverage_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <queue>
#include <string>
#include <vector>

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/geometries/linestring.hpp>
#include <boost/geometry/geometries/multi_linestring.hpp>
#include <boost/geometry/policies/is_valid/failing_reason_policy.hpp>

namespace f2c_cpp {
namespace {
namespace bg = boost::geometry;

using BgPoint = bg::model::d2::point_xy<double>;
using BgPolygon = bg::model::polygon<BgPoint, /*ClockWise=*/false, /*Closed=*/true>;
using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;
using BgLineString = bg::model::linestring<BgPoint>;
using BgMultiLineString = bg::model::multi_linestring<BgLineString>;

constexpr double kGeomEps = 1e-9;
constexpr double kMinValidArea = 1e-10;

bool nearEqual(double a, double b, double eps = kGeomEps) {
    return std::fabs(a - b) <= eps;
}
bool nearPoint(const Point2D& a, const Point2D& b, double eps = kGeomEps) {
    return nearEqual(a.x, b.x, eps) && nearEqual(a.y, b.y, eps);
}

Polygon2D sanitize(const Polygon2D& in) {
    Polygon2D out;
    out.reserve(in.size());
    for (const auto& p : in) {
        if (out.empty() || !nearPoint(out.back(), p)) out.push_back(p);
    }
    if (out.size() >= 2 && nearPoint(out.front(), out.back())) out.pop_back();
    return out;
}

BgPolygon toBg(const Obstacle2D& obs) {
    BgPolygon poly;
    for (const auto& p : sanitize(obs.outer)) poly.outer().push_back(BgPoint(p.x, p.y));
    for (const auto& hole : obs.holes) {
        Polygon2D h = sanitize(hole);
        if (h.size() < 3) continue;
        poly.inners().emplace_back();
        for (const auto& p : h) poly.inners().back().push_back(BgPoint(p.x, p.y));
    }
    bg::correct(poly);
    return poly;
}
BgPolygon toBg(const Polygon2D& ring) { return toBg(Obstacle2D{sanitize(ring), {}}); }

bool validate(const BgPolygon& poly, std::string& reason) {
    bg::validity_failure_type failure;
    if (!bg::is_valid(poly, failure)) {
        reason = bg::validity_failure_type_message(failure);
        return false;
    }
    if (std::fabs(bg::area(poly)) <= kMinValidArea) {
        reason = "area is too small";
        return false;
    }
    return true;
}

template <typename RingT>
Polygon2D ringToPoly(const RingT& ring) {
    Polygon2D out;
    out.reserve(ring.size());
    for (const auto& pt : ring) out.emplace_back(bg::get<0>(pt), bg::get<1>(pt));
    if (out.size() >= 2 && nearPoint(out.front(), out.back())) out.pop_back();
    return out;
}

Obstacle2D bgToObstacle(const BgPolygon& p) {
    Obstacle2D o;
    o.outer = ringToPoly(p.outer());
    for (const auto& inner : p.inners()) {
        Polygon2D h = ringToPoly(inner);
        if (h.size() >= 3) o.holes.push_back(std::move(h));
    }
    return o;
}

template <typename Geom>
BgMultiPolygon buffered(const Geom& g, double dist) {
    BgMultiPolygon out;
    bg::strategy::buffer::distance_symmetric<double> ds(dist);
    bg::strategy::buffer::join_round jr(16);
    bg::strategy::buffer::end_round er(16);
    bg::strategy::buffer::point_circle pc(16);
    bg::strategy::buffer::side_straight ss;
    bg::buffer(g, out, ds, ss, jr, er, pc);
    for (auto& p : out) bg::correct(p);
    return out;
}

double bboxDiag(const BgPolygon& p) {
    double min_x = 1e300, min_y = 1e300, max_x = -1e300, max_y = -1e300;
    for (const auto& pt : p.outer()) {
        const double x = bg::get<0>(pt), y = bg::get<1>(pt);
        min_x = std::min(min_x, x); max_x = std::max(max_x, x);
        min_y = std::min(min_y, y); max_y = std::max(max_y, y);
    }
    if (max_x < min_x) return 0.0;
    return std::hypot(max_x - min_x, max_y - min_y);
}

// Normalize one obstacle to valid disjoint parts. Tries correct(); if still
// invalid, escalating zero-ish buffers heal self-touches/bow-ties. Returns
// false only when nothing usable survives.
bool repairObstacle(const Obstacle2D& obs, BgMultiPolygon& out, std::string& reason) {
    out.clear();
    Polygon2D outer = sanitize(obs.outer);
    if (outer.size() < 3) { reason = "degenerate: < 3 vertices"; return false; }

    BgPolygon poly = toBg(Obstacle2D{outer, obs.holes});
    if (validate(poly, reason)) { out.push_back(poly); return true; }

    const double diag = std::max(1.0, bboxDiag(poly));
    for (double f : {1e-6, 1e-5, 1e-4, 1e-3}) {
        BgMultiPolygon rep;
        try {
            rep = buffered(poly, std::max(1e-6, diag * f));
        } catch (const std::exception& e) {
            reason = e.what();
            continue;
        }
        BgMultiPolygon valid;
        for (auto& p : rep) {
            bg::correct(p);
            std::string w;
            if (validate(p, w)) valid.push_back(p);
            else reason = w;
        }
        if (!valid.empty()) { out = std::move(valid); return true; }
    }
    return false;
}

// Repaired union of every obstacle, optionally inflated by `clearance`.
// `skipped` (when non-null) accumulates obstacles dropped as unrepairable.
BgMultiPolygon unionInflatedObstacles(const std::vector<Obstacle2D>& obstacles,
                                      double clearance, int* skipped) {
    BgMultiPolygon u;
    for (size_t i = 0; i < obstacles.size(); ++i) {
        BgMultiPolygon parts;
        std::string why;
        if (!repairObstacle(obstacles[i], parts, why)) {
            if (skipped) ++*skipped;
            std::cerr << "[Coverage] Skipping obstacle #" << (i + 1) << " (" << why << ")\n";
            continue;
        }
        for (const auto& p : parts) {
            BgMultiPolygon merged;
            bg::union_(u, p, merged);
            for (auto& q : merged) bg::correct(q);
            u = std::move(merged);
        }
    }
    if (!u.empty() && clearance > 0.0) u = buffered(u, clearance);
    return u;
}

// ---- Layer-1 obstacle-aware connector routing helpers ---------------------

BgMultiPolygon toBgFreeSpace(const std::vector<Obstacle2D>& free_space) {
    BgMultiPolygon mp;
    for (const auto& reg : free_space) {
        if (sanitize(reg.outer).size() < 3) continue;
        BgPolygon p = toBg(reg);  // corrects orientation + attaches holes
        std::string why;
        if (validate(p, why)) mp.push_back(std::move(p));
    }
    return mp;
}

// A segment is traversable iff it lies within the closure of a single free-space
// region (boundary contact allowed; swath endpoints sit on the region boundary).
bool segmentInFreeSpace(const BgPoint& a, const BgPoint& b, const BgMultiPolygon& fs) {
    BgLineString seg;
    seg.push_back(a);
    seg.push_back(b);
    for (const auto& poly : fs) {
        if (bg::covered_by(seg, poly)) return true;
    }
    return false;
}

double dist(const BgPoint& p, const BgPoint& q) {
    return std::hypot(bg::get<0>(p) - bg::get<0>(q), bg::get<1>(p) - bg::get<1>(q));
}

// Sample the minor arc from t1 to t2 about center, radius r (excludes endpoints).
std::vector<Point2D> sampleArc(const Point2D& center, const Point2D& t1,
                               const Point2D& t2, double r) {
    double a1 = std::atan2(t1.y - center.y, t1.x - center.x);
    double a2 = std::atan2(t2.y - center.y, t2.x - center.x);
    double sweep = a2 - a1;
    while (sweep > M_PI) sweep -= 2.0 * M_PI;
    while (sweep < -M_PI) sweep += 2.0 * M_PI;

    const int steps = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / (M_PI / 18.0))));
    std::vector<Point2D> out;
    out.reserve(steps);
    for (int k = 1; k < steps; ++k) {
        double a = a1 + sweep * (static_cast<double>(k) / steps);
        out.push_back(Point2D(center.x + r * std::cos(a), center.y + r * std::sin(a)));
    }
    return out;
}

}  // namespace

std::vector<Obstacle2D> clipObstacleToPolygon(const Obstacle2D& obstacle,
                                              const Polygon2D& clip) {
    std::vector<Obstacle2D> out;
    Polygon2D outer_clean = sanitize(obstacle.outer);
    if (outer_clean.size() < 3) return out;
    Polygon2D clip_clean = sanitize(clip);
    if (clip_clean.size() < 3) { out.push_back(obstacle); return out; }

    BgPolygon obs_bg = toBg(obstacle);
    BgPolygon clip_bg = toBg(clip_clean);
    std::string why;
    if (!validate(obs_bg, why) || !validate(clip_bg, why)) {
        out.push_back(obstacle);  // can't clip safely -> passthrough
        return out;
    }

    BgMultiPolygon clipped;
    bg::intersection(obs_bg, clip_bg, clipped);
    for (auto& p : clipped) {
        bg::correct(p);
        if (std::fabs(bg::area(p)) <= kMinValidArea) continue;
        Obstacle2D piece = bgToObstacle(p);
        if (piece.outer.size() >= 3) out.push_back(std::move(piece));
    }
    return out;
}

std::vector<Obstacle2D> subtractPolygonFromObstacle(const Obstacle2D& obstacle,
                                                    const Polygon2D& cutter) {
    std::vector<Obstacle2D> out;
    Polygon2D outer_clean = sanitize(obstacle.outer);
    if (outer_clean.size() < 3) return out;
    Polygon2D cut_clean = sanitize(cutter);
    if (cut_clean.size() < 3) { out.push_back(obstacle); return out; }

    BgPolygon obs_bg = toBg(obstacle);
    BgPolygon cut_bg = toBg(cut_clean);
    std::string why;
    if (!validate(obs_bg, why) || !validate(cut_bg, why)) {
        out.push_back(obstacle);  // can't cut safely -> leave obstacle intact
        return out;
    }

    BgMultiPolygon diff;
    bg::difference(obs_bg, cut_bg, diff);  // obstacle minus cutter
    for (auto& p : diff) {
        bg::correct(p);
        if (std::fabs(bg::area(p)) <= kMinValidArea) continue;
        Obstacle2D piece = bgToObstacle(p);
        if (piece.outer.size() >= 3) out.push_back(std::move(piece));
    }
    return out;  // empty => cutter fully covered the obstacle
}

PathValidation validatePathClearsObstacles(const std::vector<Point2D>& path_points,
                                            const std::vector<Obstacle2D>* obstacles,
                                            double obstacle_clearance) {
    PathValidation v;
    if (!obstacles || obstacles->empty() || path_points.size() < 2) return v;

    BgMultiPolygon inflated =
        unionInflatedObstacles(*obstacles, obstacle_clearance, nullptr);
    if (inflated.empty()) return v;

    // A swath that legitimately hugs the clearance edge touches the corridor
    // with ~zero length; only a connector driving *through* it accrues length.
    constexpr double kBreachLenEps = 0.05;  // meters

    for (size_t i = 1; i < path_points.size(); ++i) {
        BgLineString seg;
        seg.push_back(BgPoint(path_points[i - 1].x, path_points[i - 1].y));
        seg.push_back(BgPoint(path_points[i].x, path_points[i].y));

        BgMultiLineString breach;
        bg::intersection(seg, inflated, breach);

        double len = 0.0;
        for (const auto& ls : breach) len += bg::length(ls);
        if (len > kBreachLenEps) {
            ++v.crossing_segments;
            v.breach_length_m += len;
        }
    }
    v.valid = (v.crossing_segments == 0);
    return v;
}

std::vector<Point2D> routeConnectorThroughFreeSpace(
    const std::vector<Obstacle2D>& free_space,
    const Point2D& from,
    const Point2D& to) {
    BgMultiPolygon fs = toBgFreeSpace(free_space);
    if (fs.empty()) return {};

    const BgPoint a(from.x, from.y);
    const BgPoint b(to.x, to.y);

    // Fast path: direct line of sight needs no graph search.
    if (segmentInFreeSpace(a, b, fs)) return {from, to};

    // Nodes: from(0), to(1), then every region/hole vertex.
    std::vector<BgPoint> nodes;
    nodes.push_back(a);
    nodes.push_back(b);
    for (const auto& reg : free_space) {
        for (const auto& p : reg.outer) nodes.push_back(BgPoint(p.x, p.y));
        for (const auto& h : reg.holes)
            for (const auto& p : h) nodes.push_back(BgPoint(p.x, p.y));
    }
    const size_t n = nodes.size();

    std::vector<std::vector<std::pair<size_t, double>>> adj(n);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            if (segmentInFreeSpace(nodes[i], nodes[j], fs)) {
                const double w = dist(nodes[i], nodes[j]);
                adj[i].push_back({j, w});
                adj[j].push_back({i, w});
            }
        }
    }

    constexpr double kInf = std::numeric_limits<double>::max();
    std::vector<double> d(n, kInf);
    std::vector<size_t> prev(n, std::numeric_limits<size_t>::max());
    d[0] = 0.0;
    using QE = std::pair<double, size_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    pq.push({0.0, 0});
    while (!pq.empty()) {
        auto [du, u] = pq.top();
        pq.pop();
        if (du > d[u]) continue;
        if (u == 1) break;
        for (const auto& [v, w] : adj[u]) {
            if (d[u] + w < d[v]) {
                d[v] = d[u] + w;
                prev[v] = u;
                pq.push({d[v], v});
            }
        }
    }
    if (d[1] >= kInf) return {};  // disconnected components

    std::vector<Point2D> path;
    for (size_t cur = 1; cur != std::numeric_limits<size_t>::max(); cur = prev[cur]) {
        path.push_back(Point2D(bg::get<0>(nodes[cur]), bg::get<1>(nodes[cur])));
        if (cur == 0) break;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<Point2D> smoothPolylineWithinFreeSpace(
    const std::vector<Point2D>& polyline,
    const std::vector<Obstacle2D>& free_space,
    double max_radius,
    double min_radius) {
    if (polyline.size() < 3 || max_radius <= 0.0) return polyline;
    BgMultiPolygon fs = toBgFreeSpace(free_space);
    if (fs.empty()) return polyline;

    const double min_r = std::max(1e-3, min_radius);

    std::vector<Point2D> out;
    out.reserve(polyline.size() * 2);
    out.push_back(polyline.front());

    for (size_t i = 1; i + 1 < polyline.size(); ++i) {
        const Point2D& p0 = polyline[i - 1];
        const Point2D& p1 = polyline[i];
        const Point2D& p2 = polyline[i + 1];

        double v1x = p0.x - p1.x, v1y = p0.y - p1.y;
        double v2x = p2.x - p1.x, v2y = p2.y - p1.y;
        const double l1 = std::hypot(v1x, v1y);
        const double l2 = std::hypot(v2x, v2y);
        if (l1 < 1e-9 || l2 < 1e-9) {
            out.push_back(p1);
            continue;
        }
        v1x /= l1; v1y /= l1; v2x /= l2; v2y /= l2;

        const double cosang = std::clamp(v1x * v2x + v1y * v2y, -1.0, 1.0);
        const double angle = std::acos(cosang);  // interior angle at the corner
        if (angle > M_PI - 1e-3 || angle < 1e-3) {
            out.push_back(p1);  // nearly straight or a spike — nothing to round
            continue;
        }

        const double half = angle / 2.0;
        const double tan_half = std::tan(half);
        const double sin_half = std::sin(half);
        const double max_t = std::min(l1, l2) * 0.5;

        double bx = v1x + v2x, by = v1y + v2y;
        const double bl = std::hypot(bx, by);
        if (bl < 1e-9) {
            out.push_back(p1);
            continue;
        }
        bx /= bl; by /= bl;

        bool placed = false;
        for (double r = max_radius; r >= min_r - 1e-9; r *= 0.6) {
            const double t = r / tan_half;
            if (t > max_t) continue;  // doesn't fit segment lengths — shrink

            const Point2D t1{p1.x + v1x * t, p1.y + v1y * t};
            const Point2D t2{p1.x + v2x * t, p1.y + v2y * t};
            const double center_d = r / sin_half;
            const Point2D c{p1.x + bx * center_d, p1.y + by * center_d};

            std::vector<Point2D> arc = sampleArc(c, t1, t2, r);

            std::vector<Point2D> fillet;
            fillet.reserve(arc.size() + 2);
            fillet.push_back(t1);
            for (const auto& p : arc) fillet.push_back(p);
            fillet.push_back(t2);

            bool inside = true;
            for (size_t k = 1; k < fillet.size(); ++k) {
                if (!segmentInFreeSpace(BgPoint(fillet[k - 1].x, fillet[k - 1].y),
                                        BgPoint(fillet[k].x, fillet[k].y), fs)) {
                    inside = false;
                    break;
                }
            }
            if (inside) {
                for (const auto& p : fillet) out.push_back(p);
                placed = true;
                break;
            }
            if (r <= min_r + 1e-9) break;
        }
        if (!placed) out.push_back(p1);
    }

    out.push_back(polyline.back());
    return out;
}

FreeSpaceResult buildFreeSpacePolygons(const Polygon2D& boundary,
                                       const Polygon2D* roi,
                                       const std::vector<Obstacle2D>* obstacles,
                                       double obstacle_clearance,
                                       double min_region_area_m2) {
    FreeSpaceResult r;

    Polygon2D bclean = sanitize(boundary);
    if (bclean.size() < 3) {
        r.error_message = "Boundary polygon is too small (need >= 3 vertices)";
        return r;
    }
    BgPolygon boundary_bg = toBg(bclean);
    {
        std::string why;
        if (!validate(boundary_bg, why)) {
            r.error_message = "Boundary polygon is invalid: " + why;
            return r;
        }
    }

    BgMultiPolygon work;
    if (roi && !roi->empty()) {
        Polygon2D rclean = sanitize(*roi);
        if (rclean.size() < 3) {
            r.error_message = "ROI polygon is too small (need >= 3 vertices)";
            return r;
        }
        BgPolygon roi_bg = toBg(rclean);
        {
            std::string why;
            if (!validate(roi_bg, why)) {
                r.error_message = "ROI polygon is invalid: " + why;
                return r;
            }
        }
        bg::intersection(boundary_bg, roi_bg, work);
    } else {
        work.push_back(boundary_bg);
    }

    {
        BgMultiPolygon filtered;
        for (auto& p : work) {
            bg::correct(p);
            if (std::fabs(bg::area(p)) > kMinValidArea) filtered.push_back(p);
        }
        work = std::move(filtered);
    }
    if (work.empty()) {
        r.error_message = "ROI does not intersect the boundary (effective area is empty)";
        return r;
    }

    if (obstacles && !obstacles->empty()) {
        BgMultiPolygon inflated =
            unionInflatedObstacles(*obstacles, obstacle_clearance, &r.skipped_obstacles);

        if (!inflated.empty()) {
            BgMultiPolygon after;
            bg::difference(work, inflated, after);
            BgMultiPolygon kept;
            for (auto& p : after) {
                bg::correct(p);
                if (std::fabs(bg::area(p)) > kMinValidArea) kept.push_back(p);
            }
            work = std::move(kept);
            if (work.empty()) {
                r.error_message = "Obstacles removed all usable area";
                return r;
            }
        }
    }

    // Discard unscannable slivers: free-space components below the navigable
    // area floor (typically the robot footprint). This stops edge pinch-offs
    // and crumbs between clutter from being treated as real coverage regions.
    const double area_floor = std::max(kMinValidArea, min_region_area_m2);
    r.effective_area_m2 = 0.0;
    r.regions.reserve(work.size());
    for (const auto& p : work) {
        const double a = std::fabs(bg::area(p));
        if (a < area_floor) continue;
        r.effective_area_m2 += a;
        Obstacle2D reg = bgToObstacle(p);
        if (reg.outer.size() >= 3) r.regions.push_back(std::move(reg));
    }
    if (r.regions.empty()) {
        r.error_message = "Effective area is empty";
        return r;
    }
    r.success = true;
    return r;
}

}  // namespace f2c_cpp
