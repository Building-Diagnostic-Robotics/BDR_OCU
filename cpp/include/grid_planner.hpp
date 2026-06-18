/**
 * @file grid_planner.hpp
 * @brief Deterministic 2D global planner on an inflated occupancy grid.
 *
 * Built once per Generate from the (global) obstacle set, then queried many
 * times for inter-swath connectors and the robot->start approach. Replaces the
 * O(n^2) visibility-graph router that blew up on cluttered maps.
 *
 * Pipeline: rasterize obstacle polygons -> exact Euclidean distance transform
 * (clearance per cell) -> binary lethal grid (clearance < inflation) -> JPS
 * (canonical Jump Point Search, 8-connected, uniform cost) -> any-angle
 * shortcut smoothing biased toward higher-clearance cells. Clearance is
 * enforced by inflation (not a search weight), keeping JPS uniform-cost-correct.
 */

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "coverage_pipeline.hpp"  // Point2D, Obstacle2D, Polygon2D

namespace f2c_cpp {

class GridPlanner {
public:
    // Rasterize @p obstacles into [min,max] at @p resolution (m/cell), inflate
    // lethal cells by @p inflation_radius (m). Returns false on degenerate
    // bounds. Auto-coarsens resolution if the grid would exceed kMaxCells.
    bool build(const std::vector<Obstacle2D>& obstacles, double min_x, double min_y,
               double max_x, double max_y, double resolution, double inflation_radius);

    bool valid() const { return !lethal_.empty(); }

    // Collision-free polyline from @p from to @p to (endpoints inclusive,
    // snapped to the nearest free cell within @p snap_radius_m). Empty when
    // unreachable or an endpoint cannot be snapped. When @p bias_clearance is
    // true the smoother nudges interior vertices toward higher clearance
    // (centered, slightly curved routes); when false it keeps the any-angle
    // shortcut result (piecewise-straight legs). Safety is identical either
    // way — both stay >= inflation from obstacles via the inflated grid.
    std::vector<Point2D> plan(const Point2D& from, const Point2D& to,
                              double snap_radius_m, bool bias_clearance = true) const;

    // True when the straight segment a->b stays entirely in free space.
    bool lineOfSight(const Point2D& a, const Point2D& b) const;

private:
    static constexpr int kMaxCells = 6'000'000;

    int idx(int cx, int cy) const { return cy * w_ + cx; }
    bool inBounds(int cx, int cy) const { return cx >= 0 && cy >= 0 && cx < w_ && cy < h_; }
    bool freeCell(int cx, int cy) const { return inBounds(cx, cy) && lethal_[idx(cx, cy)] == 0; }
    bool worldToCell(const Point2D& p, int& cx, int& cy) const;
    Point2D cellToWorld(int cx, int cy) const;
    float clearanceAt(int cx, int cy) const;
    float clearanceAtWorld(const Point2D& p) const;
    bool snapToFree(int& cx, int& cy, double radius_m) const;
    bool losCells(int x0, int y0, int x1, int y1) const;

    void computeEdt(const std::vector<uint8_t>& occupied);
    int jump(int x, int y, int dx, int dy, int gx, int gy) const;
    std::vector<std::pair<int, int>> prunedDirs(int cur, int parent) const;
    std::vector<std::pair<int, int>> jpsCells(int sx, int sy, int gx, int gy) const;
    std::vector<Point2D> smooth(const std::vector<Point2D>& pts, bool bias_clearance) const;

    int w_ = 0, h_ = 0;
    double res_ = 0.1;
    double ox_ = 0.0, oy_ = 0.0;  // world coords of cell (0,0) lower-left corner
    double inflation_ = 0.0;
    std::vector<uint8_t> lethal_;   // 1 = occupied or within inflation
    std::vector<float> clearance_;  // metres to nearest occupied cell
};

}  // namespace f2c_cpp
