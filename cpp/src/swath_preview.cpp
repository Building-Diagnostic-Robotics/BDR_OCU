/**
 * @file swath_preview.cpp
 * @brief See swath_preview.hpp.
 */

#include "swath_preview.hpp"

#include "satellite_geo_math.hpp"

#include <algorithm>
#include <cmath>

namespace f2c_cpp {
namespace {

constexpr double kEps = 1e-7;
constexpr double kInf = 1e6;

struct Iv {
    double lo = 0;
    double hi = 0;
    bool empty() const { return hi - lo <= kEps; }
};

double dot(const QPointF& a, const QPointF& b) {
    return QPointF::dotProduct(a, b);
}

QVector<Iv> mergeIntervals(QVector<Iv> in) {
    QVector<Iv> out;
    std::sort(in.begin(), in.end(),
              [](const Iv& a, const Iv& b) { return a.lo < b.lo; });
    for (const Iv& iv : in) {
        if (iv.empty()) {
            continue;
        }
        if (out.isEmpty() || iv.lo > out.last().hi + 1e-5) {
            out.append(iv);
        } else {
            out.last().hi = std::max(out.last().hi, iv.hi);
        }
    }
    return out;
}

QVector<Iv> clipIntervals(const QVector<Iv>& src, const Iv& window) {
    QVector<Iv> out;
    if (window.empty()) {
        return out;
    }
    for (const Iv& iv : src) {
        const Iv cut{std::max(iv.lo, window.lo), std::min(iv.hi, window.hi)};
        if (!cut.empty()) {
            out.append(cut);
        }
    }
    return out;
}

QVector<Iv> subtractIntervals(const QVector<Iv>& base, QVector<Iv> cuts) {
    cuts = mergeIntervals(std::move(cuts));
    QVector<Iv> out;
    for (const Iv& span : base) {
        double cursor = span.lo;
        for (const Iv& cut : cuts) {
            if (cut.hi <= cursor + kEps) {
                continue;
            }
            if (cut.lo >= span.hi - kEps) {
                break;
            }
            if (cut.lo > cursor + kEps) {
                out.append(Iv{cursor, std::min(cut.lo, span.hi)});
            }
            cursor = std::max(cursor, cut.hi);
            if (cursor >= span.hi - kEps) {
                break;
            }
        }
        if (cursor < span.hi - kEps) {
            out.append(Iv{cursor, span.hi});
        }
    }
    return out;
}

QVector<Iv> solveLessEqual(double a, double b, double c) {
    QVector<Iv> out;
    if (std::abs(a) < 1e-12) {
        if (std::abs(b) < 1e-12) {
            if (c <= kEps) {
                out.append(Iv{-kInf, kInf});
            }
            return out;
        }
        const double t = -c / b;
        out.append(b > 0 ? Iv{-kInf, t} : Iv{t, kInf});
        return out;
    }
    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) {
        if (a < 0.0) {
            out.append(Iv{-kInf, kInf});
        }
        return out;
    }
    const double root = std::sqrt(disc);
    double r1 = (-b - root) / (2.0 * a);
    double r2 = (-b + root) / (2.0 * a);
    if (r1 > r2) {
        std::swap(r1, r2);
    }
    if (a > 0.0) {
        out.append(Iv{r1, r2});
    } else {
        out.append(Iv{-kInf, r1});
        out.append(Iv{r2, kInf});
    }
    return out;
}

Iv whereParam(double limit, bool at_most, double slope, double intercept,
              double length_sq) {
    // slope * t + intercept  ?  limit * length_sq
    const double rhs = limit * length_sq - intercept;
    if (std::abs(slope) < 1e-12) {
        const double limit_scaled = limit * length_sq;
        const bool holds = at_most ? intercept <= limit_scaled + 1e-8
                                   : intercept >= limit_scaled - 1e-8;
        return holds ? Iv{-kInf, kInf} : Iv{1.0, 0.0};
    }
    const double t = rhs / slope;
    const bool toward_positive = at_most ? slope > 0.0 : slope < 0.0;
    return toward_positive ? Iv{-kInf, t} : Iv{t, kInf};
}

Iv intersection(const Iv& a, const Iv& b) {
    return Iv{std::max(a.lo, b.lo), std::min(a.hi, b.hi)};
}

QVector<Iv> bufferOnLane(const QPointF& origin, const QPointF& direction,
                         const QPointF& a, const QPointF& b, double radius) {
    const QPointF edge = b - a;
    const double length_sq = dot(edge, edge);
    if (radius <= 0.0) {
        return {};
    }
    if (length_sq < kEps) {
        const QPointF offset = origin - a;
        const QVector<Iv> disk = solveLessEqual(
            1.0, 2.0 * dot(offset, direction),
            dot(offset, offset) - radius * radius);
        return disk;
    }
    const QPointF from_a = origin - a;
    const double along = dot(direction, edge);
    const double base = dot(from_a, edge);

    const auto disk = [&](const QPointF& centre) {
        const QPointF offset = origin - centre;
        return solveLessEqual(1.0, 2.0 * dot(offset, direction),
                              dot(offset, offset) - radius * radius);
    };
    QVector<Iv> forbidden = clipIntervals(
        disk(a), whereParam(0.0, /*at_most=*/true, along, base, length_sq));
    forbidden += clipIntervals(
        disk(b), whereParam(1.0, /*at_most=*/false, along, base, length_sq));

    // Perpendicular distance to the infinite line, only where the foot
    // lands on the segment itself.
    const double t2 = 1.0 - (along * along) / length_sq;
    const double t1 =
        2.0 * dot(from_a, direction) - 2.0 * along * base / length_sq;
    const double t0 =
        dot(from_a, from_a) - (base * base) / length_sq - radius * radius;
    const Iv on_segment = intersection(
        whereParam(0.0, /*at_most=*/false, along, base, length_sq),
        whereParam(1.0, /*at_most=*/true, along, base, length_sq));
    forbidden += clipIntervals(solveLessEqual(t2, t1, t0), on_segment);
    return mergeIntervals(std::move(forbidden));
}

bool pointInRing(const QVector<QPointF>& ring, const QPointF& point) {
    bool inside = false;
    const int n = ring.size();
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const QPointF& a = ring[j];
        const QPointF& b = ring[i];
        const bool crosses = (a.y() > point.y()) != (b.y() > point.y());
        if (!crosses || std::abs(b.y() - a.y()) < kEps) {
            continue;
        }
        const double x = (b.x() - a.x()) * (point.y() - a.y()) / (b.y() - a.y()) +
                         a.x();
        if (point.x() < x) {
            inside = !inside;
        }
    }
    return inside;
}

QVector<Iv> clipLane(const QVector<QPointF>& ring, const QPointF& normal,
                     const QPointF& direction, double level) {
    QVector<double> hits;
    const int n = ring.size();
    for (int i = 0; i < n; ++i) {
        const QPointF& a = ring[i];
        const QPointF& b = ring[(i + 1) % n];
        const double sa = dot(a, normal) - level;
        const double sb = dot(b, normal) - level;
        if ((sa < 0.0) == (sb < 0.0)) {
            continue;
        }
        const double denom = sa - sb;
        if (std::abs(denom) < kEps) {
            continue;
        }
        const QPointF hit = a + (sa / denom) * (b - a);
        hits.append(dot(hit, direction));
    }
    std::sort(hits.begin(), hits.end());
    QVector<double> unique;
    for (double hit : hits) {
        if (unique.isEmpty() || hit - unique.last() > 1e-4) {
            unique.append(hit);
        }
    }
    QVector<Iv> inside;
    for (int i = 0; i + 1 < unique.size(); ++i) {
        if (unique[i + 1] - unique[i] < 1e-3) {
            continue;
        }
        const double mid = 0.5 * (unique[i] + unique[i + 1]);
        if (pointInRing(ring, normal * level + direction * mid)) {
            inside.append(Iv{unique[i], unique[i + 1]});
        }
    }
    return inside;
}

}  // namespace

SwathPreview previewSwaths(const SwathPreviewRequest& request) {
    SwathPreview out;
    if (!request.marker_valid || request.width_m <= 0.0 ||
        request.ring_enu.size() < 3) {
        return out;
    }
    QVector<QPointF> ring;
    ring.reserve(request.ring_enu.size());
    for (const QPointF& enu : request.ring_enu) {
        ring.append(geo::bodyFromEnu(enu, request.heading_deg));
    }
    if (QLineF(ring.first(), ring.last()).length() < kEps) {
        ring.removeLast();
    }
    if (ring.size() < 3) {
        return out;
    }

    double min_x = ring[0].x();
    double max_x = min_x;
    double min_y = ring[0].y();
    double max_y = min_y;
    for (const QPointF& point : ring) {
        min_x = std::min(min_x, point.x());
        max_x = std::max(max_x, point.x());
        min_y = std::min(min_y, point.y());
        max_y = std::max(max_y, point.y());
    }
    // The director's rule: the longer side of the body-frame box is the
    // sweep direction. A tie (or a longer left-right span) sweeps along
    // left; otherwise along forward.
    const bool along_left = (max_y - min_y) >= (max_x - min_x);
    const QPointF normal = along_left ? QPointF(1, 0) : QPointF(0, 1);
    const QPointF direction = along_left ? QPointF(0, 1) : QPointF(1, 0);
    const double low = along_left ? min_x : min_y;
    const double high = along_left ? max_x : max_y;
    const double minimum = previewMinFragmentM(request.width_m);
    const int n = ring.size();

    for (double level = low + 0.5 * request.width_m; level < high - kEps;
         level += request.width_m) {
        QVector<Iv> pieces = clipLane(ring, normal, direction, level);
        if (pieces.isEmpty()) {
            continue;
        }
        const QPointF origin = normal * level;
        QVector<Iv> forbidden;
        for (int i = 0; i < n; ++i) {
            const bool roof =
                i < request.roof_edges.size() && request.roof_edges[i];
            forbidden += bufferOnLane(origin, direction, ring[i],
                                      ring[(i + 1) % n],
                                      previewEdgeClearanceM(roof));
        }
        pieces = subtractIntervals(pieces, std::move(forbidden));
        bool emitted = false;
        for (const Iv& piece : pieces) {
            if (piece.hi - piece.lo < minimum) {
                continue;
            }
            const QPointF start = geo::enuFromBody(
                origin + direction * piece.lo, request.heading_deg);
            const QPointF end = geo::enuFromBody(
                origin + direction * piece.hi, request.heading_deg);
            out.segments.append(QLineF(start, end));
            emitted = true;
        }
        if (emitted) {
            ++out.passes;
        }
    }
    return out;
}

}  // namespace f2c_cpp
