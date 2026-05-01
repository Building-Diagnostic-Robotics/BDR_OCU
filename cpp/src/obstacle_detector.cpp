/**
 * @file obstacle_detector.cpp
 * @brief Implementation of automatic obstacle detection (AUTO mode).
 */

#include "obstacle_detector.hpp"

#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/kdtree/kdtree_flann.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <unordered_map>

namespace f2c_cpp {

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

    std::vector<Polygon2D> rings = extractContourRingsFromOcc(occ);
    return groupRingsIntoShapes(std::move(rings), min_contour_area_m2);
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

        std::vector<Polygon2D> rings = extractContourRingsFromOcc(occ2);
        auto grouped = groupRingsIntoShapes(std::move(rings), min_contour_area_m2);
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

struct Edge {
    int x0, y0;
    int x1, y1;
};

static inline uint64_t packV(int x, int y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
}

static std::vector<Polygon2D> extractContourRingsFromOcc(const OccGrid& g) {
    std::vector<Edge> edges;
    edges.reserve(static_cast<size_t>(g.w) * static_cast<size_t>(g.h));

    // Build directed boundary edges with "occupied" on the left.
    for (int y = 0; y < g.h; ++y) {
        for (int x = 0; x < g.w; ++x) {
            if (!occAt(g, x, y)) continue;

            // Corners in vertex-grid coordinates
            int vx0 = x;
            int vx1 = x + 1;
            int vy0 = y;
            int vy1 = y + 1;

            // Neighbour checks (N,S,E,W)
            bool n_occ = occAt(g, x, y + 1);
            bool s_occ = occAt(g, x, y - 1);
            bool e_occ = occAt(g, x + 1, y);
            bool w_occ = occAt(g, x - 1, y);

            if (!n_occ) {
                // top: (vx1,vy1)->(vx0,vy1)
                edges.push_back({vx1, vy1, vx0, vy1});
            }
            if (!s_occ) {
                // bottom: (vx0,vy0)->(vx1,vy0)
                edges.push_back({vx0, vy0, vx1, vy0});
            }
            if (!e_occ) {
                // right: (vx1,vy0)->(vx1,vy1)
                edges.push_back({vx1, vy0, vx1, vy1});
            }
            if (!w_occ) {
                // left: (vx0,vy1)->(vx0,vy0)
                edges.push_back({vx0, vy1, vx0, vy0});
            }
        }
    }

    // Map start vertex -> outgoing edges
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

            // Choose next edge by left-hand rule: left, straight, right, back.
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

        // Convert to world coordinates at grid vertices.
        Polygon2D ring;
        ring.reserve(verts.size());
        for (const auto& v : verts) {
            double wx = g.xmin + static_cast<double>(v.first) * g.cell;
            double wy = g.ymin + static_cast<double>(v.second) * g.cell;
            ring.emplace_back(wx, wy);
        }
        rings.push_back(std::move(ring));
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

static std::vector<std::pair<Polygon2D, std::vector<Polygon2D>>> groupRingsIntoShapes(
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

    // Filter path to ROI (if provided)
    std::vector<PathState> path = driven_path;
    if (!scope.empty()) {
        path = filterPathToPolygon(driven_path, scope);
    }
    res.stats.path_poses = path.size();

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
            fp_ground->clear();
            for (const auto& pt : scoped_cloud->points) {
                if (pt.z <= params.ground_z_max) fp_ground->push_back(pt);
            }
        }
    } else {
        for (const auto& pt : scoped_cloud->points) {
            if (pt.z <= params.ground_z_max) fp_ground->push_back(pt);
        }
    }
    res.stats.footprint_ground_points = fp_ground->size();

    PlaneModel plane;
    if (fp_ground->size() < 20) {
        plane.nx = 0.0;
        plane.ny = 0.0;
        plane.nz = 1.0;
        plane.d = -medianZ(fp_ground);
    } else {
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
    for (const auto& pt : scoped_cloud->points) {
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
    PointCloudPtr obstacle_clean(new PointCloud);
    if (!obstacle_raw->empty()) {
        pcl::StatisticalOutlierRemoval<pcl::PointXYZ> sor;
        sor.setInputCloud(obstacle_raw);
        sor.setMeanK(std::max(1, params.outlier_k));
        sor.setStddevMulThresh(params.outlier_std);
        sor.filter(*obstacle_clean);
    }
    res.stats.obstacle_points_after_outlier = obstacle_clean->size();

    // ------------------------------------------------------------------
    // 6. 2D projection + DBSCAN
    // ------------------------------------------------------------------
    std::vector<Point2D> obs_xy;
    obs_xy.reserve(obstacle_clean->size());
    for (const auto& pt : obstacle_clean->points) {
        obs_xy.emplace_back(pt.x, pt.y);
    }
    std::vector<int> labels = dbscan2D(obs_xy, params.cluster_eps_m, params.cluster_min_pts);
    int max_label = -1;
    for (int l : labels) max_label = std::max(max_label, l);
    const int n_clusters = max_label + 1;
    res.stats.clusters_found = n_clusters;

    std::vector<std::vector<Point2D>> clusters;
    clusters.resize(static_cast<size_t>(n_clusters));
    for (size_t i = 0; i < obs_xy.size(); ++i) {
        int l = labels[i];
        if (l < 0) continue;
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
        for (auto& cl : cluster_list) {
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

