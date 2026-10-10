/**
 * @file swath_preview_tests.cpp
 * @brief The Scan Parameters preview follows the coverage director: robot
 *        frame, edge clearances, and the minimum piece length.
 */

#include "swath_preview.hpp"

#include "satellite_geo_math.hpp"

#include <gtest/gtest.h>

#include <cmath>

using f2c_cpp::SwathPreview;
using f2c_cpp::SwathPreviewRequest;
using f2c_cpp::previewEdgeClearanceM;
using f2c_cpp::previewMinFragmentM;
using f2c_cpp::previewSwaths;

namespace {

SwathPreviewRequest eastNorthRectangle(double forward_m, double left_m,
                                       double heading_deg, double width_m) {
    // Body rectangle [0, forward] x [0, left], expressed in ENU at heading.
    SwathPreviewRequest request;
    request.marker_valid = true;
    request.heading_deg = heading_deg;
    request.width_m = width_m;
    const QVector<QPointF> body{{0, 0},
                                {forward_m, 0},
                                {forward_m, left_m},
                                {0, left_m}};
    for (const QPointF& point : body) {
        request.ring_enu.append(f2c_cpp::geo::enuFromBody(point, heading_deg));
    }
    return request;
}

double distanceToSegment(const QPointF& point, const QPointF& a,
                         const QPointF& b) {
    const QPointF ab = b - a;
    const double len_sq = QPointF::dotProduct(ab, ab);
    if (len_sq < 1e-12) {
        return QLineF(point, a).length();
    }
    const double u = qBound(0.0, QPointF::dotProduct(point - a, ab) / len_sq, 1.0);
    return QLineF(point, a + ab * u).length();
}

}  // namespace

TEST(SwathPreview, ClearancesMatchTheDirectorDefaults) {
    EXPECT_NEAR(previewEdgeClearanceM(false), 0.55, 1e-12);
    EXPECT_NEAR(previewEdgeClearanceM(true), 1.25, 1e-12);
    EXPECT_NEAR(previewMinFragmentM(1.0), 0.70, 1e-12);
    EXPECT_NEAR(previewMinFragmentM(0.4), 0.45, 1e-12);
}

TEST(SwathPreview, LanesFollowTheLongerBodyAxisAndStayClearOfEdges) {
    // 20 m forward, 10 m left, heading 0. The longer axis is forward, so
    // the lanes run along it: constant left, half a width in, step one width.
    const SwathPreview preview = previewSwaths(eastNorthRectangle(20, 10, 0, 2));
    ASSERT_EQ(preview.passes, 5);
    ASSERT_EQ(preview.segments.size(), 5);
    for (int i = 0; i < preview.segments.size(); ++i) {
        const QLineF& line = preview.segments[i];
        EXPECT_NEAR(line.x1(), -(1.0 + 2.0 * i), 1e-4);
        EXPECT_NEAR(line.x2(), line.x1(), 1e-4);
        EXPECT_NEAR(std::min(line.y1(), line.y2()), 0.55, 1e-3);
        EXPECT_NEAR(std::max(line.y1(), line.y2()), 19.45, 1e-3);
    }
}

TEST(SwathPreview, HeadingUsesTheRobotFrameNotTheLongEdge) {
    const SwathPreview aligned =
        previewSwaths(eastNorthRectangle(30, 6, 40.0, 2.0));
    ASSERT_FALSE(aligned.segments.isEmpty());
    const QPointF forward = f2c_cpp::geo::enuFromBody(QPointF(1, 0), 40.0);
    for (const QLineF& line : aligned.segments) {
        const QPointF dir = (line.p2() - line.p1()) / line.length();
        EXPECT_NEAR(std::abs(QPointF::dotProduct(dir, forward)), 1.0, 1e-3);
    }

    // Same roof, robot facing north. The long edge is 40° off the robot
    // axes, so the lanes must not follow it.
    SwathPreviewRequest turned = eastNorthRectangle(30, 6, 40.0, 2.0);
    turned.heading_deg = 0.0;
    const SwathPreview preview = previewSwaths(turned);
    ASSERT_FALSE(preview.segments.isEmpty());
    const QPointF north(0, 1);
    for (const QLineF& line : preview.segments) {
        const QPointF dir = (line.p2() - line.p1()) / line.length();
        EXPECT_NEAR(std::abs(QPointF::dotProduct(dir, north)), 1.0, 1e-3);
        EXPECT_LT(std::abs(QPointF::dotProduct(dir, forward)), 0.95);
    }
}

TEST(SwathPreview, MarkedEdgeTrimsFurtherThanAnUnmarkedEdge) {
    SwathPreviewRequest plain = eastNorthRectangle(20, 10, 0, 2);
    SwathPreviewRequest marked = plain;
    marked.roof_edges = {true, false, false, false};
    const SwathPreview open = previewSwaths(plain);
    const SwathPreview roof = previewSwaths(marked);
    EXPECT_EQ(open.passes, 5);
    EXPECT_EQ(roof.passes, 4);
    for (const QLineF& line : roof.segments) {
        EXPECT_GT(std::abs(line.x1()), 1.25 - 1e-3);
    }
}

TEST(SwathPreview, ShortLeftoverPieceIsDropped) {
    // Left prong is 1.2 m wide. Both walls take 0.55 m, leaving 0.1 m,
    // under the 0.7 m minimum. The right prong survives.
    SwathPreviewRequest request;
    request.marker_valid = true;
    request.heading_deg = 0.0;
    request.width_m = 2.0;
    const QVector<QPointF> body{{0, 0},   {1.2, 0}, {1.2, 2}, {6, 2},
                                {6, 0},   {10, 0},  {10, 8},  {0, 8}};
    for (const QPointF& point : body) {
        request.ring_enu.append(f2c_cpp::geo::enuFromBody(point, 0.0));
    }
    const SwathPreview preview = previewSwaths(request);
    ASSERT_GE(preview.passes, 1);
    for (const QLineF& line : preview.segments) {
        EXPECT_GE(line.length(), previewMinFragmentM(2.0) - 1e-3);
    }
    int on_first_lane = 0;
    for (const QLineF& line : preview.segments) {
        // Body left = 1 maps to ENU east = -1 at heading 0.
        if (std::abs(line.x1() + 1.0) < 1e-2 &&
            std::abs(line.x2() + 1.0) < 1e-2) {
            ++on_first_lane;
        }
    }
    EXPECT_EQ(on_first_lane, 1);
}

TEST(SwathPreview, ConcaveLaneSplitsIntoTwoPieces) {
    SwathPreviewRequest request;
    request.marker_valid = true;
    request.heading_deg = 0.0;
    request.width_m = 2.0;
    const QVector<QPointF> body{{0, 0}, {3, 0},  {3, 2}, {6, 2},
                                {6, 0}, {10, 0}, {10, 8}, {0, 8}};
    for (const QPointF& point : body) {
        request.ring_enu.append(f2c_cpp::geo::enuFromBody(point, 0.0));
    }
    const SwathPreview preview = previewSwaths(request);
    EXPECT_GT(preview.segments.size(), preview.passes);
}

TEST(SwathPreview, DegenerateInputsAreEmpty) {
    SwathPreviewRequest request;
    request.marker_valid = false;
    request.width_m = 1.0;
    request.ring_enu = {{0, 0}, {4, 0}, {4, 3}};
    EXPECT_TRUE(previewSwaths(request).segments.isEmpty());

    request.marker_valid = true;
    request.ring_enu = {{0, 0}, {1, 0}};
    EXPECT_TRUE(previewSwaths(request).segments.isEmpty());
    request.ring_enu = {{0, 0}, {4, 0}, {4, 3}, {0, 3}};
    request.width_m = 0.0;
    EXPECT_TRUE(previewSwaths(request).segments.isEmpty());
    request.width_m = -1.0;
    EXPECT_TRUE(previewSwaths(request).segments.isEmpty());
}

TEST(SwathPreview, SegmentsStayClearOfEveryEdge) {
    const SwathPreviewRequest request = eastNorthRectangle(20, 10, 0, 2);
    const SwathPreview preview = previewSwaths(request);
    ASSERT_FALSE(preview.segments.isEmpty());
    for (const QLineF& line : preview.segments) {
        const QVector<QPointF> samples{line.p1(), line.p2(),
                                       (line.p1() + line.p2()) / 2.0};
        for (const QPointF& sample : samples) {
            double nearest = 1e9;
            const int n = request.ring_enu.size();
            for (int i = 0; i < n; ++i) {
                nearest = std::min(
                    nearest, distanceToSegment(sample, request.ring_enu[i],
                                               request.ring_enu[(i + 1) % n]));
            }
            EXPECT_GE(nearest, previewEdgeClearanceM(false) - 1e-3);
        }
    }
}
