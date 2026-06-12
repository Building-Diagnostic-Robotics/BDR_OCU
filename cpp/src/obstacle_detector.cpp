/**
 * @file obstacle_detector.cpp
 * @brief Implementation of automatic obstacle detection (CSF ground model).
 *
 * Ground/non-ground segmentation uses the official Cloth Simulation Filter
 * (CSF, Apache-2.0, vendored at external/csf). Non-ground points within a
 * traversable clearance band become obstacle candidates; points swept by the
 * driven robot footprint are removed, and the survivors are polygonized on an
 * occupancy grid.
 */

#include "obstacle_detector.hpp"

#include "CSF.h"

#include <pcl/kdtree/kdtree_flann.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace f2c_cpp {

namespace {

static double signedArea2D(const Polygon2D& ring) {
    if (ring.size() < 3) return 0.0;
    double a = 0.0;
    for (size_t i = 0; i < ring.size(); ++i) {
        const auto& p0 = ring[i];
        const auto& p1 = ring[(i + 1) % ring.size()];
        a += (p0.x * p1.y - p1.x * p0.y);
    }
    return 0.5 * a;
}

static void ensureCCW(Polygon2D& ring) {
    if (signedArea2D(ring) < 0.0) {
        std::reverse(ring.begin(), ring.end());
    }
}

static void ensureCW(Polygon2D& ring) {
    if (signedArea2D(ring) > 0.0) {
        std::reverse(ring.begin(), ring.end());
    }
}

static bool pointInPolyRayCast(const Point2D& p, const Polygon2D& poly) {
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

static Polygon2D effectiveScopePolygon(const Polygon2D* roi_or_boundary) {
    if (roi_or_boundary && roi_or_boundary->size() >= 3) {
        return *roi_or_boundary;
    }
    return {};
}

static PointCloudPtr filterCloudToPolygon(const PointCloudPtr& cloud, const Polygon2D& scope) {
    if (!cloud) return PointCloudPtr(new PointCloud);
    if (scope.size() < 3) {
        return cloud;
    }
    PointCloudPtr out(new PointCloud);
    out->reserve(cloud->size());
    for (const auto& pt : cloud->points) {
        if (pointInPolyRayCast(Point2D(pt.x, pt.y), scope)) {
            out->push_back(pt);
        }
    }
    return out;
}

static std::vector<PathState> filterPathToPolygon(const std::vector<PathState>& path, const Polygon2D& scope) {
    if (scope.size() < 3) return path;
    std::vector<PathState> out;
    out.reserve(path.size());
    for (const auto& st : path) {
        if (pointInPolyRayCast(st.point, scope)) {
            out.push_back(st);
        }
    }
    if (out.empty()) return path;
    return out;
}

// --------------------- Occupancy grid & contour rings -----------------------

struct OccGrid {
    int w = 0;
    int h = 0;
    double xmin = 0.0;
    double ymin = 0.0;
    double cell = 0.09;
    std::vector<uint8_t> occ;  // row-major (y then x), 1=occupied
};

static inline bool occAt(const OccGrid& g, int x, int y) {
    if (x < 0 || y < 0 || x >= g.w || y >= g.h) return false;
    return g.occ[static_cast<size_t>(y) * static_cast<size_t>(g.w) + static_cast<size_t>(x)] != 0;
}

static OccGrid occupancyFromPoints(const std::vector<Point2D>& pts, double cell, double padding) {
    OccGrid g;
    g.cell = cell;
    if (pts.empty() || cell <= 0) {
        return g;
    }
    double minx = pts[0].x, maxx = pts[0].x;
    double miny = pts[0].y, maxy = pts[0].y;
    for (const auto& p : pts) {
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }
    g.xmin = minx - padding;
    double xmax = maxx + padding;
    g.ymin = miny - padding;
    double ymax = maxy + padding;

    g.w = std::max(1, static_cast<int>(std::ceil((xmax - g.xmin) / cell)));
    g.h = std::max(1, static_cast<int>(std::ceil((ymax - g.ymin) / cell)));
    g.occ.assign(static_cast<size_t>(g.w) * static_cast<size_t>(g.h), 0);

    for (const auto& p : pts) {
        int ix = static_cast<int>(std::floor((p.x - g.xmin) / cell));
        int iy = static_cast<int>(std::floor((p.y - g.ymin) / cell));
        ix = std::max(0, std::min(g.w - 1, ix));
        iy = std::max(0, std::min(g.h - 1, iy));
        g.occ[static_cast<size_t>(iy) * static_cast<size_t>(g.w) + static_cast<size_t>(ix)] = 1;
    }
    return g;
}

static OccGrid inflateOccupancy(const OccGrid& in, double radius_m) {
    if (in.w <= 0 || in.h <= 0 || in.occ.empty()) return in;
    if (radius_m <= 1e-9) return in;

    int r = static_cast<int>(std::ceil(radius_m / in.cell));
    if (r <= 0) return in;

    std::vector<std::pair<int, int>> offsets;
    offsets.reserve(static_cast<size_t>((2 * r + 1) * (2 * r + 1)));
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy <= r * r) {
                offsets.emplace_back(dx, dy);
            }
        }
    }

    OccGrid out = in;
    std::fill(out.occ.begin(), out.occ.end(), 0);

    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            if (!occAt(in, x, y)) continue;
            for (const auto& [dx, dy] : offsets) {
                int nx = x + dx;
                int ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= in.w || ny >= in.h) continue;
                out.occ[static_cast<size_t>(ny) * static_cast<size_t>(in.w) + static_cast<size_t>(nx)] = 1;
            }
        }
    }
    return out;
}

static std::vector<std::pair<int, int>> circleOffsets(int r) {
    std::vector<std::pair<int, int>> offsets;
    if (r <= 0) return offsets;
    offsets.reserve(static_cast<size_t>((2 * r + 1) * (2 * r + 1)));
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy <= r * r) {
                offsets.emplace_back(dx, dy);
            }
        }
    }
    return offsets;
}

static OccGrid erodeOccupancy(const OccGrid& in, double radius_m) {
    if (in.w <= 0 || in.h <= 0 || in.occ.empty()) return in;
    if (radius_m <= 1e-9) return in;

    int r = static_cast<int>(std::ceil(radius_m / in.cell));
    if (r <= 0) return in;

    const auto offsets = circleOffsets(r);
    if (offsets.empty()) return in;

    OccGrid out = in;
    std::fill(out.occ.begin(), out.occ.end(), 0);

    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            bool keep = true;
            for (const auto& [dx, dy] : offsets) {
                const int nx = x + dx;
                const int ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= in.w || ny >= in.h) {
                    keep = false;
                    break;
                }
                if (!occAt(in, nx, ny)) {
                    keep = false;
                    break;
                }
            }
            if (keep) {
                out.occ[static_cast<size_t>(y) * static_cast<size_t>(in.w) + static_cast<size_t>(x)] = 1;
            }
        }
    }
    return out;
}

static OccGrid closeOccupancyConservative(const OccGrid& in, double radius_m) {
    // Morphological closing = dilation then erosion.
    if (radius_m <= 1e-9) return in;
    OccGrid dil = inflateOccupancy(in, radius_m);
    OccGrid clo = erodeOccupancy(dil, radius_m);
    return clo;
}

static OccGrid maxpoolOccupancy(const OccGrid& in, int factor) {
    if (in.w <= 0 || in.h <= 0 || in.occ.empty()) return in;
    if (factor <= 1) return in;

    const int pad_w = (factor - (in.w % factor)) % factor;
    const int pad_h = (factor - (in.h % factor)) % factor;
    const int w2 = in.w + pad_w;
    const int h2 = in.h + pad_h;

    std::vector<uint8_t> padded(static_cast<size_t>(w2) * static_cast<size_t>(h2), 0);
    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            padded[static_cast<size_t>(y) * static_cast<size_t>(w2) + static_cast<size_t>(x)] =
                in.occ[static_cast<size_t>(y) * static_cast<size_t>(in.w) + static_cast<size_t>(x)];
        }
    }

    const int out_w = w2 / factor;
    const int out_h = h2 / factor;
    OccGrid out;
    out.xmin = in.xmin;
    out.ymin = in.ymin;
    out.cell = in.cell * static_cast<double>(factor);
    out.w = out_w;
    out.h = out_h;
    out.occ.assign(static_cast<size_t>(out_w) * static_cast<size_t>(out_h), 0);

    for (int by = 0; by < out_h; ++by) {
        for (int bx = 0; bx < out_w; ++bx) {
            uint8_t mx = 0;
            const int y0 = by * factor;
            const int x0 = bx * factor;
            for (int dy = 0; dy < factor && mx == 0; ++dy) {
                for (int dx = 0; dx < factor; ++dx) {
                    const int ix = x0 + dx;
                    const int iy = y0 + dy;
                    mx = std::max<uint8_t>(mx, padded[static_cast<size_t>(iy) * static_cast<size_t>(w2) + static_cast<size_t>(ix)]);
                    if (mx) break;
                }
            }
            out.occ[static_cast<size_t>(by) * static_cast<size_t>(out_w) + static_cast<size_t>(bx)] = mx;
        }
    }
    return out;
}

// Forward declarations (used by helper routines below)
static std::vector<Polygon2D> extractContourRingsFromOcc(const OccGrid& g);
static std::vector<std::pair<Polygon2D, std::vector<Polygon2D>>> groupRingsIntoShapes(
    std::vector<Polygon2D> rings,
    double min_area_m2);

static std::vector<std::pair<Polygon2D, std::vector<Polygon2D>>> polygonizeClusterGrid(
    const std::vector<Point2D>& pts2d,
    double grid_cell_m,
    double contour_cell_m,
    double inflate_radius_m,
    double smooth_radius_m,
    double min_contour_area_m2) {
    if (pts2d.empty() || grid_cell_m <= 0.0) {
        return {};
    }

    const double padding = std::max(0.20, 2.0 * std::max(0.0, inflate_radius_m));
    OccGrid occ = occupancyFromPoints(pts2d, grid_cell_m, padding);
    occ = inflateOccupancy(occ, std::max(0.0, inflate_radius_m));
    occ = closeOccupancyConservative(occ, std::max(0.0, smooth_radius_m));

    // Optional coarser contour grid (conservative max-pooling)
    if (contour_cell_m > 0.0 && contour_cell_m > grid_cell_m + 1e-12) {
        const int factor = std::max(1, static_cast<int>(std::round(contour_cell_m / grid_cell_m)));
        if (factor > 1) {
            occ = maxpoolOccupancy(occ, factor);
        }
    }

    std::vector<Polygon2D> rings = extractContourRingsFromOcc(occ);
    return groupRingsIntoShapes(std::move(rings), min_contour_area_m2);
}

struct Edge {
    int x0, y0;
    int x1, y1;
};

static inline uint64_t packV(int x, int y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
}

// Split a traced boundary loop into simple sub-loops at any revisited vertex.
// A diagonal saddle (NW+SE or NE+SW occupied) makes the tracer pass through one
// grid corner twice, yielding a self-touching ring that Boost flags invalid.
// Peeling each closed sub-loop off a stack guarantees simple polygons at the
// source instead of relying solely on downstream buffer repair.
static std::vector<std::vector<std::pair<int, int>>> splitSimpleLoops(
    const std::vector<std::pair<int, int>>& seq) {
    std::vector<std::vector<std::pair<int, int>>> loops;
    std::vector<std::pair<int, int>> stack;
    std::unordered_map<uint64_t, int> pos;
    for (const auto& v : seq) {
        const uint64_t k = packV(v.first, v.second);
        auto it = pos.find(k);
        if (it != pos.end()) {
            const int idx = it->second;
            std::vector<std::pair<int, int>> loop(stack.begin() + idx, stack.end());
            if (loop.size() >= 3) loops.push_back(std::move(loop));
            for (int t = idx; t < static_cast<int>(stack.size()); ++t) {
                pos.erase(packV(stack[t].first, stack[t].second));
            }
            stack.resize(idx);
        }
        pos[k] = static_cast<int>(stack.size());
        stack.push_back(v);
    }
    if (stack.size() >= 3) loops.push_back(std::move(stack));
    return loops;
}

static std::vector<Polygon2D> extractContourRingsFromOcc(const OccGrid& g) {
    std::vector<Edge> edges;
    edges.reserve(static_cast<size_t>(g.w) * static_cast<size_t>(g.h));

    // Build directed boundary edges with "occupied" on the left.
    for (int y = 0; y < g.h; ++y) {
        for (int x = 0; x < g.w; ++x) {
            if (!occAt(g, x, y)) continue;

            int vx0 = x;
            int vx1 = x + 1;
            int vy0 = y;
            int vy1 = y + 1;

            bool n_occ = occAt(g, x, y + 1);
            bool s_occ = occAt(g, x, y - 1);
            bool e_occ = occAt(g, x + 1, y);
            bool w_occ = occAt(g, x - 1, y);

            if (!n_occ) {
                edges.push_back({vx1, vy1, vx0, vy1});
            }
            if (!s_occ) {
                edges.push_back({vx0, vy0, vx1, vy0});
            }
            if (!e_occ) {
                edges.push_back({vx1, vy0, vx1, vy1});
            }
            if (!w_occ) {
                edges.push_back({vx0, vy1, vx0, vy0});
            }
        }
    }

    std::unordered_map<uint64_t, std::vector<int>> out_map;
    out_map.reserve(edges.size() * 2);
    for (int i = 0; i < static_cast<int>(edges.size()); ++i) {
        out_map[packV(edges[i].x0, edges[i].y0)].push_back(i);
    }

    std::vector<uint8_t> used(edges.size(), 0);
    std::vector<Polygon2D> rings;

    auto dirOf = [&](const Edge& e) -> std::pair<int, int> {
        return {e.x1 - e.x0, e.y1 - e.y0};
    };
    auto rotCCW = [&](std::pair<int, int> d) -> std::pair<int, int> { return {-d.second, d.first}; };
    auto rotCW  = [&](std::pair<int, int> d) -> std::pair<int, int> { return {d.second, -d.first}; };

    for (int start_e = 0; start_e < static_cast<int>(edges.size()); ++start_e) {
        if (used[start_e]) continue;

        const Edge& e0 = edges[start_e];
        int sx = e0.x0, sy = e0.y0;
        int cx = sx, cy = sy;
        std::pair<int, int> cdir = dirOf(e0);

        std::vector<std::pair<int, int>> verts;
        verts.reserve(256);
        verts.emplace_back(cx, cy);

        int curr_e = start_e;
        while (true) {
            used[curr_e] = 1;
            const Edge& ce = edges[curr_e];
            cx = ce.x1;
            cy = ce.y1;
            if (cx == sx && cy == sy) {
                break;
            }
            verts.emplace_back(cx, cy);

            auto it = out_map.find(packV(cx, cy));
            if (it == out_map.end()) {
                break;
            }
            const auto& candidates = it->second;

            std::array<std::pair<int, int>, 4> prefs = {rotCCW(cdir), cdir, rotCW(cdir), std::make_pair(-cdir.first, -cdir.second)};
            int next_e = -1;
            for (const auto& pd : prefs) {
                for (int ei : candidates) {
                    if (used[ei]) continue;
                    auto d = dirOf(edges[ei]);
                    if (d == pd) {
                        next_e = ei;
                        break;
                    }
                }
                if (next_e != -1) break;
            }
            if (next_e == -1) {
                break;
            }
            curr_e = next_e;
            cdir = dirOf(edges[curr_e]);
        }

        if (verts.size() < 3) {
            continue;
        }

        for (const auto& loop : splitSimpleLoops(verts)) {
            Polygon2D ring;
            ring.reserve(loop.size());
            for (const auto& v : loop) {
                double wx = g.xmin + static_cast<double>(v.first) * g.cell;
                double wy = g.ymin + static_cast<double>(v.second) * g.cell;
                ring.emplace_back(wx, wy);
            }
            rings.push_back(std::move(ring));
        }
    }

    return rings;
}

static void removeConsecutiveDuplicates(Polygon2D& ring) {
    if (ring.empty()) return;
    Polygon2D out;
    out.reserve(ring.size());
    out.push_back(ring.front());
    for (size_t i = 1; i < ring.size(); ++i) {
        const auto& prev = out.back();
        const auto& curr = ring[i];
        if (std::hypot(curr.x - prev.x, curr.y - prev.y) > 1e-9) {
            out.push_back(curr);
        }
    }
    ring.swap(out);
}

static void removeCollinear(Polygon2D& ring) {
    if (ring.size() < 3) return;

    auto cross = [](const Point2D& a, const Point2D& b, const Point2D& c) -> double {
        double abx = b.x - a.x;
        double aby = b.y - a.y;
        double bcx = c.x - b.x;
        double bcy = c.y - b.y;
        return abx * bcy - aby * bcx;
    };

    bool changed = true;
    while (changed && ring.size() >= 3) {
        changed = false;
        Polygon2D out;
        out.reserve(ring.size());
        for (size_t i = 0; i < ring.size(); ++i) {
            const Point2D& prev = ring[(i + ring.size() - 1) % ring.size()];
            const Point2D& curr = ring[i];
            const Point2D& next = ring[(i + 1) % ring.size()];
            if (std::abs(cross(prev, curr, next)) < 1e-12) {
                changed = true;
                continue;
            }
            out.push_back(curr);
        }
        ring.swap(out);
    }
}

static std::vector<std::pair<Polygon2D, std::vector<Polygon2D>>> groupRingsIntoShapes(
    std::vector<Polygon2D> rings,
    double min_area_m2) {
    std::vector<Polygon2D> filtered;
    filtered.reserve(rings.size());
    for (auto& r : rings) {
        removeConsecutiveDuplicates(r);
        removeCollinear(r);
        double area = std::abs(signedArea2D(r));
        if (r.size() >= 3 && area >= min_area_m2) {
            filtered.push_back(std::move(r));
        }
    }
    rings = std::move(filtered);
    if (rings.empty()) return {};

    std::vector<double> areas(rings.size());
    for (size_t i = 0; i < rings.size(); ++i) {
        areas[i] = std::abs(signedArea2D(rings[i]));
    }
    std::vector<size_t> order(rings.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return areas[a] > areas[b];
    });

    std::vector<Polygon2D> rings_sorted;
    rings_sorted.reserve(rings.size());
    std::vector<double> areas_sorted;
    areas_sorted.reserve(rings.size());
    for (size_t idx : order) {
        rings_sorted.push_back(std::move(rings[idx]));
        areas_sorted.push_back(areas[idx]);
    }
    rings = std::move(rings_sorted);
    areas = std::move(areas_sorted);

    std::vector<Point2D> centroids(rings.size());
    for (size_t i = 0; i < rings.size(); ++i) {
        double sx = 0.0, sy = 0.0;
        for (const auto& p : rings[i]) { sx += p.x; sy += p.y; }
        double inv = 1.0 / std::max<size_t>(1, rings[i].size());
        centroids[i] = Point2D(sx * inv, sy * inv);
    }

    std::vector<int> parent(rings.size(), -1);
    for (size_t i = 0; i < rings.size(); ++i) {
        int best = -1;
        double best_area = std::numeric_limits<double>::infinity();
        for (size_t j = 0; j < rings.size(); ++j) {
            if (areas[j] <= areas[i]) continue;
            if (!pointInPolyRayCast(centroids[i], rings[j])) continue;
            if (areas[j] < best_area) {
                best_area = areas[j];
                best = static_cast<int>(j);
            }
        }
        parent[i] = best;
    }

    std::vector<int> depth(rings.size(), 0);
    for (size_t i = 0; i < rings.size(); ++i) {
        int d = 0;
        int p = parent[i];
        while (p != -1) {
            d++;
            p = parent[static_cast<size_t>(p)];
        }
        depth[i] = d;
    }

    std::vector<std::pair<Polygon2D, std::vector<Polygon2D>>> shapes;
    for (size_t i = 0; i < rings.size(); ++i) {
        if (depth[i] % 2 != 0) continue;  // holes are odd depth
        Polygon2D outer = rings[i];
        ensureCCW(outer);
        std::vector<Polygon2D> holes;
        for (size_t j = 0; j < rings.size(); ++j) {
            if (parent[j] == static_cast<int>(i) && depth[j] == depth[i] + 1) {
                Polygon2D hole = rings[j];
                ensureCW(hole);
                holes.push_back(std::move(hole));
            }
        }
        shapes.emplace_back(std::move(outer), std::move(holes));
    }
    return shapes;
}

// ----------------------------- CSF helpers ----------------------------------

// Python-like statistical outlier removal based on mean kNN distance.
static PointCloudPtr removeStatisticalOutliersMeanDist(
    const PointCloudPtr& cloud,
    int k,
    double std_ratio) {
    if (!cloud) return PointCloudPtr(new PointCloud);
    const size_t n = cloud->size();
    if (n == 0) return PointCloudPtr(new PointCloud);
    if (k < 1) k = 1;
    if (n < static_cast<size_t>(k + 1)) {
        return cloud;  // too few points => keep all
    }

    pcl::KdTreeFLANN<pcl::PointXYZ> tree;
    tree.setInputCloud(cloud);

    const int K = k + 1;  // include self
    std::vector<int> idx(K);
    std::vector<float> dist2(K);

    std::vector<double> mean_dists(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const pcl::PointXYZ& p = cloud->points[i];
        const int found = tree.nearestKSearch(p, K, idx, dist2);
        if (found <= 1) {
            mean_dists[i] = 0.0;
            continue;
        }
        double sum = 0.0;
        int cnt = 0;
        for (int j = 1; j < found; ++j) {  // skip self (dist=0)
            sum += std::sqrt(std::max(0.0f, dist2[j]));
            cnt++;
        }
        mean_dists[i] = (cnt > 0) ? (sum / static_cast<double>(cnt)) : 0.0;
    }

    double mu = 0.0;
    for (double v : mean_dists) mu += v;
    mu /= std::max<size_t>(1, n);

    double var = 0.0;
    for (double v : mean_dists) {
        const double d = v - mu;
        var += d * d;
    }
    var /= std::max<size_t>(1, n);
    const double sigma = std::sqrt(std::max(0.0, var));
    const double threshold = mu + std_ratio * sigma;

    PointCloudPtr out(new PointCloud);
    out->reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (mean_dists[i] <= threshold) {
            out->push_back(cloud->points[i]);
        }
    }
    return out;
}

static double derivedTraversableStepHeight(const ObstacleDetectionParams& params) {
    if (params.traversable_step_height_m > 0.0) {
        return params.traversable_step_height_m;
    }
    return std::max(
        0.01,
        std::max(0.0, params.traversable_step_height_ratio) * std::max(0.0, params.wheel_radius_m));
}

static bool pointInsideFootprintAtPose(
    const Point2D& point,
    const PathState& pose,
    double half_l,
    double half_w) {
    const double dx = point.x - pose.point.x;
    const double dy = point.y - pose.point.y;
    const double c = std::cos(pose.heading);
    const double s = std::sin(pose.heading);
    const double lx = c * dx + s * dy;
    const double ly = -s * dx + c * dy;
    return std::abs(lx) <= half_l && std::abs(ly) <= half_w;
}

struct CsfSegmentationResult {
    PointCloudPtr ground;
    PointCloudPtr nonground;
    size_t grid_cells = 0;
};

static CsfSegmentationResult segmentGroundClothSimulation(
    const PointCloudPtr& cloud,
    const ObstacleDetectionParams& params) {
    CsfSegmentationResult result;
    result.ground.reset(new PointCloud);
    result.nonground.reset(new PointCloud);
    if (!cloud || cloud->empty()) {
        return result;
    }

    std::vector<csf::Point> csf_points;
    csf_points.reserve(cloud->size());
    for (const auto& pt : cloud->points) {
        csf_points.push_back(csf::Point{
            static_cast<double>(pt.x),
            static_cast<double>(pt.y),
            static_cast<double>(pt.z)});
    }

    CSF csf;
    csf.params.bSloopSmooth = params.csf_slope_processing;
    csf.params.time_step = 0.65;
    csf.params.cloth_resolution = std::max(0.005, params.csf_cloth_resolution_m);
    csf.params.class_threshold = std::max(0.0, params.csf_classification_threshold_m);
    csf.params.interations = std::max(1, params.csf_max_iterations);
    csf.params.rigidness = std::clamp(params.csf_rigidness, 1, 10);
    csf.setPointCloud(std::move(csf_points));

    std::vector<int> ground_indices;
    std::vector<int> nonground_indices;
    csf.do_filtering(ground_indices, nonground_indices, /*exportCloth=*/false);
    result.grid_cells = csf.size();

    result.ground->reserve(cloud->size());
    result.nonground->reserve(cloud->size() / 4);

    for (int idx : ground_indices) {
        if (idx >= 0 && static_cast<size_t>(idx) < cloud->size()) {
            result.ground->push_back(cloud->points[static_cast<size_t>(idx)]);
        }
    }
    for (int idx : nonground_indices) {
        if (idx < 0 || static_cast<size_t>(idx) >= cloud->size()) {
            continue;
        }
        result.nonground->push_back(cloud->points[static_cast<size_t>(idx)]);
    }
    return result;
}

static PointCloudPtr filterCsfNonGroundByClearance(
    const PointCloudPtr& nonground,
    const PointCloudPtr& ground,
    const ObstacleDetectionParams& params) {
    PointCloudPtr out(new PointCloud);
    if (!nonground || nonground->empty() || !ground || ground->empty()) {
        return out;
    }

    pcl::PointCloud<pcl::PointXYZ>::Ptr ground_xy(new pcl::PointCloud<pcl::PointXYZ>);
    ground_xy->reserve(ground->size());
    std::vector<float> ground_z;
    ground_z.reserve(ground->size());
    for (const auto& pt : ground->points) {
        ground_xy->push_back(pcl::PointXYZ(pt.x, pt.y, 0.0f));
        ground_z.push_back(pt.z);
    }

    pcl::KdTreeFLANN<pcl::PointXYZ> ground_tree;
    ground_tree.setInputCloud(ground_xy);

    const double min_clearance = derivedTraversableStepHeight(params);
    const double max_clearance = std::max(min_clearance, params.csf_max_obstacle_clearance_m);
    out->reserve(nonground->size());

    std::vector<int> nn_idx(1);
    std::vector<float> nn_dist2(1);
    for (const auto& pt : nonground->points) {
        const pcl::PointXYZ query(pt.x, pt.y, 0.0f);
        if (ground_tree.nearestKSearch(query, 1, nn_idx, nn_dist2) <= 0) {
            continue;
        }
        const double clearance = static_cast<double>(pt.z - ground_z[static_cast<size_t>(nn_idx[0])]);
        if (clearance >= min_clearance && clearance <= max_clearance) {
            out->push_back(pt);
        }
    }
    return out;
}

static std::vector<PathState> densifyPathForFootprintCleanup(
    const std::vector<PathState>& path,
    double spacing_m) {
    if (path.size() < 2 || spacing_m <= 1e-6) {
        return path;
    }
    std::vector<PathState> out;
    out.reserve(path.size() * 2);
    out.push_back(path.front());
    for (size_t i = 1; i < path.size(); ++i) {
        const auto& a = path[i - 1];
        const auto& b = path[i];
        const double dx = b.point.x - a.point.x;
        const double dy = b.point.y - a.point.y;
        const double dist = std::hypot(dx, dy);
        const int steps = std::max(1, static_cast<int>(std::ceil(dist / spacing_m)));
        const double heading = (dist > 1e-6) ? std::atan2(dy, dx) : a.heading;
        for (int s = 1; s <= steps; ++s) {
            const double t = static_cast<double>(s) / static_cast<double>(steps);
            PathState st;
            st.point.x = a.point.x + dx * t;
            st.point.y = a.point.y + dy * t;
            st.heading = heading;
            st.vx = std::cos(heading);
            st.vy = std::sin(heading);
            out.push_back(st);
        }
    }
    return out;
}

static PointCloudPtr removeTrailFootprintObstacleCandidates(
    const PointCloudPtr& candidates,
    const std::vector<PathState>& path,
    const ObstacleDetectionParams& params,
    size_t* removed_count) {
    if (removed_count) {
        *removed_count = 0;
    }
    if (!candidates || candidates->empty()) {
        return PointCloudPtr(new PointCloud);
    }
    if (path.empty() || !params.csf_trail_footprint_cleanup) {
        return candidates;
    }

    const double margin = std::max(0.0, params.csf_trail_cleanup_margin_m);
    const double half_l = std::max(0.0, params.robot_length_m) / 2.0 + margin;
    const double half_w = std::max(0.0, params.robot_width_m) / 2.0 + margin;
    const double spacing = std::max(0.03, 0.5 * std::min(half_l, half_w));
    const std::vector<PathState> dense_path = densifyPathForFootprintCleanup(path, spacing);
    if (dense_path.empty()) {
        return candidates;
    }

    pcl::PointCloud<pcl::PointXYZ>::Ptr path_xy(new pcl::PointCloud<pcl::PointXYZ>);
    path_xy->reserve(dense_path.size());
    for (const auto& st : dense_path) {
        path_xy->push_back(pcl::PointXYZ(
            static_cast<float>(st.point.x),
            static_cast<float>(st.point.y),
            0.0f));
    }
    pcl::KdTreeFLANN<pcl::PointXYZ> path_tree;
    path_tree.setInputCloud(path_xy);

    const double search_radius = std::hypot(half_l, half_w);
    const int max_nn = std::min<int>(12, static_cast<int>(dense_path.size()));
    std::vector<int> nn_idx(std::max(1, max_nn));
    std::vector<float> nn_dist2(std::max(1, max_nn));

    PointCloudPtr out(new PointCloud);
    out->reserve(candidates->size());
    size_t removed = 0;
    for (const auto& pt : candidates->points) {
        const pcl::PointXYZ query(pt.x, pt.y, 0.0f);
        const int found = path_tree.radiusSearch(query, static_cast<float>(search_radius), nn_idx, nn_dist2, max_nn);
        bool in_trail = false;
        for (int i = 0; i < found; ++i) {
            const int pi = nn_idx[static_cast<size_t>(i)];
            if (pi < 0 || pi >= static_cast<int>(dense_path.size())) {
                continue;
            }
            if (pointInsideFootprintAtPose(Point2D(pt.x, pt.y), dense_path[static_cast<size_t>(pi)], half_l, half_w)) {
                in_trail = true;
                break;
            }
        }
        if (in_trail) {
            removed++;
            continue;
        }
        out->push_back(pt);
    }
    if (removed_count) {
        *removed_count = removed;
    }
    return out;
}

}  // namespace

ObstacleDetectionResult detectObstaclesAuto(
    const PointCloudPtr& cloud,
    const std::vector<PathState>& driven_path,
    const Polygon2D* roi_or_boundary,
    const ObstacleDetectionParams& params) {
    ObstacleDetectionResult res;
    if (!cloud || cloud->empty()) {
        res.error_message = "No point cloud loaded.";
        return res;
    }

    const Polygon2D scope = effectiveScopePolygon(roi_or_boundary);
    PointCloudPtr scoped_cloud = filterCloudToPolygon(cloud, scope);
    res.stats.input_points = cloud->size();
    res.stats.roi_points = scoped_cloud ? scoped_cloud->size() : 0;

    std::vector<PathState> path = driven_path;
    if (!scope.empty()) {
        path = filterPathToPolygon(driven_path, scope);
    }
    res.stats.path_poses = path.size();

    // Derived polygonization defaults (match the legacy CSF occupancy path).
    double grid_cell_m = params.grid_cell_m;
    if (grid_cell_m <= 0.0) {
        grid_cell_m = 0.09;
    }
    const double csf_grid_cell_m = std::max(0.02, grid_cell_m);

    // ------------------------------------------------------------------
    // 1. Optional pre-CSF statistical outlier removal.
    // ------------------------------------------------------------------
    PointCloudPtr csf_input_cloud = scoped_cloud;
    if (params.csf_pre_sor_enabled) {
        csf_input_cloud = removeStatisticalOutliersMeanDist(
            csf_input_cloud, params.csf_pre_sor_k, params.csf_pre_sor_std);
    }

    // ------------------------------------------------------------------
    // 2. CSF ground / non-ground segmentation.
    // ------------------------------------------------------------------
    const CsfSegmentationResult csf = segmentGroundClothSimulation(csf_input_cloud, params);
    if (!csf.ground || csf.ground->empty()) {
        res.error_message = "CSF could not classify any ground points.";
        return res;
    }
    PointCloudPtr obstacle_raw = csf.nonground ? csf.nonground : PointCloudPtr(new PointCloud);
    res.stats.footprint_ground_points = csf.ground->size();
    res.stats.ground_points_band = csf.ground->size();
    res.stats.raw_obstacle_candidates = obstacle_raw->size();

    // ------------------------------------------------------------------
    // 3. Denoise non-ground, keep traversable-clearance band, strip trail.
    // ------------------------------------------------------------------
    PointCloudPtr obstacle_clean = removeStatisticalOutliersMeanDist(
        obstacle_raw, params.outlier_k, params.outlier_std);
    obstacle_clean = filterCsfNonGroundByClearance(obstacle_clean, csf.ground, params);
    size_t trail_removed = 0;
    obstacle_clean = removeTrailFootprintObstacleCandidates(
        obstacle_clean, path, params, &trail_removed);
    res.stats.obstacle_points_after_outlier = obstacle_clean ? obstacle_clean->size() : 0;

    // ------------------------------------------------------------------
    // 4. Project to 2D and polygonize on an occupancy grid.
    // ------------------------------------------------------------------
    std::vector<Point2D> obs_xy;
    obs_xy.reserve(obstacle_clean ? obstacle_clean->size() : 0);
    if (obstacle_clean) {
        for (const auto& pt : obstacle_clean->points) {
            obs_xy.emplace_back(pt.x, pt.y);
        }
    }

    std::vector<Obstacle2D> occupancy_obstacles;
    int total_holes = 0;
    auto shapes = polygonizeClusterGrid(
        obs_xy,
        csf_grid_cell_m,
        /*contour_cell_m=*/csf_grid_cell_m,
        /*inflate_radius_m=*/0.0,
        /*smooth_radius_m=*/0.0,
        params.min_contour_area_m2);
    occupancy_obstacles.reserve(shapes.size());
    for (auto& sh : shapes) {
        Obstacle2D obs;
        obs.outer = std::move(sh.first);
        obs.holes = std::move(sh.second);
        total_holes += static_cast<int>(obs.holes.size());
        occupancy_obstacles.push_back(std::move(obs));
    }

    res.stats.clusters_found = 0;
    res.stats.groups_merged = static_cast<int>(occupancy_obstacles.size());
    res.stats.total_holes = total_holes;
    res.stats.obstacle_shapes = static_cast<int>(occupancy_obstacles.size());
    res.obstacles = std::move(occupancy_obstacles);
    res.success = true;
    return res;
}

}  // namespace f2c_cpp
