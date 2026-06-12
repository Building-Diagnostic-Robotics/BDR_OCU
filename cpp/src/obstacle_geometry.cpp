/**
 * @file obstacle_geometry.cpp
 * @brief Boost.Geometry obstacle/free-space core (no PCL / Fields2Cover deps).
 *
 * Unit-tested in isolation via tests/obstacle_geometry_tests.cpp.
 */

#include "coverage_pipeline.hpp"

#include <cmath>
#include <iostream>
#include <string>

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/policies/is_valid/failing_reason_policy.hpp>

namespace f2c_cpp {
namespace {
namespace bg = boost::geometry;

using BgPoint = bg::model::d2::point_xy<double>;
using BgPolygon = bg::model::polygon<BgPoint, /*ClockWise=*/false, /*Closed=*/true>;
using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;

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

FreeSpaceResult buildFreeSpacePolygons(const Polygon2D& boundary,
                                       const Polygon2D* roi,
                                       const std::vector<Obstacle2D>* obstacles,
                                       double obstacle_clearance) {
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
        BgMultiPolygon obstacles_union;
        for (size_t i = 0; i < obstacles->size(); ++i) {
            BgMultiPolygon parts;
            std::string why;
            if (!repairObstacle(obstacles->at(i), parts, why)) {
                ++r.skipped_obstacles;
                std::cerr << "[Coverage] Skipping obstacle #" << (i + 1) << " (" << why << ")\n";
                continue;
            }
            for (const auto& p : parts) {
                BgMultiPolygon merged;
                bg::union_(obstacles_union, p, merged);
                for (auto& q : merged) bg::correct(q);
                obstacles_union = std::move(merged);
            }
        }

        if (!obstacles_union.empty()) {
            BgMultiPolygon inflated = obstacles_union;
            if (obstacle_clearance > 0.0) inflated = buffered(obstacles_union, obstacle_clearance);

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

    r.effective_area_m2 = 0.0;
    r.regions.reserve(work.size());
    for (const auto& p : work) {
        r.effective_area_m2 += std::fabs(bg::area(p));
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
