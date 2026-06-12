/**
 * @file obstacle_geometry_tests.cpp
 * @brief Unit tests for the boost-only obstacle/free-space geometry core.
 *
 * Locks the robustness contract: bad obstacle shapes (bow-ties, overlaps,
 * holes touching the shell) must NOT abort coverage generation, area is
 * conserved, ROI clipping is concave-safe, and clearance shrinks free space.
 */

#include <gtest/gtest.h>

#include <cmath>

#include "coverage_pipeline.hpp"

using f2c_cpp::Point2D;
using f2c_cpp::Polygon2D;
using f2c_cpp::Obstacle2D;
using f2c_cpp::buildFreeSpacePolygons;
using f2c_cpp::clipObstacleToPolygon;
using f2c_cpp::subtractPolygonFromObstacle;

namespace {

Polygon2D rect(double x0, double y0, double x1, double y1) {
    return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

double totalArea(const std::vector<Obstacle2D>& regions) {
    double a = 0.0;
    for (const auto& r : regions) {
        double s = 0.0;
        const auto& p = r.outer;
        for (size_t i = 0; i < p.size(); ++i) {
            const auto& q1 = p[i];
            const auto& q2 = p[(i + 1) % p.size()];
            s += q1.x * q2.y - q2.x * q1.y;
        }
        a += std::fabs(s / 2.0);
    }
    return a;
}

}  // namespace

TEST(FreeSpace, NoObstaclesKeepsFullArea) {
    Polygon2D boundary = rect(0, 0, 10, 10);
    auto r = buildFreeSpacePolygons(boundary, nullptr, nullptr, 0.0);
    ASSERT_TRUE(r.success) << r.error_message;
    EXPECT_NEAR(r.effective_area_m2, 100.0, 1e-6);
    EXPECT_EQ(r.skipped_obstacles, 0);
}

TEST(FreeSpace, SimpleObstacleSubtracted) {
    Polygon2D boundary = rect(0, 0, 10, 10);
    std::vector<Obstacle2D> obs = {{rect(2, 2, 4, 4), {}}};
    auto r = buildFreeSpacePolygons(boundary, nullptr, &obs, 0.0);
    ASSERT_TRUE(r.success) << r.error_message;
    EXPECT_NEAR(r.effective_area_m2, 96.0, 1e-4);
}

TEST(FreeSpace, SelfTouchingObstacleRepairedNotSkipped) {
    // Two triangles meeting at a single shared vertex (saddle/hourglass) with
    // NON-crossing edges. This is the real occupancy-grid contour case: the
    // ring self-touches at a diagonal cell. Must be repaired (area removed),
    // never skipped.
    Polygon2D boundary = rect(0, 0, 10, 10);
    Obstacle2D hourglass;
    hourglass.outer = {{2, 2}, {4, 2}, {3, 3}, {4, 4}, {2, 4}, {3, 3}};
    std::vector<Obstacle2D> obs = {hourglass};
    auto r = buildFreeSpacePolygons(boundary, nullptr, &obs, 0.0);
    ASSERT_TRUE(r.success) << r.error_message;
    EXPECT_EQ(r.skipped_obstacles, 0);
    EXPECT_LT(r.effective_area_m2, 100.0);  // ~2 m^2 of triangles removed
    EXPECT_GT(r.effective_area_m2, 95.0);
}

TEST(FreeSpace, CrossingBowtieNeverAborts) {
    // Pathological edge-crossing figure-8. Boost overlay can't re-node invalid
    // input, so this may be skipped — but it must NOT abort the whole build.
    // The detector is hardened upstream so it never emits crossing rings.
    Polygon2D boundary = rect(0, 0, 10, 10);
    Obstacle2D bowtie;
    bowtie.outer = {{2, 2}, {4, 4}, {4, 2}, {2, 4}};
    std::vector<Obstacle2D> obs = {bowtie};
    auto r = buildFreeSpacePolygons(boundary, nullptr, &obs, 0.0);
    EXPECT_TRUE(r.success) << r.error_message;  // no abort
}

TEST(FreeSpace, OverlappingObstaclesUnioned) {
    Polygon2D boundary = rect(0, 0, 10, 10);
    std::vector<Obstacle2D> obs = {
        {rect(2, 2, 5, 5), {}},
        {rect(4, 4, 7, 7), {}},
    };
    auto r = buildFreeSpacePolygons(boundary, nullptr, &obs, 0.0);
    ASSERT_TRUE(r.success) << r.error_message;
    // Union area = 9 + 9 - 1 (overlap) = 17 removed.
    EXPECT_NEAR(r.effective_area_m2, 100.0 - 17.0, 1e-3);
}

TEST(FreeSpace, ObstacleCoveringRoiRemovesAllArea) {
    Polygon2D boundary = rect(0, 0, 10, 10);
    std::vector<Obstacle2D> obs = {{rect(-1, -1, 11, 11), {}}};
    auto r = buildFreeSpacePolygons(boundary, nullptr, &obs, 0.0);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_message, "Obstacles removed all usable area");
}

TEST(FreeSpace, ClearanceShrinksFreeArea) {
    Polygon2D boundary = rect(0, 0, 10, 10);
    std::vector<Obstacle2D> obs = {{rect(4, 4, 6, 6), {}}};
    auto raw = buildFreeSpacePolygons(boundary, nullptr, &obs, 0.0);
    auto inflated = buildFreeSpacePolygons(boundary, nullptr, &obs, 0.5);
    ASSERT_TRUE(raw.success);
    ASSERT_TRUE(inflated.success);
    EXPECT_LT(inflated.effective_area_m2, raw.effective_area_m2);
}

TEST(FreeSpace, RoiIntersectsBoundary) {
    Polygon2D boundary = rect(0, 0, 10, 10);
    Polygon2D roi = rect(5, 5, 15, 15);
    auto r = buildFreeSpacePolygons(boundary, &roi, nullptr, 0.0);
    ASSERT_TRUE(r.success) << r.error_message;
    EXPECT_NEAR(r.effective_area_m2, 25.0, 1e-6);
}

TEST(FreeSpace, RoiDisjointFromBoundaryFails) {
    Polygon2D boundary = rect(0, 0, 10, 10);
    Polygon2D roi = rect(20, 20, 30, 30);
    auto r = buildFreeSpacePolygons(boundary, &roi, nullptr, 0.0);
    EXPECT_FALSE(r.success);
}

TEST(FreeSpace, DegenerateObstacleSkippedNotFatal) {
    Polygon2D boundary = rect(0, 0, 10, 10);
    Obstacle2D line;
    line.outer = {{1, 1}, {2, 2}};  // < 3 vertices
    std::vector<Obstacle2D> obs = {line, {rect(3, 3, 5, 5), {}}};
    auto r = buildFreeSpacePolygons(boundary, nullptr, &obs, 0.0);
    ASSERT_TRUE(r.success) << r.error_message;
    EXPECT_EQ(r.skipped_obstacles, 1);
    EXPECT_NEAR(r.effective_area_m2, 96.0, 1e-4);
}

TEST(Clip, ObstacleInsideClipUnchanged) {
    Obstacle2D obs{rect(2, 2, 4, 4), {}};
    Polygon2D clip = rect(0, 0, 10, 10);
    auto pieces = clipObstacleToPolygon(obs, clip);
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_NEAR(totalArea(pieces), 4.0, 1e-6);
}

TEST(Clip, ObstacleStraddlingClipTrimmed) {
    Obstacle2D obs{rect(8, 8, 12, 12), {}};  // half outside
    Polygon2D clip = rect(0, 0, 10, 10);
    auto pieces = clipObstacleToPolygon(obs, clip);
    ASSERT_FALSE(pieces.empty());
    EXPECT_NEAR(totalArea(pieces), 4.0, 1e-4);  // 2x2 inside corner
}

TEST(Clip, ObstacleFullyOutsideClipReturnsEmpty) {
    Obstacle2D obs{rect(20, 20, 25, 25), {}};
    Polygon2D clip = rect(0, 0, 10, 10);
    auto pieces = clipObstacleToPolygon(obs, clip);
    EXPECT_TRUE(pieces.empty());
}

TEST(Cut, EdgeCutShrinksObstacle) {
    Obstacle2D obs{rect(0, 0, 10, 10), {}};
    Polygon2D cutter = rect(8, -1, 11, 11);  // bites the right 2 m strip
    auto pieces = subtractPolygonFromObstacle(obs, cutter);
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_NEAR(totalArea(pieces), 80.0, 1e-4);
}

TEST(Cut, InteriorCutPunchesHole) {
    Obstacle2D obs{rect(0, 0, 10, 10), {}};
    Polygon2D cutter = rect(4, 4, 6, 6);  // fully interior
    auto pieces = subtractPolygonFromObstacle(obs, cutter);
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_EQ(pieces[0].holes.size(), 1u);
    EXPECT_NEAR(totalArea(pieces), 100.0, 1e-4);  // outer area unchanged
}

TEST(Cut, BisectingCutSplitsIntoTwo) {
    Obstacle2D obs{rect(0, 0, 10, 10), {}};
    Polygon2D cutter = rect(4, -1, 6, 11);  // vertical band through the middle
    auto pieces = subtractPolygonFromObstacle(obs, cutter);
    ASSERT_EQ(pieces.size(), 2u);
    EXPECT_NEAR(totalArea(pieces), 80.0, 1e-4);  // two 4x10 halves
}

TEST(Cut, CutterCoveringObstacleReturnsEmpty) {
    Obstacle2D obs{rect(2, 2, 4, 4), {}};
    Polygon2D cutter = rect(0, 0, 10, 10);
    auto pieces = subtractPolygonFromObstacle(obs, cutter);
    EXPECT_TRUE(pieces.empty());
}

TEST(Cut, DisjointCutterLeavesObstacleIntact) {
    Obstacle2D obs{rect(0, 0, 4, 4), {}};
    Polygon2D cutter = rect(10, 10, 12, 12);
    auto pieces = subtractPolygonFromObstacle(obs, cutter);
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_NEAR(totalArea(pieces), 16.0, 1e-4);
}

TEST(Clip, ConcaveClipSplitsObstacleIntoPieces) {
    // L-shaped clip: full 10x10 minus the top-right 5x5 quadrant.
    Polygon2D lshape = {{0, 0}, {10, 0}, {10, 5}, {5, 5}, {5, 10}, {0, 10}};
    // Horizontal bar crossing the notch -> splits into two pieces.
    Obstacle2D obs{rect(2, 4, 8, 6), {}};
    auto pieces = clipObstacleToPolygon(obs, lshape);
    ASSERT_GE(pieces.size(), 1u);
    // Inside-L portion: left 3x2 block (2..5 x 4..6) fully in, plus 5..8 x 4..5.
    EXPECT_GT(totalArea(pieces), 0.0);
    EXPECT_LT(totalArea(pieces), 12.0);  // < full bar area (6x2=12)
}
