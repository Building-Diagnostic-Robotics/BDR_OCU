#pragma once

#include "coverage_pipeline.hpp"

#include <algorithm>
#include <cmath>

namespace f2c_cpp {

inline bool pointInPolygonRayCastLocal(const Point2D& p, const Polygon2D& poly) {
    if (poly.size() < 3) return false;
    bool inside = false;
    double x = p.x;
    double y = p.y;
    Point2D p0 = poly.back();
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point2D& p1 = poly[i];
        bool intersects = ((p1.y > y) != (p0.y > y)) &&
            (x < (p0.x - p1.x) * (y - p1.y) / ((p0.y - p1.y) + 1e-12) + p1.x);
        if (intersects) {
            inside = !inside;
        }
        p0 = p1;
    }
    return inside;
}

inline bool pointInObstacleShapeLocal(const Point2D& p, const Obstacle2D& obs) {
    if (!pointInPolygonRayCastLocal(p, obs.outer)) {
        return false;
    }
    for (const auto& hole : obs.holes) {
        if (pointInPolygonRayCastLocal(p, hole)) {
            return false;
        }
    }
    return true;
}

inline double orient2DLocal(const Point2D& a, const Point2D& b, const Point2D& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

inline bool onSegmentLocal(const Point2D& a, const Point2D& b, const Point2D& p) {
    const double eps = 1e-12;
    if (std::abs(orient2DLocal(a, b, p)) > eps) return false;
    return (p.x >= std::min(a.x, b.x) - eps && p.x <= std::max(a.x, b.x) + eps &&
            p.y >= std::min(a.y, b.y) - eps && p.y <= std::max(a.y, b.y) + eps);
}

inline bool segmentsIntersectLocal(const Point2D& a, const Point2D& b, const Point2D& c,
                                   const Point2D& d) {
    const double o1 = orient2DLocal(a, b, c);
    const double o2 = orient2DLocal(a, b, d);
    const double o3 = orient2DLocal(c, d, a);
    const double o4 = orient2DLocal(c, d, b);

    auto sgn = [](double v) -> int {
        const double eps = 1e-12;
        if (v > eps) return 1;
        if (v < -eps) return -1;
        return 0;
    };

    const int s1 = sgn(o1);
    const int s2 = sgn(o2);
    const int s3 = sgn(o3);
    const int s4 = sgn(o4);

    if (s1 * s2 < 0 && s3 * s4 < 0) {
        return true;
    }

    if (s1 == 0 && onSegmentLocal(a, b, c)) return true;
    if (s2 == 0 && onSegmentLocal(a, b, d)) return true;
    if (s3 == 0 && onSegmentLocal(c, d, a)) return true;
    if (s4 == 0 && onSegmentLocal(c, d, b)) return true;

    return false;
}

inline bool polygonEdgesIntersectLocal(const Polygon2D& a, const Polygon2D& b) {
    if (a.size() < 2 || b.size() < 2) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const Point2D& a0 = a[i];
        const Point2D& a1 = a[(i + 1) % a.size()];
        for (size_t j = 0; j < b.size(); ++j) {
            const Point2D& b0 = b[j];
            const Point2D& b1 = b[(j + 1) % b.size()];
            if (segmentsIntersectLocal(a0, a1, b0, b1)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace f2c_cpp
