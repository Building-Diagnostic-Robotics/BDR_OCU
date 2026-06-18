#include "grid_planner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace f2c_cpp {

namespace {
constexpr float kInfDt = 1e20f;
const double kSqrt2m1 = std::sqrt(2.0) - 1.0;

// Exact 1D squared-distance transform (Felzenszwalb & Huttenlocher).
void dt1d(const std::vector<float>& f, std::vector<float>& d, int n) {
    std::vector<int> v(n);
    std::vector<float> z(n + 1);
    int k = 0;
    v[0] = 0;
    z[0] = -kInfDt;
    z[1] = kInfDt;
    for (int q = 1; q < n; ++q) {
        float s = ((f[q] + static_cast<float>(q) * q) -
                   (f[v[k]] + static_cast<float>(v[k]) * v[k])) /
                  (2.0f * q - 2.0f * v[k]);
        while (s <= z[k]) {
            --k;
            s = ((f[q] + static_cast<float>(q) * q) -
                 (f[v[k]] + static_cast<float>(v[k]) * v[k])) /
                (2.0f * q - 2.0f * v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInfDt;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) ++k;
        const float dq = static_cast<float>(q - v[k]);
        d[q] = dq * dq + f[v[k]];
    }
}
}  // namespace

bool GridPlanner::build(const std::vector<Obstacle2D>& obstacles, double min_x,
                        double min_y, double max_x, double max_y, double resolution,
                        double inflation_radius) {
    lethal_.clear();
    clearance_.clear();
    if (!(max_x > min_x) || !(max_y > min_y) || resolution <= 0.0) return false;

    res_ = resolution;
    inflation_ = inflation_radius;
    ox_ = min_x;
    oy_ = min_y;
    w_ = static_cast<int>(std::ceil((max_x - min_x) / res_)) + 1;
    h_ = static_cast<int>(std::ceil((max_y - min_y) / res_)) + 1;
    if (w_ < 2 || h_ < 2) return false;
    // Auto-coarsen so a pathological bbox can't allocate unbounded memory.
    while (static_cast<long long>(w_) * h_ > kMaxCells) {
        res_ *= 1.5;
        w_ = static_cast<int>(std::ceil((max_x - min_x) / res_)) + 1;
        h_ = static_cast<int>(std::ceil((max_y - min_y) / res_)) + 1;
    }

    std::vector<uint8_t> occupied(static_cast<size_t>(w_) * h_, 0);

    // Scanline-fill a ring into `occupied` with `val`.
    auto fillRing = [&](const Polygon2D& ring, uint8_t val) {
        const int n = static_cast<int>(ring.size());
        if (n < 3) return;
        double rmin_y = ring[0].y, rmax_y = ring[0].y;
        for (const auto& p : ring) {
            rmin_y = std::min(rmin_y, p.y);
            rmax_y = std::max(rmax_y, p.y);
        }
        int cy0 = std::max(0, static_cast<int>(std::floor((rmin_y - oy_) / res_)));
        int cy1 = std::min(h_ - 1, static_cast<int>(std::ceil((rmax_y - oy_) / res_)));
        std::vector<double> xs;
        for (int cy = cy0; cy <= cy1; ++cy) {
            const double wy = oy_ + (cy + 0.5) * res_;
            xs.clear();
            for (int i = 0, j = n - 1; i < n; j = i++) {
                const double yi = ring[i].y, yj = ring[j].y;
                if ((yi > wy) != (yj > wy)) {
                    const double t = (wy - yi) / (yj - yi);
                    xs.push_back(ring[i].x + t * (ring[j].x - ring[i].x));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (size_t s = 0; s + 1 < xs.size(); s += 2) {
                int cx0 = static_cast<int>(std::ceil((xs[s] - ox_) / res_ - 0.5));
                int cx1 = static_cast<int>(std::floor((xs[s + 1] - ox_) / res_ - 0.5));
                cx0 = std::max(0, cx0);
                cx1 = std::min(w_ - 1, cx1);
                for (int cx = cx0; cx <= cx1; ++cx) occupied[idx(cx, cy)] = val;
            }
        }
        // Stamp the outline so sub-cell-thin solids never vanish between
        // scanlines (cell-center fill alone can miss them). Outer rings only;
        // stamping a hole edge would punch a gap in the surrounding solid.
        if (val == 1) {
            for (int i = 0, j = n - 1; i < n; j = i++) {
                int x0, y0, x1, y1;
                worldToCell(ring[j], x0, y0);
                worldToCell(ring[i], x1, y1);
                int ddx = std::abs(x1 - x0), ddy = std::abs(y1 - y0);
                int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
                int err = ddx - ddy, x = x0, y = y0;
                while (true) {
                    if (inBounds(x, y)) occupied[idx(x, y)] = 1;
                    if (x == x1 && y == y1) break;
                    const int e2 = 2 * err;
                    if (e2 > -ddy) { err -= ddy; x += sx; }
                    if (e2 < ddx) { err += ddx; y += sy; }
                }
            }
        }
    };

    for (const auto& obs : obstacles) {
        fillRing(obs.outer, 1);
        for (const auto& hole : obs.holes) fillRing(hole, 0);
    }

    computeEdt(occupied);
    lethal_.assign(static_cast<size_t>(w_) * h_, 0);
    for (size_t i = 0; i < lethal_.size(); ++i) {
        if (clearance_[i] < inflation_) lethal_[i] = 1;
    }
    return true;
}

void GridPlanner::computeEdt(const std::vector<uint8_t>& occupied) {
    const int n = w_ * h_;
    std::vector<float> g(n);
    for (int i = 0; i < n; ++i) g[i] = occupied[i] ? 0.0f : kInfDt;

    // Columns then rows (squared distances), then sqrt -> metres.
    std::vector<float> col(h_), dcol(h_);
    for (int x = 0; x < w_; ++x) {
        for (int y = 0; y < h_; ++y) col[y] = g[idx(x, y)];
        dt1d(col, dcol, h_);
        for (int y = 0; y < h_; ++y) g[idx(x, y)] = dcol[y];
    }
    std::vector<float> row(w_), drow(w_);
    clearance_.assign(n, 0.0f);
    for (int y = 0; y < h_; ++y) {
        for (int x = 0; x < w_; ++x) row[x] = g[idx(x, y)];
        dt1d(row, drow, w_);
        for (int x = 0; x < w_; ++x)
            clearance_[idx(x, y)] = std::sqrt(drow[x]) * static_cast<float>(res_);
    }
}

bool GridPlanner::worldToCell(const Point2D& p, int& cx, int& cy) const {
    cx = static_cast<int>(std::floor((p.x - ox_) / res_));
    cy = static_cast<int>(std::floor((p.y - oy_) / res_));
    return inBounds(cx, cy);
}

Point2D GridPlanner::cellToWorld(int cx, int cy) const {
    return Point2D(ox_ + (cx + 0.5) * res_, oy_ + (cy + 0.5) * res_);
}

float GridPlanner::clearanceAt(int cx, int cy) const {
    if (!inBounds(cx, cy)) return 0.0f;
    return clearance_[idx(cx, cy)];
}

float GridPlanner::clearanceAtWorld(const Point2D& p) const {
    int cx, cy;
    if (!worldToCell(p, cx, cy)) return 0.0f;
    return clearance_[idx(cx, cy)];
}

bool GridPlanner::snapToFree(int& cx, int& cy, double radius_m) const {
    if (freeCell(cx, cy)) return true;
    const int r = std::max(1, static_cast<int>(std::ceil(radius_m / res_)));
    int best = -1;
    double best_d2 = std::numeric_limits<double>::max();
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            const int nx = cx + dx, ny = cy + dy;
            if (!freeCell(nx, ny)) continue;
            const double d2 = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = idx(nx, ny);
            }
        }
    }
    if (best < 0) return false;
    cx = best % w_;
    cy = best / w_;
    return true;
}

bool GridPlanner::losCells(int x0, int y0, int x1, int y1) const {
    int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int x = x0, y = y0;
    while (true) {
        if (!freeCell(x, y)) return false;
        if (x == x1 && y == y1) return true;
        const int e2 = 2 * err;
        // Block diagonal corner-cutting through a lethal cell.
        if (e2 > -dy && e2 < dx) {
            if (!freeCell(x + sx, y) && !freeCell(x, y + sy)) return false;
        }
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

bool GridPlanner::lineOfSight(const Point2D& a, const Point2D& b) const {
    int ax, ay, bx, by;
    if (!worldToCell(a, ax, ay) || !worldToCell(b, bx, by)) return false;
    return losCells(ax, ay, bx, by);
}

int GridPlanner::jump(int x, int y, int dx, int dy, int gx, int gy) const {
    while (true) {
        const int nx = x + dx, ny = y + dy;
        if (!freeCell(nx, ny)) return -1;
        if (dx != 0 && dy != 0 && !freeCell(x + dx, y) && !freeCell(x, y + dy)) {
            return -1;  // no diagonal corner cut
        }
        if (nx == gx && ny == gy) return idx(nx, ny);

        if (dx != 0 && dy != 0) {
            if ((freeCell(nx - dx, ny + dy) && !freeCell(nx - dx, ny)) ||
                (freeCell(nx + dx, ny - dy) && !freeCell(nx, ny - dy))) {
                return idx(nx, ny);
            }
            if (jump(nx, ny, dx, 0, gx, gy) != -1) return idx(nx, ny);
            if (jump(nx, ny, 0, dy, gx, gy) != -1) return idx(nx, ny);
        } else if (dx != 0) {
            if ((freeCell(nx + dx, ny + 1) && !freeCell(nx, ny + 1)) ||
                (freeCell(nx + dx, ny - 1) && !freeCell(nx, ny - 1))) {
                return idx(nx, ny);
            }
        } else {
            if ((freeCell(nx + 1, ny + dy) && !freeCell(nx + 1, ny)) ||
                (freeCell(nx - 1, ny + dy) && !freeCell(nx - 1, ny))) {
                return idx(nx, ny);
            }
        }
        x = nx;
        y = ny;
    }
}

std::vector<std::pair<int, int>> GridPlanner::prunedDirs(int cur, int parent) const {
    std::vector<std::pair<int, int>> dirs;
    const int cx = cur % w_, cy = cur / w_;
    if (parent < 0) {
        static const int d8[8][2] = {{1, 0},  {-1, 0}, {0, 1},  {0, -1},
                                     {1, 1},  {1, -1}, {-1, 1}, {-1, -1}};
        for (const auto& d : d8) {
            if (d[0] != 0 && d[1] != 0 && !freeCell(cx + d[0], cy) &&
                !freeCell(cx, cy + d[1])) {
                continue;
            }
            dirs.emplace_back(d[0], d[1]);
        }
        return dirs;
    }
    const int px = parent % w_, py = parent / w_;
    const int dx = (cx == px) ? 0 : (cx > px ? 1 : -1);
    const int dy = (cy == py) ? 0 : (cy > py ? 1 : -1);
    if (dx != 0 && dy != 0) {
        if (freeCell(cx, cy + dy)) dirs.emplace_back(0, dy);
        if (freeCell(cx + dx, cy)) dirs.emplace_back(dx, 0);
        if ((freeCell(cx + dx, cy) || freeCell(cx, cy + dy)) &&
            freeCell(cx + dx, cy + dy)) {
            dirs.emplace_back(dx, dy);
        }
        if (!freeCell(cx - dx, cy) && freeCell(cx - dx, cy + dy)) dirs.emplace_back(-dx, dy);
        if (!freeCell(cx, cy - dy) && freeCell(cx + dx, cy - dy)) dirs.emplace_back(dx, -dy);
    } else if (dx != 0) {
        if (freeCell(cx + dx, cy)) dirs.emplace_back(dx, 0);
        if (!freeCell(cx, cy + 1) && freeCell(cx + dx, cy + 1)) dirs.emplace_back(dx, 1);
        if (!freeCell(cx, cy - 1) && freeCell(cx + dx, cy - 1)) dirs.emplace_back(dx, -1);
    } else {
        if (freeCell(cx, cy + dy)) dirs.emplace_back(0, dy);
        if (!freeCell(cx + 1, cy) && freeCell(cx + 1, cy + dy)) dirs.emplace_back(1, dy);
        if (!freeCell(cx - 1, cy) && freeCell(cx - 1, cy + dy)) dirs.emplace_back(-1, dy);
    }
    return dirs;
}

std::vector<std::pair<int, int>> GridPlanner::jpsCells(int sx, int sy, int gx,
                                                       int gy) const {
    const int n = w_ * h_;
    const int start = idx(sx, sy), goal = idx(gx, gy);
    std::vector<float> g(n, std::numeric_limits<float>::max());
    std::vector<int> parent(n, -1);
    std::vector<char> visited(n, 0);

    auto octile = [&](int a, int b) {
        const double ax = a % w_, ay = a / w_, bx = b % w_, by = b / w_;
        const double dx = std::abs(ax - bx), dy = std::abs(ay - by);
        return (std::max(dx, dy) + kSqrt2m1 * std::min(dx, dy)) * res_;
    };

    using QE = std::pair<float, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    g[start] = 0.0f;
    pq.push({static_cast<float>(octile(start, goal)), start});

    std::vector<std::pair<int, int>> result;
    while (!pq.empty()) {
        const int cur = pq.top().second;
        pq.pop();
        if (visited[cur]) continue;
        visited[cur] = 1;
        if (cur == goal) {
            for (int c = goal; c != -1; c = parent[c]) result.emplace_back(c % w_, c / w_);
            std::reverse(result.begin(), result.end());
            return result;
        }
        const int cx = cur % w_, cy = cur / w_;
        for (const auto& [dx, dy] : prunedDirs(cur, parent[cur])) {
            const int jp = jump(cx, cy, dx, dy, gx, gy);
            if (jp < 0) continue;
            const float ng = g[cur] + static_cast<float>(octile(cur, jp));
            if (ng < g[jp]) {
                g[jp] = ng;
                parent[jp] = cur;
                pq.push({ng + static_cast<float>(octile(jp, goal)), jp});
            }
        }
    }
    return {};  // unreachable
}

std::vector<Point2D> GridPlanner::smooth(const std::vector<Point2D>& pts,
                                         bool bias_clearance) const {
    if (pts.size() < 3) return pts;

    // Any-angle shortcut: keep the farthest still-visible vertex.
    std::vector<Point2D> sc;
    sc.push_back(pts.front());
    size_t i = 0;
    while (i + 1 < pts.size()) {
        size_t j = pts.size() - 1;
        for (; j > i + 1; --j) {
            if (lineOfSight(pts[i], pts[j])) break;
        }
        sc.push_back(pts[j]);
        i = j;
    }
    if (sc.size() < 3) return sc;

    // Without clearance bias the path stays piecewise-straight (any-angle
    // shortcut only). Inflated-grid LOS already guarantees >= inflation
    // standoff, so skipping the nudge costs no safety margin.
    if (!bias_clearance) return sc;

    // Clearance bias: nudge each interior vertex along the local normal toward
    // higher clearance while preserving line-of-sight to both neighbours.
    const int steps = std::max(2, static_cast<int>(std::ceil(inflation_ / res_)) + 2);
    for (size_t k = 1; k + 1 < sc.size(); ++k) {
        const Point2D a = sc[k - 1], b = sc[k + 1];
        double nx = -(b.y - a.y), ny = (b.x - a.x);
        const double len = std::hypot(nx, ny);
        if (len < 1e-9) continue;
        nx /= len;
        ny /= len;
        Point2D best = sc[k];
        float best_clear = clearanceAtWorld(sc[k]);
        for (int t = -steps; t <= steps; ++t) {
            if (t == 0) continue;
            const Point2D cand(sc[k].x + nx * t * res_, sc[k].y + ny * t * res_);
            if (!lineOfSight(sc[k - 1], cand) || !lineOfSight(cand, sc[k + 1])) continue;
            const float c = clearanceAtWorld(cand);
            if (c > best_clear) {
                best_clear = c;
                best = cand;
            }
        }
        sc[k] = best;
    }
    return sc;
}

std::vector<Point2D> GridPlanner::plan(const Point2D& from, const Point2D& to,
                                       double snap_radius_m, bool bias_clearance) const {
    if (lethal_.empty()) return {};
    int sx, sy, gx, gy;
    if (!worldToCell(from, sx, sy) || !worldToCell(to, gx, gy)) return {};
    if (!snapToFree(sx, sy, snap_radius_m) || !snapToFree(gx, gy, snap_radius_m)) {
        return {};
    }
    if (lineOfSight(from, to)) return {from, to};

    const std::vector<std::pair<int, int>> cells = jpsCells(sx, sy, gx, gy);
    if (cells.empty()) return {};

    std::vector<Point2D> poly;
    poly.reserve(cells.size() + 2);
    poly.push_back(from);
    for (const auto& [cx, cy] : cells) {
        const Point2D w = cellToWorld(cx, cy);
        if (std::hypot(w.x - poly.back().x, w.y - poly.back().y) > 1e-6) poly.push_back(w);
    }
    if (std::hypot(to.x - poly.back().x, to.y - poly.back().y) > 1e-6) poly.push_back(to);
    return smooth(poly, bias_clearance);
}

}  // namespace f2c_cpp
