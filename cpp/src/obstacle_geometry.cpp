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
#include <memory>
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
BgMultiPolygon buffered(const Geom& g, double dist, bool sharp = false) {
    BgMultiPolygon out;
    bg::strategy::buffer::distance_symmetric<double> ds(dist);
    bg::strategy::buffer::end_round er(16);
    bg::strategy::buffer::point_circle pc(16);
    bg::strategy::buffer::side_straight ss;
    if (sharp) {
        // Miter joins keep obstacle corners sharp so connectors routed around
        // them are straight legs. miter_limit caps acute-corner spikes (beyond
        // it Boost bevels the join) so a thin spike obstacle can't grow an
        // unbounded needle of blocked space.
        bg::strategy::buffer::join_miter jm(2.0);
        bg::buffer(g, out, ds, ss, jm, er, pc);
    } else {
        bg::strategy::buffer::join_round jr(16);
        bg::buffer(g, out, ds, ss, jr, er, pc);
    }
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
                                      double clearance, int* skipped,
                                      bool sharp = false) {
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
    if (!u.empty() && clearance > 0.0) u = buffered(u, clearance, sharp);
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

namespace {
// Douglas-Peucker simplify one closed ring; never returns < 3 vertices (falls
// back to the sanitized input). Strips sub-tolerance contour noise so the
// visibility graph stays small.
Polygon2D simplifyRing(const Polygon2D& ring, double tol) {
    Polygon2D clean = sanitize(ring);
    if (tol <= 0.0 || clean.size() < 4) return clean;
    BgLineString ls;
    for (const auto& p : clean) ls.push_back(BgPoint(p.x, p.y));
    ls.push_back(BgPoint(clean.front().x, clean.front().y));  // close the ring
    BgLineString out;
    bg::simplify(ls, out, tol);
    Polygon2D res;
    res.reserve(out.size());
    for (const auto& pt : out) res.emplace_back(bg::get<0>(pt), bg::get<1>(pt));
    if (res.size() >= 2 && nearPoint(res.front(), res.back())) res.pop_back();
    return res.size() >= 3 ? res : clean;
}
}  // namespace

struct FreeSpaceConnectorRouter::Impl {
    BgMultiPolygon fs;            // simplified free space (LOS checks)
    std::vector<BgPoint> nodes;   // static graph vertices
    std::vector<std::vector<std::pair<int, double>>> adj;  // static-static edges
};

FreeSpaceConnectorRouter::FreeSpaceConnectorRouter(
    const std::vector<Obstacle2D>& free_space, double simplify_tol_m)
    : impl_(std::make_unique<Impl>()) {
    std::vector<Obstacle2D> simplified;
    simplified.reserve(free_space.size());
    for (const auto& reg : free_space) {
        Obstacle2D s;
        s.outer = simplifyRing(reg.outer, simplify_tol_m);
        if (s.outer.size() < 3) continue;
        for (const auto& h : reg.holes) {
            Polygon2D hs = simplifyRing(h, simplify_tol_m);
            if (hs.size() >= 3) s.holes.push_back(std::move(hs));
        }
        simplified.push_back(std::move(s));
    }

    impl_->fs = toBgFreeSpace(simplified);
    if (impl_->fs.empty()) return;

    for (const auto& reg : simplified) {
        for (const auto& p : reg.outer) impl_->nodes.emplace_back(p.x, p.y);
        for (const auto& h : reg.holes)
            for (const auto& p : h) impl_->nodes.emplace_back(p.x, p.y);
    }

    const int n = static_cast<int>(impl_->nodes.size());
    impl_->adj.assign(n, {});
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (segmentInFreeSpace(impl_->nodes[i], impl_->nodes[j], impl_->fs)) {
                const double w = dist(impl_->nodes[i], impl_->nodes[j]);
                impl_->adj[i].push_back({j, w});
                impl_->adj[j].push_back({i, w});
            }
        }
    }
}

FreeSpaceConnectorRouter::~FreeSpaceConnectorRouter() = default;

bool FreeSpaceConnectorRouter::valid() const { return impl_ && !impl_->fs.empty(); }

std::vector<Point2D> FreeSpaceConnectorRouter::route(const Point2D& from,
                                                     const Point2D& to) const {
    if (!valid()) return {};
    const BgPoint a(from.x, from.y), b(to.x, to.y);
    if (segmentInFreeSpace(a, b, impl_->fs)) return {from, to};

    const int n = static_cast<int>(impl_->nodes.size());
    const int A = n, B = n + 1, N = n + 2;

    // Endpoint-to-static visibility (the only dynamic edges per query).
    std::vector<char> visA(n, 0), visB(n, 0);
    std::vector<double> wA(n, 0.0), wB(n, 0.0);
    for (int i = 0; i < n; ++i) {
        if (segmentInFreeSpace(a, impl_->nodes[i], impl_->fs)) {
            visA[i] = 1;
            wA[i] = dist(a, impl_->nodes[i]);
        }
        if (segmentInFreeSpace(b, impl_->nodes[i], impl_->fs)) {
            visB[i] = 1;
            wB[i] = dist(b, impl_->nodes[i]);
        }
    }

    constexpr double kInf = std::numeric_limits<double>::max();
    std::vector<double> d(N, kInf);
    std::vector<int> prev(N, -1);
    d[A] = 0.0;
    using QE = std::pair<double, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    pq.push({0.0, A});

    auto relax = [&](int u, int v, double w) {
        if (d[u] + w < d[v]) {
            d[v] = d[u] + w;
            prev[v] = u;
            pq.push({d[v], v});
        }
    };

    while (!pq.empty()) {
        auto [du, u] = pq.top();
        pq.pop();
        if (du > d[u]) continue;
        if (u == B) break;
        if (u < n) {
            for (const auto& [v, w] : impl_->adj[u]) relax(u, v, w);
            if (visA[u]) relax(u, A, wA[u]);
            if (visB[u]) relax(u, B, wB[u]);
        } else if (u == A) {
            for (int i = 0; i < n; ++i)
                if (visA[i]) relax(A, i, wA[i]);
        }
    }
    if (d[B] >= kInf) return {};  // disconnected components

    std::vector<Point2D> path;
    for (int cur = B; cur != -1; cur = prev[cur]) {
        if (cur == A) path.push_back(from);
        else if (cur == B) path.push_back(to);
        else path.emplace_back(bg::get<0>(impl_->nodes[cur]), bg::get<1>(impl_->nodes[cur]));
    }
    std::reverse(path.begin(), path.end());
    return path;
}

FreeSpaceResult buildFreeSpacePolygons(const Polygon2D& boundary,
                                       const Polygon2D* roi,
                                       const std::vector<Obstacle2D>* obstacles,
                                       double obstacle_clearance,
                                       double min_region_area_m2,
                                       bool sharp_corners) {
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
        BgMultiPolygon inflated = unionInflatedObstacles(
            *obstacles, obstacle_clearance, &r.skipped_obstacles, sharp_corners);

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
