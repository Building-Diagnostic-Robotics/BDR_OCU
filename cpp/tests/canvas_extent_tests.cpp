/**
 * @file canvas_extent_tests.cpp
 * @brief Regression: the aligned canvas must size to the imagery, not the cloud.
 *
 * PORTED from the parallel OCU's tests/canvas_extent_tests.cpp
 * (pilot_control/scripts/F2C/cpp, branch `autonomy`, commit a287113),
 * converted from that copy's hand-rolled check()/printf harness to GoogleTest.
 *
 * The blow-up reproduced here was reported against the parallel OCU, whose
 * extent rule unioned the imagery footprint with the cloud's metric bounds.
 * This repo never shipped that rule — `oldRule` below is a transcription kept
 * only so the regression has something to fail against.
 */

#include "alignment_geometry.hpp"
#include "similarity_2d.hpp"

#include <gtest/gtest.h>

#include <QPolygonF>
#include <QRectF>
#include <QTransform>

#include <cmath>

using f2c_cpp::AlignedCanvas;
using f2c_cpp::alignedCanvasFor;
using f2c_cpp::estimateSimilarity2D;
using f2c_cpp::Similarity2D;

namespace {

constexpr int kMaxDim = 4096;

struct Canvas {
    int w = 0;
    int h = 0;
    double mpp = 0;
    QTransform x;
    bool ok = false;
};

/// The REMOVED rule: the image's footprint unioned with the point cloud's
/// metric bounds. Deliberately describes code that is not in production.
Canvas oldRule(const Similarity2D& fit, QSize sat, const QRectF& pcd) {
    Canvas c;
    const QTransform to_sat(fit.a00, fit.a10, fit.a01, fit.a11, fit.tx, fit.ty);
    bool inv = false;
    const QTransform to_m = to_sat.inverted(&inv);
    if (!inv) return c;
    const QRectF extent =
        to_m.map(QPolygonF(QRectF(0, 0, sat.width(), sat.height())))
            .boundingRect()
            .united(pcd);  // <-- the bug
    const double ppm = fit.scalePxPerM();
    if (!(ppm > 1e-9)) return c;
    c.mpp = 1.0 / ppm;
    c.w = int(std::ceil(extent.width() / c.mpp)) + 1;
    c.h = int(std::ceil(extent.height() / c.mpp)) + 1;
    const int longest = std::max(c.w, c.h);
    if (longest > kMaxDim) {
        c.mpp *= double(longest) / double(kMaxDim);
        c.w = int(std::ceil(extent.width() / c.mpp)) + 1;
        c.h = int(std::ceil(extent.height() / c.mpp)) + 1;
    }
    const QTransform to_px(1.0 / c.mpp, 0, 0, -1.0 / c.mpp,
                           -extent.left() / c.mpp, extent.bottom() / c.mpp);
    c.x = to_m * to_px;
    c.ok = true;
    return c;
}

/// Production, via the real function.
Canvas newRule(const Similarity2D& fit, QSize sat) {
    const AlignedCanvas a = alignedCanvasFor(fit, sat, kMaxDim);
    return Canvas{a.width, a.height, a.metres_per_px, a.transform, a.valid};
}

/// Fraction of the canvas the imagery actually occupies. fitToView shows the
/// whole canvas, so this is directly how small the operator's imagery gets.
double coverage(const Canvas& c, QSize sat) {
    const QRectF bb = c.x.map(QPolygonF(QRectF(0, 0, sat.width(), sat.height())))
                          .boundingRect();
    return (bb.width() * bb.height()) /
           std::max(1.0, double(c.w) * double(c.h));
}

const QSize kSat(1400, 900);
const QRectF kPcdBounds(QPointF(-52.5, -61.25), QPointF(47.5, 38.75));  // ~100 m

/// A two-pair fit where one pick was mismatched: the picks are much closer
/// together in the cloud than in the imagery, so the fitted scale comes out
/// far too high and the imagery's metric footprint collapses.
Similarity2D mismatchedFit() {
    const QVector<QPointF> pcd_pts{{0.0, 0.0}, {3.0, 2.0}};
    const QVector<QPointF> sat_pts{{300.0, 700.0}, {900.0, 260.0}};
    return estimateSimilarity2D(pcd_pts, sat_pts)->transform;
}

}  // namespace

TEST(CanvasExtent, UnioningWithTheCloudShrinksImageryToASpeck) {
    const Canvas old_way = oldRule(mismatchedFit(), kSat, kPcdBounds);
    ASSERT_TRUE(old_way.ok);
    EXPECT_LT(coverage(old_way, kSat), 0.02)
        << "the reported blow-up: " << old_way.w << " x " << old_way.h
        << " px, imagery covers " << coverage(old_way, kSat) * 100.0 << "%";
}

TEST(CanvasExtent, SizingToTheImageryKeepsItFillingTheCanvas) {
    const Similarity2D fit = mismatchedFit();
    const Canvas new_way = newRule(fit, kSat);
    ASSERT_TRUE(new_way.ok);
    EXPECT_GT(coverage(new_way, kSat), 0.45)
        << "mismatched 2-pair fit is " << fit.scalePxPerM()
        << " px/m (imagery is ~4.63); canvas " << new_way.w << " x " << new_way.h;
}

// With the cloud union gone the canvas is about the size of the image whatever
// the fit's scale does.
TEST(CanvasExtent, CanvasSizeIsScaleInvariant) {
    double lo = 1e18, hi = 0.0;
    for (double k : {0.2, 1.0, 4.63, 25.0, 200.0}) {
        const QVector<QPointF> p{{-30, -20}, {35, -25}, {28, 31}};
        QVector<QPointF> q;
        for (const auto& m : p) {
            q.push_back(QPointF(
                700 + k * (std::cos(0.6) * m.x() - std::sin(0.6) * m.y()),
                450 - k * (std::sin(0.6) * m.x() + std::cos(0.6) * m.y())));
        }
        const Canvas c = newRule(estimateSimilarity2D(p, q)->transform, kSat);
        lo = std::min(lo, double(std::max(c.w, c.h)));
        hi = std::max(hi, double(std::max(c.w, c.h)));
    }
    const double diag = std::hypot(kSat.width(), kSat.height());
    EXPECT_LE(hi, diag + 2) << "longest side across 0.2..200 px/m: " << lo << ".."
                            << hi << " (diagonal " << diag << ")";
    EXPECT_GT(lo, 0.5 * kSat.height());
}

TEST(CanvasExtent, DoesNotCoarsenTheImagery) {
    const Canvas new_way = newRule(mismatchedFit(), kSat);
    ASSERT_TRUE(new_way.ok);
    EXPECT_NEAR(std::sqrt(std::abs(new_way.x.determinant())), 1.0, 1e-9)
        << "canvas px == original px, so sigma units stay honest";
}
