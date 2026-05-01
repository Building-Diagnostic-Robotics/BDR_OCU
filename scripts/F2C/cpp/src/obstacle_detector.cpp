/**
 * @file obstacle_detector.cpp
 * @brief Implementation of automatic obstacle detection (AUTO mode).
 */

#include "obstacle_detector.hpp"

#include <pcl/kdtree/kdtree_flann.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <unordered_map>
#include <utility>

namespace f2c_cpp {

static ObstacleCancelCallback g_obstacleCancelCallback = nullptr;

void setObstacleCancelCallback(ObstacleCancelCallback callback) {
    g_obstacleCancelCallback = callback;
}

namespace {

static constexpr double kEps = 1e-12;

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

static double absArea2D(const Polygon2D& ring) {
    return std::abs(signedArea2D(ring));
}

static Polygon2D rectFromBBox(
    double xmin, double ymin, double xmax, double ymax,
    double min_size, double margin) {
    // Axis-aligned bbox rectangle with optional minimum size and margin.
    const double cx = 0.5 * (xmin + xmax);
    const double cy = 0.5 * (ymin + ymax);
    const double w = std::max(min_size, (xmax - xmin)) + 2.0 * margin;
    const double h = std::max(min_size, (ymax - ymin)) + 2.0 * margin;
    const double hw = 0.5 * w;
    const double hh = 0.5 * h;

    Polygon2D poly;
    poly.reserve(4);
    poly.emplace_back(cx - hw, cy - hh);
    poly.emplace_back(cx + hw, cy - hh);
    poly.emplace_back(cx + hw, cy + hh);
    poly.emplace_back(cx - hw, cy + hh);
    return poly;
}

static bool pointInPolyRayCast(const Point2D& p, const Polygon2D& poly) {
    // Ray casting; matches the Python script's simple implementation.
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
    // If the ROI removes everything, fall back to full path.
    if (out.empty()) return path;
    return out;
}

struct PlaneModel {
    double nx = 0.0;
    double ny = 0.0;
    double nz = 1.0;
    double d = 0.0;  // n·p + d = 0
};

static double signedDist(const PlaneModel& pl, const pcl::PointXYZ& p) {
    return pl.nx * p.x + pl.ny * p.y + pl.nz * p.z + pl.d;
}

static PlaneModel fitPlaneRansac(const PointCloudPtr& ground_cloud, int iters, double thresh_m) {
    PlaneModel best;
    if (!ground_cloud || ground_cloud->size() < 3) {
        return best;
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist_idx(0, ground_cloud->size() - 1);

    size_t best_count = 0;
    const size_t n = ground_cloud->size();

    auto getPt = [&](size_t i) -> Eigen::Vector3d {
        const auto& p = ground_cloud->points[i];
        return Eigen::Vector3d(p.x, p.y, p.z);
    };

    for (int iter = 0; iter < iters; ++iter) {
        size_t i0 = dist_idx(rng);
        size_t i1 = dist_idx(rng);
        size_t i2 = dist_idx(rng);
        if (i0 == i1 || i0 == i2 || i1 == i2) {
            continue;
        }
        Eigen::Vector3d p0 = getPt(i0);
        Eigen::Vector3d p1 = getPt(i1);
        Eigen::Vector3d p2 = getPt(i2);
        Eigen::Vector3d v1 = p1 - p0;
        Eigen::Vector3d v2 = p2 - p0;
        Eigen::Vector3d nrm = v1.cross(v2);
        double norm = nrm.norm();
        if (norm < 1e-12) continue;
        nrm /= norm;
        if (nrm.z() < 0.0) nrm = -nrm;
        double d = -nrm.dot(p0);

        size_t count = 0;
        for (const auto& pt : ground_cloud->points) {
            double dd = std::abs(nrm.x() * pt.x + nrm.y() * pt.y + nrm.z() * pt.z + d);
            if (dd <= thresh_m) {
                count++;
            }
        }
        if (count > best_count) {
            best_count = count;
            best.nx = nrm.x();
            best.ny = nrm.y();
            best.nz = nrm.z();
            best.d = d;
        }
    }
    return best;
}

static double medianZ(const PointCloudPtr& cloud) {
    if (!cloud || cloud->empty()) return 0.0;
    std::vector<float> z;
    z.reserve(cloud->size());
    for (const auto& p : cloud->points) z.push_back(p.z);
    size_t mid = z.size() / 2;
    std::nth_element(z.begin(), z.begin() + mid, z.end());
    return static_cast<double>(z[mid]);
}

static PointCloudPtr extractFootprintGround(
    const PointCloudPtr& cloud,
    const std::vector<PathState>& path,
    double robot_length_m,
    double robot_width_m,
    double footprint_margin_m,
    double z_max) {
    if (!cloud || cloud->empty()) return PointCloudPtr(new PointCloud);
    if (path.empty()) return PointCloudPtr(new PointCloud);

    // Build KD-tree of path XY
    pcl::PointCloud<pcl::PointXYZ>::Ptr path_xy(new pcl::PointCloud<pcl::PointXYZ>);
    path_xy->reserve(path.size());
    for (const auto& st : path) {
        path_xy->push_back(pcl::PointXYZ(static_cast<float>(st.point.x),
                                         static_cast<float>(st.point.y),
                                         0.0f));
    }
    pcl::KdTreeFLANN<pcl::PointXYZ> path_tree;
    path_tree.setInputCloud(path_xy);

    const double half_l = robot_length_m / 2.0 + footprint_margin_m;
    const double half_w = robot_width_m / 2.0 + footprint_margin_m;
    const double search_radius = std::hypot(half_l, half_w);
    const double dist_limit = search_radius * 1.5;
    const int K = std::min<int>(5, static_cast<int>(path.size()));

    PointCloudPtr ground(new PointCloud);
    ground->reserve(cloud->size() / 10);

    std::vector<int> nn_idx;
    std::vector<float> nn_dist2;
    nn_idx.resize(std::max(1, K));
    nn_dist2.resize(std::max(1, K));

    for (const auto& pt : cloud->points) {
        if (pt.z > z_max) continue;

        pcl::PointXYZ q(pt.x, pt.y, 0.0f);
        int found = path_tree.nearestKSearch(q, K, nn_idx, nn_dist2);
        if (found <= 0) continue;

        bool hit = false;
        for (int i = 0; i < found; ++i) {
            double d = std::sqrt(static_cast<double>(nn_dist2[i]));
            if (d > dist_limit) {
                continue;
            }
            const auto& pose = path[static_cast<size_t>(nn_idx[i])];
            double cx = pose.point.x;
            double cy = pose.point.y;
            double yaw = pose.heading;

            double dx = pt.x - cx;
            double dy = pt.y - cy;
            double c = std::cos(yaw);
            double s = std::sin(yaw);
            // Rotate by -yaw into footprint local frame
            double lx = c * dx + s * dy;
            double ly = -s * dx + c * dy;
            if (std::abs(lx) <= half_l && std::abs(ly) <= half_w) {
                hit = true;
                break;
            }
        }
        if (hit) {
            ground->push_back(pt);
        }
    }

    return ground;
}

// --------------------- Statistical outlier removal (Python-like) ------------

static PointCloudPtr removeStatisticalOutliersMeanDist(
    const PointCloudPtr& cloud,
    int k,
    double std_ratio) {
    if (!cloud) return PointCloudPtr(new PointCloud);
    const size_t n = cloud->size();
    if (n == 0) return PointCloudPtr(new PointCloud);
    if (k < 1) k = 1;
    if (n < static_cast<size_t>(k + 1)) {
        return cloud;  // match python: too few points => keep all
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

// --------------------------- DBSCAN (2D) ------------------------------------

static std::vector<int> dbscan2D(const std::vector<Point2D>& pts, double eps, int min_samples) {
    const int n = static_cast<int>(pts.size());
    std::vector<int> labels(n, -1);
    if (n == 0) return labels;

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_xy(new pcl::PointCloud<pcl::PointXYZ>);
    cloud_xy->reserve(pts.size());
    for (const auto& p : pts) {
        cloud_xy->push_back(pcl::PointXYZ(static_cast<float>(p.x), static_cast<float>(p.y), 0.0f));
    }

    pcl::KdTreeFLANN<pcl::PointXYZ> tree;
    tree.setInputCloud(cloud_xy);

    std::vector<std::vector<int>> neighbours(n);
    neighbours.reserve(n);
    const float eps_f = static_cast<float>(eps);
    for (int i = 0; i < n; ++i) {
        std::vector<int> idx;
        std::vector<float> dist2;
        tree.radiusSearch(cloud_xy->points[i], eps_f, idx, dist2);
        neighbours[i] = std::move(idx);
    }

    std::vector<uint8_t> core(n, 0);
    for (int i = 0; i < n; ++i) {
        if (static_cast<int>(neighbours[i].size()) >= min_samples) {
            core[i] = 1;
        }
    }

    int cluster_id = 0;
    for (int seed = 0; seed < n; ++seed) {
        if (labels[seed] != -1 || !core[seed]) continue;
        std::deque<int> q;
        q.push_back(seed);
        labels[seed] = cluster_id;
        while (!q.empty()) {
            int curr = q.front();
            q.pop_front();
            for (int nb : neighbours[curr]) {
                if (labels[nb] != -1) continue;
                labels[nb] = cluster_id;
                if (core[nb]) {
                    q.push_back(nb);
                }
            }
        }
        cluster_id++;
    }

    return labels;
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

struct ScalarGrid {
    int w = 0;
    int h = 0;
    double xmin = 0.0;
    double ymin = 0.0;
    double cell = 0.09;
    std::vector<double> values;  // row-major signed distance, +inside / -outside
};

static inline double scalarAt(const ScalarGrid& g, int x, int y) {
    if (x < 0 || y < 0 || x >= g.w || y >= g.h) {
        return -0.5 * g.cell;
    }
    return g.values[static_cast<size_t>(y) * static_cast<size_t>(g.w) + static_cast<size_t>(x)];
}

using ShapeContours = std::vector<std::pair<Polygon2D, std::vector<Polygon2D>>>;

static std::vector<double> squaredDistanceToFeature(const OccGrid& g, bool feature_occupied) {
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> row_sq(static_cast<size_t>(g.w) * static_cast<size_t>(g.h), inf);
    std::vector<double> out(static_cast<size_t>(g.w) * static_cast<size_t>(g.h), inf);

    for (int y = 0; y < g.h; ++y) {
        int last = -1;
        for (int x = 0; x < g.w; ++x) {
            if ((occAt(g, x, y) != 0) == feature_occupied) {
                last = x;
            }
            if (last >= 0) {
                const double dx = static_cast<double>(x - last);
                row_sq[static_cast<size_t>(y) * static_cast<size_t>(g.w) + static_cast<size_t>(x)] = dx * dx;
            }
        }

        last = -1;
        for (int x = g.w - 1; x >= 0; --x) {
            if ((occAt(g, x, y) != 0) == feature_occupied) {
                last = x;
            }
            if (last >= 0) {
                const double dx = static_cast<double>(last - x);
                double& v = row_sq[static_cast<size_t>(y) * static_cast<size_t>(g.w) + static_cast<size_t>(x)];
                v = std::min(v, dx * dx);
            }
        }
    }

    for (int x = 0; x < g.w; ++x) {
        std::vector<std::pair<int, double>> candidates;
        candidates.reserve(static_cast<size_t>(g.h));
        for (int y = 0; y < g.h; ++y) {
            const double base = row_sq[static_cast<size_t>(y) * static_cast<size_t>(g.w) + static_cast<size_t>(x)];
            if (std::isfinite(base)) {
                candidates.emplace_back(y, base);
            }
        }
        if (candidates.empty()) {
            continue;
        }

        for (int y = 0; y < g.h; ++y) {
            double best = inf;
            for (const auto& [cy, base] : candidates) {
                const double dy = static_cast<double>(y - cy);
                best = std::min(best, base + dy * dy);
            }
            out[static_cast<size_t>(y) * static_cast<size_t>(g.w) + static_cast<size_t>(x)] = best;
        }
    }

    return out;
}

static ScalarGrid signedDistanceFieldFromOcc(const OccGrid& occ) {
    ScalarGrid g;
    if (occ.w <= 0 || occ.h <= 0 || occ.occ.empty()) {
        return g;
    }

    g.w = occ.w;
    g.h = occ.h;
    g.xmin = occ.xmin;
    g.ymin = occ.ymin;
    g.cell = occ.cell;
    g.values.assign(static_cast<size_t>(g.w) * static_cast<size_t>(g.h), 0.0);

    const std::vector<double> dist_occ_sq = squaredDistanceToFeature(occ, true);
    const std::vector<double> dist_empty_sq = squaredDistanceToFeature(occ, false);
    for (size_t i = 0; i < g.values.size(); ++i) {
        const double d_occ = std::isfinite(dist_occ_sq[i]) ? std::sqrt(std::max(0.0, dist_occ_sq[i])) : 0.0;
        const double d_empty = std::isfinite(dist_empty_sq[i]) ? std::sqrt(std::max(0.0, dist_empty_sq[i])) : 0.0;
        // A center-based signed distance keeps the same topology as the binary mask,
        // while shifting iso-crossings away from fixed midpoints on long oblique runs.
        g.values[i] = 0.5 * (d_empty - d_occ) * occ.cell;
    }
    return g;
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
    // Morphological closing = dilation then erosion (conservative / extensive in infinite grid).
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
static std::vector<Polygon2D> extractContourRingsFromSdf(const ScalarGrid& g);
static ShapeContours groupRingsIntoShapes(
    std::vector<Polygon2D> rings,
    double min_area_m2);
static ShapeContours extractShapesFromOcc(const OccGrid& g, double min_area_m2);

static ShapeContours polygonizeClusterGrid(
    const std::vector<Point2D>& pts2d,
    double grid_cell_m,
    double contour_cell_m,
    double inflate_radius_m,
    double smooth_radius_m,
    double min_contour_area_m2) {
    if (pts2d.empty() || grid_cell_m <= 0.0) {
        return {};
    }

    // Match Python: padding=max(0.20, 2*inflate_radius)
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

    return extractShapesFromOcc(occ, min_contour_area_m2);
}

static Polygon2D rectFromBbox(const std::vector<Point2D>& pts2d, double min_size_m, double margin_m) {
    if (pts2d.empty()) {
        const double half = 0.5 * min_size_m;
        return Polygon2D{
            { -half, -half },
            {  half, -half },
            {  half,  half },
            { -half,  half },
        };
    }
    double xmin = pts2d.front().x;
    double xmax = pts2d.front().x;
    double ymin = pts2d.front().y;
    double ymax = pts2d.front().y;
    for (const auto& p : pts2d) {
        xmin = std::min(xmin, p.x);
        xmax = std::max(xmax, p.x);
        ymin = std::min(ymin, p.y);
        ymax = std::max(ymax, p.y);
    }
    const double cx = 0.5 * (xmin + xmax);
    const double cy = 0.5 * (ymin + ymax);
    const double w = std::max(min_size_m, (xmax - xmin)) + 2.0 * margin_m;
    const double h = std::max(min_size_m, (ymax - ymin)) + 2.0 * margin_m;
    const double hw = 0.5 * w;
    const double hh = 0.5 * h;
    return Polygon2D{
        { cx - hw, cy - hh },
        { cx + hw, cy - hh },
        { cx + hw, cy + hh },
        { cx - hw, cy + hh },
    };
}

static bool isMicroCluster(const std::vector<Point2D>& pts2d, const ObstacleDetectionParams& params) {
    if (static_cast<int>(pts2d.size()) < params.micro_min_pts) {
        return false;
    }
    double xmin = pts2d.front().x;
    double xmax = pts2d.front().x;
    double ymin = pts2d.front().y;
    double ymax = pts2d.front().y;
    for (const auto& p : pts2d) {
        xmin = std::min(xmin, p.x);
        xmax = std::max(xmax, p.x);
        ymin = std::min(ymin, p.y);
        ymax = std::max(ymax, p.y);
    }
    const double span_x = xmax - xmin;
    const double span_y = ymax - ymin;
    if (std::max(span_x, span_y) > params.micro_max_span_m) {
        return false;
    }
    const double area = std::max(span_x, 1e-6) * std::max(span_y, 1e-6);
    const double density = static_cast<double>(pts2d.size()) / area;
    return density >= params.micro_min_density_pts_per_m2;
}

static bool pointInObstacleShape(const Point2D& p, const Obstacle2D& shape) {
    if (!pointInPolyRayCast(p, shape.outer)) {
        return false;
    }
    for (const auto& h : shape.holes) {
        if (pointInPolyRayCast(p, h)) {
            return false;
        }
    }
    return true;
}

static OccGrid rasterizeShapeToOccupancy(
    const Obstacle2D& shape,
    double cell_size,
    double padding) {
    OccGrid g;
    if (shape.outer.size() < 3 || cell_size <= 0.0) {
        return g;
    }
    g.cell = cell_size;

    double xmin = shape.outer.front().x;
    double xmax = shape.outer.front().x;
    double ymin = shape.outer.front().y;
    double ymax = shape.outer.front().y;
    for (const auto& p : shape.outer) {
        xmin = std::min(xmin, p.x);
        xmax = std::max(xmax, p.x);
        ymin = std::min(ymin, p.y);
        ymax = std::max(ymax, p.y);
    }
    g.xmin = xmin - padding;
    g.ymin = ymin - padding;
    const double xmax2 = xmax + padding;
    const double ymax2 = ymax + padding;

    g.w = std::max(1, static_cast<int>(std::ceil((xmax2 - g.xmin) / cell_size)));
    g.h = std::max(1, static_cast<int>(std::ceil((ymax2 - g.ymin) / cell_size)));
    g.occ.assign(static_cast<size_t>(g.w) * static_cast<size_t>(g.h), 0);

    for (int y = 0; y < g.h; ++y) {
        const double wy = g.ymin + (static_cast<double>(y) + 0.5) * cell_size;
        for (int x = 0; x < g.w; ++x) {
            const double wx = g.xmin + (static_cast<double>(x) + 0.5) * cell_size;
            if (pointInObstacleShape(Point2D(wx, wy), shape)) {
                g.occ[static_cast<size_t>(y) * static_cast<size_t>(g.w) + static_cast<size_t>(x)] = 1;
            }
        }
    }
    return g;
}

static std::vector<Obstacle2D> smoothShapesRollingDiskGrid(
    const std::vector<Obstacle2D>& shapes,
    double radius_m,
    double cell_size,
    double contour_cell_m,
    double min_contour_area_m2,
    bool preserve_holes,
    double preserve_holes_min_area_m2) {
    if (radius_m <= 1e-9 || shapes.empty()) {
        return shapes;
    }

    std::vector<Obstacle2D> out;
    out.reserve(shapes.size());

    for (const auto& sh : shapes) {
        const double padding = std::max(0.20, 2.0 * radius_m);
        OccGrid occ = rasterizeShapeToOccupancy(sh, cell_size, padding);
        if (occ.occ.empty()) {
            out.push_back(sh);
            continue;
        }

        OccGrid occ2 = closeOccupancyConservative(occ, radius_m);

        // Preserve large holes: prevent smoothing from filling navigable cavities.
        if (preserve_holes && !sh.holes.empty() && preserve_holes_min_area_m2 > 0.0) {
            std::vector<const Polygon2D*> holes_keep;
            holes_keep.reserve(sh.holes.size());
            for (const auto& h : sh.holes) {
                if (polygonArea(h) >= preserve_holes_min_area_m2) {
                    holes_keep.push_back(&h);
                }
            }
            if (!holes_keep.empty()) {
                for (int y = 0; y < occ2.h; ++y) {
                    const double wy = occ2.ymin + (static_cast<double>(y) + 0.5) * occ2.cell;
                    for (int x = 0; x < occ2.w; ++x) {
                        if (!occAt(occ2, x, y)) {
                            continue;
                        }
                        const double wx = occ2.xmin + (static_cast<double>(x) + 0.5) * occ2.cell;
                        const Point2D p(wx, wy);
                        bool in_preserved_hole = false;
                        for (const auto* hptr : holes_keep) {
                            if (pointInPolyRayCast(p, *hptr)) {
                                in_preserved_hole = true;
                                break;
                            }
                        }
                        if (in_preserved_hole) {
                            occ2.occ[static_cast<size_t>(y) * static_cast<size_t>(occ2.w) + static_cast<size_t>(x)] = 0;
                        }
                    }
                }
            }
        }

        // Optional coarser contour grid (conservative max-pooling)
        if (contour_cell_m > 0.0 && contour_cell_m > cell_size + 1e-12) {
            const int factor = std::max(1, static_cast<int>(std::round(contour_cell_m / cell_size)));
            if (factor > 1) {
                occ2 = maxpoolOccupancy(occ2, factor);
            }
        }

        auto grouped = extractShapesFromOcc(occ2, min_contour_area_m2);
        if (grouped.empty()) {
            out.push_back(sh);
            continue;
        }

        for (auto& gsh : grouped) {
            Obstacle2D obs;
            obs.outer = std::move(gsh.first);
            obs.holes = std::move(gsh.second);
            out.push_back(std::move(obs));
        }
    }

    return out;
}

template <typename SampleFn>
static std::vector<Polygon2D> extractContourRingsGeneric(
    int w,
    int h,
    double xmin,
    double ymin,
    double cell,
    double iso,
    const SampleFn& sampleValue) {
    struct Segment {
        Point2D a;
        Point2D b;
    };
    struct Key {
        int64_t x = 0;
        int64_t y = 0;
        bool operator==(const Key& o) const { return x == o.x && y == o.y; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return static_cast<size_t>((static_cast<uint64_t>(k.x) * 1315423911ULL) ^
                                       (static_cast<uint64_t>(k.y) + 0x9e3779b97f4a7c15ULL));
        }
    };

    std::vector<Polygon2D> rings;
    if (w <= 0 || h <= 0) {
        return rings;
    }

    const double snap = std::max(1e-9, cell * 1e-6);
    const double close_tol = snap * 8.0;

    auto toKey = [&](const Point2D& p) -> Key {
        return Key{
            static_cast<int64_t>(std::llround(p.x / snap)),
            static_cast<int64_t>(std::llround(p.y / snap))
        };
    };

    auto interp = [&](const Point2D& p0, const Point2D& p1, double v0, double v1) -> Point2D {
        double t = 0.5;
        const double dv = v1 - v0;
        if (std::abs(dv) > 1e-12) {
            t = (iso - v0) / dv;
        }
        t = std::clamp(t, 0.0, 1.0);
        return Point2D(p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t);
    };

    auto samplePos = [](int cx, int cy) -> Point2D {
        return Point2D(static_cast<double>(cx) + 0.5, static_cast<double>(cy) + 0.5);
    };

    std::vector<Segment> segments;
    segments.reserve(static_cast<size_t>(w) * static_cast<size_t>(h) * 2);

    // Marching squares over 2x2 blocks of occupancy-cell samples.
    // This preserves tiny occupied islands better than averaged vertex fields.
    for (int y = -1; y < h; ++y) {
        for (int x = -1; x < w; ++x) {
            const double v0 = sampleValue(x, y);         // bottom-left
            const double v1 = sampleValue(x + 1, y);     // bottom-right
            const double v2 = sampleValue(x + 1, y + 1); // top-right
            const double v3 = sampleValue(x, y + 1);     // top-left

            const int c0 = (v0 > iso) ? 1 : 0;
            const int c1 = (v1 > iso) ? 1 : 0;
            const int c2 = (v2 > iso) ? 1 : 0;
            const int c3 = (v3 > iso) ? 1 : 0;
            const int idx = c0 | (c1 << 1) | (c2 << 2) | (c3 << 3);
            if (idx == 0 || idx == 15) {
                continue;
            }

            const Point2D p0 = samplePos(x, y);
            const Point2D p1 = samplePos(x + 1, y);
            const Point2D p2 = samplePos(x + 1, y + 1);
            const Point2D p3 = samplePos(x, y + 1);

            const Point2D e0 = interp(p0, p1, v0, v1); // bottom
            const Point2D e1 = interp(p1, p2, v1, v2); // right
            const Point2D e2 = interp(p2, p3, v2, v3); // top
            const Point2D e3 = interp(p3, p0, v3, v0); // left

            auto addSeg = [&](const Point2D& a, const Point2D& b) {
                if (std::hypot(a.x - b.x, a.y - b.y) > 1e-12) {
                    segments.push_back(Segment{a, b});
                }
            };

            switch (idx) {
                case 1:  addSeg(e3, e0); break;
                case 2:  addSeg(e0, e1); break;
                case 3:  addSeg(e3, e1); break;
                case 4:  addSeg(e1, e2); break;
                case 5: {
                    const double center = 0.25 * (v0 + v1 + v2 + v3);
                    if (center > iso) { addSeg(e3, e0); addSeg(e2, e1); }
                    else              { addSeg(e3, e2); addSeg(e0, e1); }
                    break;
                }
                case 6:  addSeg(e0, e2); break;
                case 7:  addSeg(e3, e2); break;
                case 8:  addSeg(e2, e3); break;
                case 9:  addSeg(e0, e2); break;
                case 10: {
                    const double center = 0.25 * (v0 + v1 + v2 + v3);
                    if (center > iso) { addSeg(e0, e3); addSeg(e1, e2); }
                    else              { addSeg(e0, e1); addSeg(e2, e3); }
                    break;
                }
                case 11: addSeg(e1, e2); break;
                case 12: addSeg(e1, e3); break;
                case 13: addSeg(e0, e1); break;
                case 14: addSeg(e3, e0); break;
                default: break;
            }
        }
    }

    if (segments.empty()) {
        return rings;
    }

    std::unordered_map<Key, std::vector<std::pair<int, int>>, KeyHash> endpoint_map;
    endpoint_map.reserve(segments.size() * 2);
    for (int i = 0; i < static_cast<int>(segments.size()); ++i) {
        endpoint_map[toKey(segments[static_cast<size_t>(i)].a)].push_back({i, 0});
        endpoint_map[toKey(segments[static_cast<size_t>(i)].b)].push_back({i, 1});
    }

    std::vector<uint8_t> used(segments.size(), 0);
    auto toWorld = [&](const Point2D& p) -> Point2D {
        return Point2D(xmin + p.x * cell, ymin + p.y * cell);
    };

    for (int si = 0; si < static_cast<int>(segments.size()); ++si) {
        if (used[static_cast<size_t>(si)]) {
            continue;
        }
        used[static_cast<size_t>(si)] = 1;

        Polygon2D ring;
        ring.reserve(128);
        Point2D start = segments[static_cast<size_t>(si)].a;
        Point2D curr = segments[static_cast<size_t>(si)].b;
        ring.push_back(toWorld(start));
        ring.push_back(toWorld(curr));

        int guard = 0;
        const int guard_max = static_cast<int>(segments.size()) * 2 + 16;
        bool closed = false;
        while (++guard <= guard_max) {
            if (std::hypot(curr.x - start.x, curr.y - start.y) <= close_tol) {
                closed = true;
                break;
            }
            auto it = endpoint_map.find(toKey(curr));
            if (it == endpoint_map.end()) {
                break;
            }

            int next_seg = -1;
            int next_end = -1;
            for (const auto& ent : it->second) {
                const int seg_idx = ent.first;
                const int end_idx = ent.second;
                if (used[static_cast<size_t>(seg_idx)]) continue;
                next_seg = seg_idx;
                next_end = end_idx;
                break;
            }

            if (next_seg < 0) {
                break;
            }
            used[static_cast<size_t>(next_seg)] = 1;
            const Segment& s = segments[static_cast<size_t>(next_seg)];
            const Point2D nxt = (next_end == 0) ? s.b : s.a;
            curr = nxt;
            ring.push_back(toWorld(curr));
        }

        if (!closed && ring.size() >= 4) {
            // Defensive close for tiny numerical cracks.
            const Point2D& first = ring.front();
            const Point2D& last = ring.back();
            if (std::hypot(last.x - first.x, last.y - first.y) <= close_tol * cell) {
                ring.back() = first;  // close cleanly for downstream cleaning.
                closed = true;
            }
        }

        if (closed && ring.size() >= 4) {
            rings.push_back(std::move(ring));
        }
    }

    return rings;
}

static std::vector<Polygon2D> extractContourRingsFromOcc(const OccGrid& g) {
    if (g.w <= 0 || g.h <= 0 || g.occ.empty()) {
        return {};
    }
    auto sample = [&](int cx, int cy) -> double {
        // Marching-squares samples live on cell centers; outside grid is empty.
        return occAt(g, cx, cy) ? 1.0 : 0.0;
    };
    return extractContourRingsGeneric(g.w, g.h, g.xmin, g.ymin, g.cell, 0.5, sample);
}

static std::vector<Polygon2D> extractContourRingsFromSdf(const ScalarGrid& g) {
    if (g.w <= 0 || g.h <= 0 || g.values.empty()) {
        return {};
    }
    auto sample = [&](int cx, int cy) -> double {
        return scalarAt(g, cx, cy);
    };
    return extractContourRingsGeneric(g.w, g.h, g.xmin, g.ymin, g.cell, 0.0, sample);
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
        // cross((b-a),(c-b))
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

static Polygon2D normalizeOpenRing(const Polygon2D& ring) {
    Polygon2D out = ring;
    if (out.size() >= 2 && std::hypot(out.front().x - out.back().x, out.front().y - out.back().y) <= 1e-9) {
        out.pop_back();
    }
    return out;
}

static double pointSegmentDistance(const Point2D& p, const Point2D& a, const Point2D& b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double len_sq = dx * dx + dy * dy;
    if (len_sq <= 1e-18) {
        return std::hypot(p.x - a.x, p.y - a.y);
    }
    double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len_sq;
    t = std::clamp(t, 0.0, 1.0);
    const double qx = a.x + t * dx;
    const double qy = a.y + t * dy;
    return std::hypot(p.x - qx, p.y - qy);
}

static double pointToRingDistance(const Point2D& p, const Polygon2D& ring) {
    const Polygon2D open = normalizeOpenRing(ring);
    if (open.empty()) {
        return std::numeric_limits<double>::infinity();
    }
    if (open.size() == 1) {
        return std::hypot(p.x - open.front().x, p.y - open.front().y);
    }

    double best = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < open.size(); ++i) {
        const Point2D& a = open[i];
        const Point2D& b = open[(i + 1) % open.size()];
        best = std::min(best, pointSegmentDistance(p, a, b));
    }
    return best;
}

static double maxRingToRingVertexDistance(const Polygon2D& src, const Polygon2D& dst) {
    const Polygon2D open = normalizeOpenRing(src);
    double worst = 0.0;
    for (const auto& p : open) {
        worst = std::max(worst, pointToRingDistance(p, dst));
    }
    return worst;
}

static double orient2D(const Point2D& a, const Point2D& b, const Point2D& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static bool pointOnSegment(const Point2D& p, const Point2D& a, const Point2D& b, double eps = 1e-9) {
    if (std::abs(orient2D(a, b, p)) > eps) {
        return false;
    }
    return p.x >= std::min(a.x, b.x) - eps && p.x <= std::max(a.x, b.x) + eps &&
           p.y >= std::min(a.y, b.y) - eps && p.y <= std::max(a.y, b.y) + eps;
}

static bool segmentsIntersect(const Point2D& a1, const Point2D& a2, const Point2D& b1, const Point2D& b2) {
    const double o1 = orient2D(a1, a2, b1);
    const double o2 = orient2D(a1, a2, b2);
    const double o3 = orient2D(b1, b2, a1);
    const double o4 = orient2D(b1, b2, a2);
    const double eps = 1e-9;

    if (((o1 > eps && o2 < -eps) || (o1 < -eps && o2 > eps)) &&
        ((o3 > eps && o4 < -eps) || (o3 < -eps && o4 > eps))) {
        return true;
    }
    if (std::abs(o1) <= eps && pointOnSegment(b1, a1, a2, eps)) return true;
    if (std::abs(o2) <= eps && pointOnSegment(b2, a1, a2, eps)) return true;
    if (std::abs(o3) <= eps && pointOnSegment(a1, b1, b2, eps)) return true;
    if (std::abs(o4) <= eps && pointOnSegment(a2, b1, b2, eps)) return true;
    return false;
}

static bool ringHasSelfIntersection(const Polygon2D& ring) {
    const Polygon2D open = normalizeOpenRing(ring);
    if (open.size() < 4) {
        return false;
    }
    for (size_t i = 0; i < open.size(); ++i) {
        const Point2D& a1 = open[i];
        const Point2D& a2 = open[(i + 1) % open.size()];
        for (size_t j = i + 1; j < open.size(); ++j) {
            if (j == i) continue;
            if ((i + 1) % open.size() == j) continue;
            if (i == 0 && (j + 1) % open.size() == 0) continue;

            const Point2D& b1 = open[j];
            const Point2D& b2 = open[(j + 1) % open.size()];
            if (segmentsIntersect(a1, a2, b1, b2)) {
                return true;
            }
        }
    }
    return false;
}

static size_t strongestCornerIndex(const Polygon2D& ring) {
    const Polygon2D open = normalizeOpenRing(ring);
    if (open.size() < 3) {
        return 0;
    }
    size_t best_idx = 0;
    double best_turn = -1.0;
    for (size_t i = 0; i < open.size(); ++i) {
        const Point2D& prev = open[(i + open.size() - 1) % open.size()];
        const Point2D& curr = open[i];
        const Point2D& next = open[(i + 1) % open.size()];
        const double ax = curr.x - prev.x;
        const double ay = curr.y - prev.y;
        const double bx = next.x - curr.x;
        const double by = next.y - curr.y;
        const double an = std::hypot(ax, ay);
        const double bn = std::hypot(bx, by);
        if (an <= 1e-12 || bn <= 1e-12) {
            continue;
        }
        const double cross = (ax * by - ay * bx) / (an * bn);
        const double dot = (ax * bx + ay * by) / (an * bn);
        const double turn = std::abs(std::atan2(cross, dot));
        if (turn > best_turn) {
            best_turn = turn;
            best_idx = i;
        }
    }
    return best_idx;
}

static bool runIsStraightEnough(
    const Polygon2D& ring,
    size_t begin,
    size_t end,
    double max_residual_m,
    double monotonic_tol_m) {
    if (end <= begin + 1) {
        return false;
    }
    const Point2D& a = ring[begin];
    const Point2D& b = ring[end];
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double chord = std::hypot(dx, dy);
    if (chord <= 1e-9) {
        return false;
    }
    const double ux = dx / chord;
    const double uy = dy / chord;
    double prev_t = -std::numeric_limits<double>::infinity();
    for (size_t i = begin; i <= end; ++i) {
        const double px = ring[i].x - a.x;
        const double py = ring[i].y - a.y;
        const double t = px * ux + py * uy;
        if (t + monotonic_tol_m < prev_t) {
            return false;
        }
        prev_t = t;
        if (i > begin && i < end && pointSegmentDistance(ring[i], a, b) > max_residual_m) {
            return false;
        }
    }
    return true;
}

static Polygon2D regularizeStraightSegments(const Polygon2D& ring, double cell_m) {
    Polygon2D open = normalizeOpenRing(ring);
    if (open.size() < 6 || cell_m <= 0.0) {
        return ring;
    }

    const size_t start_idx = strongestCornerIndex(open);
    Polygon2D ordered;
    ordered.reserve(open.size());
    for (size_t i = 0; i < open.size(); ++i) {
        ordered.push_back(open[(start_idx + i) % open.size()]);
    }

    const size_t min_run_points = 4;
    const double min_run_length_m = std::max(4.0 * cell_m, 0.35);
    const double max_residual_m = std::max(0.02, std::min(0.55 * cell_m, 0.05));
    const double monotonic_tol_m = std::max(1e-3, 0.10 * cell_m);

    Polygon2D simplified;
    simplified.reserve(ordered.size());
    size_t i = 0;
    while (i < ordered.size()) {
        if (simplified.empty() ||
            std::hypot(simplified.back().x - ordered[i].x, simplified.back().y - ordered[i].y) > 1e-9) {
            simplified.push_back(ordered[i]);
        }

        size_t best_end = i;
        double run_length_m = 0.0;
        for (size_t j = i + 1; j < ordered.size(); ++j) {
            run_length_m += std::hypot(ordered[j].x - ordered[j - 1].x, ordered[j].y - ordered[j - 1].y);
            if (j - i + 1 < min_run_points || run_length_m < min_run_length_m) {
                continue;
            }
            if (!runIsStraightEnough(ordered, i, j, max_residual_m, monotonic_tol_m)) {
                break;
            }
            best_end = j;
        }

        if (best_end > i + 1) {
            if (std::hypot(simplified.back().x - ordered[best_end].x,
                           simplified.back().y - ordered[best_end].y) > 1e-9) {
                simplified.push_back(ordered[best_end]);
            }
            i = best_end;
        } else {
            ++i;
        }
    }

    removeConsecutiveDuplicates(simplified);
    removeCollinear(simplified);
    if (simplified.size() < 3 || ringHasSelfIntersection(simplified)) {
        return ring;
    }

    const double base_area = polygonArea(open);
    const double new_area = polygonArea(simplified);
    const double abs_area_delta = std::abs(new_area - base_area);
    if (base_area > 1e-9) {
        const double rel_area_delta = abs_area_delta / base_area;
        if (rel_area_delta > 0.20 && abs_area_delta > std::max(2.0 * cell_m * cell_m, 0.05)) {
            return ring;
        }
    }

    const double max_dev_m = std::max(0.03, std::min(0.75 * cell_m, 0.08));
    const double deviation_m = std::max(
        maxRingToRingVertexDistance(open, simplified),
        maxRingToRingVertexDistance(simplified, open));
    if (deviation_m > max_dev_m) {
        return ring;
    }

    return simplified;
}

static bool ringInsideOuter(const Polygon2D& ring, const Polygon2D& outer) {
    const Polygon2D open = normalizeOpenRing(ring);
    if (open.empty()) {
        return false;
    }
    const size_t stride = std::max<size_t>(1, open.size() / 8);
    for (size_t i = 0; i < open.size(); i += stride) {
        if (!pointInPolyRayCast(open[i], outer)) {
            return false;
        }
    }
    return pointInPolyRayCast(open.back(), outer);
}

static ShapeContours regularizeShapeContours(ShapeContours shapes, double cell_m) {
    if (cell_m <= 0.0) {
        return shapes;
    }
    for (auto& shape : shapes) {
        const Polygon2D original_outer = shape.first;
        Polygon2D regularized_outer = regularizeStraightSegments(shape.first, cell_m);
        ensureCCW(regularized_outer);

        std::vector<Polygon2D> regularized_holes;
        regularized_holes.reserve(shape.second.size());
        bool keep_regularized_outer = true;
        for (const auto& original_hole : shape.second) {
            Polygon2D hole = regularizeStraightSegments(original_hole, cell_m);
            ensureCW(hole);
            if (!ringInsideOuter(hole, regularized_outer)) {
                hole = original_hole;
                ensureCW(hole);
                if (!ringInsideOuter(hole, regularized_outer)) {
                    keep_regularized_outer = false;
                }
            }
            regularized_holes.push_back(std::move(hole));
        }

        if (!keep_regularized_outer) {
            shape.first = original_outer;
            ensureCCW(shape.first);
            for (auto& hole : shape.second) {
                ensureCW(hole);
            }
            continue;
        }

        shape.first = std::move(regularized_outer);
        shape.second = std::move(regularized_holes);
    }
    return shapes;
}

static ShapeContours groupRingsIntoShapes(
    std::vector<Polygon2D> rings,
    double min_area_m2) {
    // Filter tiny/degenerate rings
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

    // Centroids = mean of vertices (matches python)
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

static int totalHoleCount(const ShapeContours& shapes) {
    int holes = 0;
    for (const auto& shape : shapes) {
        holes += static_cast<int>(shape.second.size());
    }
    return holes;
}

static bool sameShapeTopology(const ShapeContours& a, const ShapeContours& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].second.size() != b[i].second.size()) {
            return false;
        }
        const double area_a = polygonArea(a[i].first);
        const double area_b = polygonArea(b[i].first);
        const double denom = std::max({area_a, area_b, 1e-9});
        if (std::abs(area_a - area_b) / denom > 0.35) {
            return false;
        }
    }
    return totalHoleCount(a) == totalHoleCount(b);
}

static ShapeContours extractShapesFromOcc(const OccGrid& g, double min_area_m2) {
    if (g.w <= 0 || g.h <= 0 || g.occ.empty()) {
        return {};
    }

    ShapeContours baseline = groupRingsIntoShapes(extractContourRingsFromOcc(g), min_area_m2);
    ShapeContours chosen = baseline;

    // Only promote the signed-distance contour when it preserves the same topology
    // as the current binary pipeline; otherwise keep the existing behavior.
    if (!baseline.empty()) {
        ScalarGrid sdf = signedDistanceFieldFromOcc(g);
        if (!sdf.values.empty()) {
            ShapeContours refined = groupRingsIntoShapes(extractContourRingsFromSdf(sdf), min_area_m2);
            if (!refined.empty() && sameShapeTopology(refined, baseline)) {
                chosen = std::move(refined);
            }
        }
    }

    return regularizeShapeContours(std::move(chosen), g.cell);
}

static ShapeContours polygonizeClusterGridLikePython(
    const std::vector<Point2D>& pts2d,
    double grid_cell_m,
    double contour_cell_m,
    double inflate_radius_m,
    double smooth_radius_m,
    double min_contour_area_m2) {
    if (pts2d.empty() || grid_cell_m <= 0.0) return {};
    const double padding = std::max(0.20, 2.0 * inflate_radius_m);
    OccGrid occ = occupancyFromPoints(pts2d, grid_cell_m, padding);
    occ = inflateOccupancy(occ, inflate_radius_m);
    occ = closeOccupancyConservative(occ, smooth_radius_m);

    // Optional contour coarsening (conservative max-pooling), matching python's contour_cell.
    int factor = 1;
    if (contour_cell_m > grid_cell_m) {
        factor = std::max(1, static_cast<int>(std::lround(contour_cell_m / grid_cell_m)));
    }
    if (factor > 1) {
        occ = maxpoolOccupancy(occ, factor);
    }

    return extractShapesFromOcc(occ, min_contour_area_m2);
}

}  // namespace

ObstacleDetectionResult detectObstaclesAuto(
    const PointCloudPtr& cloud,
    const std::vector<PathState>& driven_path,
    const Polygon2D* roi_or_boundary,
    const ObstacleDetectionParams& params) {
    ObstacleDetectionResult res;
    auto abortIfCancelled = [&res]() -> bool {
        if (!g_obstacleCancelCallback || !g_obstacleCancelCallback()) {
            return false;
        }
        res.error_message = "Operation cancelled";
        return true;
    };
    if (!cloud || cloud->empty()) {
        res.error_message = "No point cloud loaded.";
        return res;
    }

    const Polygon2D scope = effectiveScopePolygon(roi_or_boundary);
    PointCloudPtr scoped_cloud = filterCloudToPolygon(cloud, scope);
    res.stats.input_points = cloud->size();
    res.stats.roi_points = scoped_cloud ? scoped_cloud->size() : 0;

    // Filter path to ROI (if provided)
    std::vector<PathState> path = driven_path;
    if (!scope.empty()) {
        path = filterPathToPolygon(driven_path, scope);
    }
    res.stats.path_poses = path.size();
    if (abortIfCancelled()) {
        return res;
    }

    // Derived defaults (match Python main()).
    double grid_cell_m = params.grid_cell_m;
    if (grid_cell_m <= 0.0) {
        grid_cell_m = 0.09;
    }
    double contour_cell_m = params.contour_cell_m;
    if (contour_cell_m < 0.0) {
        contour_cell_m = 2.0 * grid_cell_m;
    }
    if (contour_cell_m < grid_cell_m) {
        contour_cell_m = grid_cell_m;
    }
    double smooth_radius_m = params.smooth_radius_m;
    if (smooth_radius_m < 0.0) {
        smooth_radius_m = 2.0 * grid_cell_m;
    }
    if (smooth_radius_m < 0.0) {
        smooth_radius_m = 0.0;
    }
    const double inflate_radius_m = std::max(0.0, params.inflate_radius_m);
    const double geom_smooth_radius_m = std::max(0.0, params.geom_smooth_radius_m);
    const bool preserve_holes = params.preserve_holes;
    const double preserve_holes_min_area_m2 = std::max(0.0, params.preserve_holes_min_area_m2);

    // ------------------------------------------------------------------
    // 3. Ground detection
    // ------------------------------------------------------------------
    PointCloudPtr fp_ground(new PointCloud);
    if (!path.empty()) {
        fp_ground = extractFootprintGround(
            scoped_cloud, path,
            params.robot_length_m, params.robot_width_m, params.footprint_margin_m,
            params.ground_z_max);
        if (fp_ground->size() < 10) {
            // Fallback: z-threshold
            std::cout << "[ObstacleDetect] Ground: footprint sample too small ("
                      << fp_ground->size() << "), falling back to z-threshold (z <= "
                      << params.ground_z_max << ")\n";
            fp_ground->clear();
            size_t ground_idx = 0;
            for (const auto& pt : scoped_cloud->points) {
                if ((ground_idx++ & 0x1FFFu) == 0u && abortIfCancelled()) {
                    return res;
                }
                if (pt.z <= params.ground_z_max) fp_ground->push_back(pt);
            }
        } else {
            std::cout << "[ObstacleDetect] Ground: using footprint path (poses="
                      << path.size() << ", points=" << fp_ground->size()
                      << ", z_max=" << params.ground_z_max << ")\n";
        }
    } else {
        std::cout << "[ObstacleDetect] Ground: no path provided, using z-threshold (z <= "
                  << params.ground_z_max << ")\n";
        size_t ground_idx = 0;
        for (const auto& pt : scoped_cloud->points) {
            if ((ground_idx++ & 0x1FFFu) == 0u && abortIfCancelled()) {
                return res;
            }
            if (pt.z <= params.ground_z_max) fp_ground->push_back(pt);
        }
    }
    res.stats.footprint_ground_points = fp_ground->size();
    if (abortIfCancelled()) {
        return res;
    }

    PlaneModel plane;
    if (fp_ground->size() < 20) {
        std::cout << "[ObstacleDetect] Ground: using median-Z flat plane (n="
                  << fp_ground->size() << ")\n";
        plane.nx = 0.0;
        plane.ny = 0.0;
        plane.nz = 1.0;
        plane.d = -medianZ(fp_ground);
    } else {
        std::cout << "[ObstacleDetect] Ground: fitting RANSAC plane (n="
                  << fp_ground->size() << ", iters=" << params.ransac_iters
                  << ", thresh=" << params.ransac_thresh_m << ")\n";
        plane = fitPlaneRansac(fp_ground, params.ransac_iters, params.ransac_thresh_m);
    }
    res.stats.plane_nx = plane.nx;
    res.stats.plane_ny = plane.ny;
    res.stats.plane_nz = plane.nz;
    res.stats.plane_d = plane.d;

    // ------------------------------------------------------------------
    // 4. Obstacle candidate extraction
    // ------------------------------------------------------------------
    PointCloudPtr obstacle_raw(new PointCloud);
    obstacle_raw->reserve(scoped_cloud->size() / 4);
    size_t ground_band_count = 0;
    size_t candidate_idx = 0;
    for (const auto& pt : scoped_cloud->points) {
        if ((candidate_idx++ & 0x1FFFu) == 0u && abortIfCancelled()) {
            return res;
        }
        double sd = signedDist(plane, pt);
        bool is_ground = std::abs(sd) <= params.ground_band_m;
        if (is_ground) {
            ground_band_count++;
        }
        bool positive = (sd > params.ground_band_m) && (pt.z <= params.obstacle_z_max);
        bool trough = (sd < -(params.ground_band_m + params.trough_depth_m)) && (pt.z <= params.ground_z_max);
        if (positive || trough) {
            obstacle_raw->push_back(pt);
        }
    }
    res.stats.ground_points_band = ground_band_count;
    res.stats.raw_obstacle_candidates = obstacle_raw->size();

    // ------------------------------------------------------------------
    // 5. Statistical outlier removal (PCL SOR)
    // ------------------------------------------------------------------
    // Python-like filter based on mean kNN distance statistics.
    PointCloudPtr obstacle_clean = removeStatisticalOutliersMeanDist(
        obstacle_raw, params.outlier_k, params.outlier_std);
    res.stats.obstacle_points_after_outlier = obstacle_clean ? obstacle_clean->size() : 0;
    if (abortIfCancelled()) {
        return res;
    }

    // ------------------------------------------------------------------
    // 6. 2D projection + DBSCAN
    // ------------------------------------------------------------------
    std::vector<Point2D> obs_xy;
    obs_xy.reserve(obstacle_clean->size());
    size_t obs_xy_idx = 0;
    for (const auto& pt : obstacle_clean->points) {
        if ((obs_xy_idx++ & 0x1FFFu) == 0u && abortIfCancelled()) {
            return res;
        }
        obs_xy.emplace_back(pt.x, pt.y);
    }
    std::vector<int> labels = dbscan2D(obs_xy, params.cluster_eps_m, params.cluster_min_pts);
    if (abortIfCancelled()) {
        return res;
    }
    int max_label = -1;
    for (int l : labels) max_label = std::max(max_label, l);
    const int n_clusters = max_label + 1;
    res.stats.clusters_found = n_clusters;

    std::vector<std::vector<Point2D>> clusters;
    clusters.resize(static_cast<size_t>(n_clusters));
    std::vector<Point2D> noise_pts;
    noise_pts.reserve(obs_xy.size());
    for (size_t i = 0; i < obs_xy.size(); ++i) {
        if ((i & 0x1FFFu) == 0u && abortIfCancelled()) {
            return res;
        }
        int l = labels[i];
        if (l < 0) {
            noise_pts.push_back(obs_xy[i]);
            continue;
        }
        clusters[static_cast<size_t>(l)].push_back(obs_xy[i]);
    }

    // Remove empty clusters (shouldn't happen, but keep safe)
    std::vector<std::vector<Point2D>> cluster_list;
    cluster_list.reserve(clusters.size());
    for (auto& c : clusters) {
        if (!c.empty()) cluster_list.push_back(std::move(c));
    }
    clusters.clear();

    // ------------------------------------------------------------------
    // 6b. Preserve micro obstacles (tiny but dense) (matches Python)
    // ------------------------------------------------------------------
    std::vector<Obstacle2D> micro_obstacles;
    if (params.micro_enable && !obs_xy.empty()) {
        std::vector<std::vector<Point2D>> normal_clusters;
        normal_clusters.reserve(cluster_list.size());
        size_t cluster_idx = 0;
        for (auto& cl : cluster_list) {
            if ((cluster_idx++ & 0x3Fu) == 0u && abortIfCancelled()) {
                return res;
            }
            if (isMicroCluster(cl, params)) {
                Obstacle2D obs;
                obs.outer = rectFromBbox(cl, params.micro_min_size_m, params.micro_margin_m);
                micro_obstacles.push_back(std::move(obs));
            } else {
                normal_clusters.push_back(std::move(cl));
            }
        }
        cluster_list = std::move(normal_clusters);

        // Recover micro obstacles from noise points (labels == -1) using tighter DBSCAN
        std::vector<Point2D> noise_pts;
        noise_pts.reserve(obs_xy.size());
        for (size_t i = 0; i < obs_xy.size(); ++i) {
            if (labels[i] == -1) {
                noise_pts.push_back(obs_xy[i]);
            }
        }
        if (static_cast<int>(noise_pts.size()) >= params.micro_min_pts && params.micro_noise_eps_m > 0.0) {
            std::vector<int> micro_labels = dbscan2D(noise_pts, params.micro_noise_eps_m, params.micro_min_pts);
            if (abortIfCancelled()) {
                return res;
            }
            int micro_max_label = -1;
            for (int l : micro_labels) micro_max_label = std::max(micro_max_label, l);
            const int micro_n_clusters = micro_max_label + 1;
            if (micro_n_clusters > 0) {
                std::vector<std::vector<Point2D>> micro_clusters(static_cast<size_t>(micro_n_clusters));
                for (size_t i = 0; i < noise_pts.size(); ++i) {
                    int l = micro_labels[i];
                    if (l < 0) continue;
                    micro_clusters[static_cast<size_t>(l)].push_back(noise_pts[i]);
                }
                for (auto& mc : micro_clusters) {
                    if (mc.empty()) continue;
                    if (!isMicroCluster(mc, params)) continue;
                    Obstacle2D obs;
                    obs.outer = rectFromBbox(mc, params.micro_min_size_m, params.micro_margin_m);
                    micro_obstacles.push_back(std::move(obs));
                }
            }
        }
    }

    if (cluster_list.empty()) {
        // Only micro obstacles exist (or no obstacles at all)
        res.stats.total_holes = 0;
        res.stats.obstacle_shapes = static_cast<int>(micro_obstacles.size());
        res.obstacles = std::move(micro_obstacles);
        res.success = true;
        return res;
    }

    // ------------------------------------------------------------------
    // 7. Merge nearby clusters (union-find) + polygonize groups (AUTO)
    // ------------------------------------------------------------------
    const double merge_d2 = params.merge_distance_m * params.merge_distance_m;

    struct ClusterKD {
        std::vector<Point2D> pts;
        Point2D minp{0, 0};
        Point2D maxp{0, 0};
        pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_cloud;
        std::unique_ptr<pcl::KdTreeFLANN<pcl::PointXYZ>> tree;
    };

    std::vector<ClusterKD> ck;
    ck.reserve(cluster_list.size());
    for (auto& c : cluster_list) {
        ClusterKD entry;
        entry.pts = std::move(c);
        entry.minp = entry.maxp = entry.pts.front();
        for (const auto& p : entry.pts) {
            entry.minp.x = std::min(entry.minp.x, p.x);
            entry.minp.y = std::min(entry.minp.y, p.y);
            entry.maxp.x = std::max(entry.maxp.x, p.x);
            entry.maxp.y = std::max(entry.maxp.y, p.y);
        }
        entry.pcl_cloud.reset(new pcl::PointCloud<pcl::PointXYZ>);
        entry.pcl_cloud->reserve(entry.pts.size());
        for (const auto& p : entry.pts) {
            entry.pcl_cloud->push_back(pcl::PointXYZ(static_cast<float>(p.x), static_cast<float>(p.y), 0.0f));
        }
        entry.tree = std::make_unique<pcl::KdTreeFLANN<pcl::PointXYZ>>();
        entry.tree->setInputCloud(entry.pcl_cloud);
        ck.push_back(std::move(entry));
    }

    struct UnionFind {
        std::vector<int> parent;
        explicit UnionFind(int n) : parent(static_cast<size_t>(n)) {
            for (int i = 0; i < n; ++i) parent[static_cast<size_t>(i)] = i;
        }
        int find(int x) {
            int r = x;
            while (parent[static_cast<size_t>(r)] != r) {
                r = parent[static_cast<size_t>(r)];
            }
            while (parent[static_cast<size_t>(x)] != x) {
                int px = parent[static_cast<size_t>(x)];
                parent[static_cast<size_t>(x)] = r;
                x = px;
            }
            return r;
        }
        void unite(int a, int b) {
            int ra = find(a);
            int rb = find(b);
            if (ra != rb) parent[static_cast<size_t>(ra)] = rb;
        }
    };

    UnionFind uf(static_cast<int>(ck.size()));

    auto aabbMinDist2 = [](const ClusterKD& a, const ClusterKD& b) -> double {
        double dx = 0.0;
        if (a.maxp.x < b.minp.x) dx = b.minp.x - a.maxp.x;
        else if (b.maxp.x < a.minp.x) dx = a.minp.x - b.maxp.x;
        double dy = 0.0;
        if (a.maxp.y < b.minp.y) dy = b.minp.y - a.maxp.y;
        else if (b.maxp.y < a.minp.y) dy = a.minp.y - b.maxp.y;
        return dx * dx + dy * dy;
    };

    std::vector<int> nn_idx(1);
    std::vector<float> nn_dist2(1);

    for (int i = 0; i < static_cast<int>(ck.size()); ++i) {
        if ((i & 0x0Fu) == 0 && abortIfCancelled()) {
            return res;
        }
        for (int j = i + 1; j < static_cast<int>(ck.size()); ++j) {
            if (aabbMinDist2(ck[static_cast<size_t>(i)], ck[static_cast<size_t>(j)]) > merge_d2) {
                continue;
            }
            double best_d2 = std::numeric_limits<double>::infinity();
            for (const auto& p : ck[static_cast<size_t>(i)].pcl_cloud->points) {
                int found = ck[static_cast<size_t>(j)].tree->nearestKSearch(p, 1, nn_idx, nn_dist2);
                if (found > 0) {
                    best_d2 = std::min(best_d2, static_cast<double>(nn_dist2[0]));
                    if (best_d2 <= merge_d2) break;
                }
            }
            if (best_d2 <= merge_d2) {
                uf.unite(i, j);
            }
        }
    }

    std::unordered_map<int, std::vector<int>> groups;
    groups.reserve(ck.size());
    for (int i = 0; i < static_cast<int>(ck.size()); ++i) {
        groups[uf.find(i)].push_back(i);
    }
    res.stats.groups_merged = static_cast<int>(groups.size());

    std::vector<Obstacle2D> obstacles_out;
    int total_holes = 0;

    for (const auto& kv : groups) {
        if (abortIfCancelled()) {
            return res;
        }
        const auto& idxs = kv.second;
        std::vector<Point2D> merged_pts;
        size_t total_pts = 0;
        for (int ci : idxs) total_pts += ck[static_cast<size_t>(ci)].pts.size();
        merged_pts.reserve(total_pts);
        for (int ci : idxs) {
            const auto& pts = ck[static_cast<size_t>(ci)].pts;
            merged_pts.insert(merged_pts.end(), pts.begin(), pts.end());
        }
        if (merged_pts.empty()) continue;

        // Representative hull (used for AUTO heuristics and hull fallback)
        Polygon2D merged_hull;
        try {
            if (merged_pts.size() >= 3) {
                merged_hull = computeConvexHull(merged_pts);
            } else {
                merged_hull = rectFromBbox(merged_pts, 0.05, 0.0);
            }
        } catch (...) {
            merged_hull = rectFromBbox(merged_pts, 0.05, 0.0);
        }
        double hull_area = polygonArea(merged_hull);
        if (hull_area <= 1e-12) {
            merged_hull = rectFromBbox(merged_pts, 0.05, 0.0);
            hull_area = polygonArea(merged_hull);
        }

        bool use_grid = false;
        bool allow_grid = (params.polygon_mode == ObstaclePolygonMode::Auto ||
                           params.polygon_mode == ObstaclePolygonMode::Grid);

        if (allow_grid) {
            const bool prefer_grid = (params.polygon_mode == ObstaclePolygonMode::Auto) &&
                (geom_smooth_radius_m > 0.0 || contour_cell_m > grid_cell_m + 1e-12);

            // Hollow ratio computed from *non-inflated* occupancy (padding=0.20)
            bool hollow_trigger = false;
            if (grid_cell_m > 0.0 && hull_area > params.min_contour_area_m2) {
                OccGrid occ0 = occupancyFromPoints(merged_pts, grid_cell_m, 0.20);
                size_t occ_sum = 0;
                for (uint8_t v : occ0.occ) occ_sum += (v != 0);
                const double occ_area = static_cast<double>(occ_sum) * (grid_cell_m * grid_cell_m);
                const double hollow_ratio = occ_area / std::max(hull_area, 1e-9);
                hollow_trigger = (hollow_ratio < params.hollow_ratio_thresh);
            }

            use_grid = (params.polygon_mode == ObstaclePolygonMode::Grid) || prefer_grid || hollow_trigger;
        }

        if (use_grid) {
            // Keep polygonization simple; apply smoothing once as a post-process (matches Python).
            auto shapes = polygonizeClusterGrid(
                merged_pts,
                grid_cell_m,
                contour_cell_m,
                inflate_radius_m,
                /*smooth_radius_m=*/0.0,
                params.min_contour_area_m2);

            if (!shapes.empty()) {
                auto pointInAnyShape = [&](const Point2D& p) -> bool {
                    for (const auto& sh : shapes) {
                        if (!pointInPolyRayCast(p, sh.first)) continue;
                        bool in_hole = false;
                        for (const auto& h : sh.second) {
                            if (pointInPolyRayCast(p, h)) {
                                in_hole = true;
                                break;
                            }
                        }
                        if (!in_hole) return true;
                    }
                    return false;
                };

                // Preserve tiny/disconnected source clusters that can be dropped by contour cleanup.
                for (int ci : idxs) {
                    const auto& cpts = ck[static_cast<size_t>(ci)].pts;
                    if (cpts.empty()) continue;
                    bool covered = false;
                    const size_t stride = std::max<size_t>(1, cpts.size() / 12);
                    for (size_t k = 0; k < cpts.size(); k += stride) {
                        if (pointInAnyShape(cpts[k])) {
                            covered = true;
                            break;
                        }
                    }
                    if (!covered && pointInAnyShape(cpts.back())) {
                        covered = true;
                    }
                    if (covered) continue;

                    Polygon2D tiny = rectFromBbox(cpts, std::max(0.02, 0.75 * grid_cell_m), 0.0);
                    shapes.emplace_back(std::move(tiny), std::vector<Polygon2D>{});
                }

                for (auto& sh : shapes) {
                    Obstacle2D obs;
                    obs.outer = std::move(sh.first);
                    obs.holes = std::move(sh.second);
                    total_holes += static_cast<int>(obs.holes.size());
                    obstacles_out.push_back(std::move(obs));
                }
                continue;
            }
            // If grid fails, fall back to hull.
        }

        Obstacle2D obs;
        obs.outer = std::move(merged_hull);
        obstacles_out.push_back(std::move(obs));
    }

    // 7c. Optional geometric smoothing (rolling-disk closing) on final polygons (grid fallback).
    if (geom_smooth_radius_m > 0.0 && !obstacles_out.empty()) {
        obstacles_out = smoothShapesRollingDiskGrid(
            obstacles_out,
            geom_smooth_radius_m,
            grid_cell_m,
            contour_cell_m,
            params.min_contour_area_m2,
            preserve_holes,
            preserve_holes_min_area_m2);
    }
    if (abortIfCancelled()) {
        return res;
    }

    // Append preserved micro obstacles unchanged (Python skips smoothing for micro shapes).
    if (!micro_obstacles.empty()) {
        obstacles_out.reserve(obstacles_out.size() + micro_obstacles.size());
        for (auto& m : micro_obstacles) {
            obstacles_out.push_back(std::move(m));
        }
    }

    int final_holes = 0;
    for (const auto& o : obstacles_out) {
        final_holes += static_cast<int>(o.holes.size());
    }
    res.stats.total_holes = final_holes;
    res.stats.obstacle_shapes = static_cast<int>(obstacles_out.size());
    res.obstacles = std::move(obstacles_out);
    res.success = true;
    return res;
}

}  // namespace f2c_cpp

