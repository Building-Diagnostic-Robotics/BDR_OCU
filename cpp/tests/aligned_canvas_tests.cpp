/**
 * @file aligned_canvas_tests.cpp
 * @brief Frame plumbing for a satellite pane re-projected into the robot frame.
 *
 * Correspondences are stored in ORIGINAL satellite pixels. These tests cover
 * the plumbing that keeps that invariant true while the displayed projection
 * changes underneath the operator.
 *
 * PORTED from the parallel OCU's tests/aligned_canvas_tests.cpp
 * (pilot_control/scripts/F2C/cpp, branch `autonomy`, commit a287113),
 * converted from that copy's hand-rolled check()/printf harness to GoogleTest.
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

/// The PCD raster's own metres -> pixel mapping (worldToPcdImage's inverse).
QPointF pcdWorldToImage(const QRectF& b, const QSize& sz, QPointF m) {
    return QPointF(
        (m.x() - b.left()) / b.width() * std::max(1, sz.width() - 1),
        (b.bottom() - m.y()) / b.height() * std::max(1, sz.height() - 1));
}

Similarity2D fitFrom(const QVector<QPointF>& pcd, double k, double th) {
    QVector<QPointF> sat;
    for (const auto& m : pcd) {
        sat.push_back(
            QPointF(1200.0 + k * (std::cos(th) * m.x() - std::sin(th) * m.y()),
                    900.0 - k * (std::sin(th) * m.x() + std::cos(th) * m.y())));
    }
    return estimateSimilarity2D(pcd, sat)->transform;
}

class AlignedCanvasTest : public ::testing::Test {
  protected:
    void SetUp() override {
        pcd = {{-30, -20}, {35, -25}, {28, 31}, {-33, 27}, {2, -38}};
        fit = fitFrom(pcd, 4.63, 0.7);
        const AlignedCanvas c = alignedCanvasFor(fit, sat_size, kMaxDim);
        ASSERT_TRUE(c.valid);
        X = c.transform;
        mpp = c.metres_per_px;
    }

    /// Robot metres -> aligned canvas pixels.
    QPointF satPx(QPointF m) const { return X.map(fit.apply(m)); }

    QVector<QPointF> pcd;
    const QSize sat_size{1400, 900};
    // Sized as renderTopDownAlphaDensity does: proportional to the cloud's
    // aspect, capped at kPointCloudProjectionMaxDim. Square bounds -> square
    // raster; anything else would be a raster the renderer cannot produce.
    const QRectF pcd_bounds{QPointF(-52.5, -61.25), QPointF(47.5, 38.75)};
    const QSize pcd_size{4096, 4096};
    Similarity2D fit;
    QTransform X;
    double mpp = 0.0;
};

}  // namespace

// A pick in the projected view maps back to the exact original satellite
// pixel. This is the stored-pair invariant.
TEST_F(AlignedCanvasTest, OriginalToDisplayToOriginalIsIdentity) {
    bool ok = false;
    const QTransform Xinv = X.inverted(&ok);
    ASSERT_TRUE(ok) << "view transform inverts";

    double worst = 0.0;
    for (int i = 0; i <= 12; ++i) {
        for (int j = 0; j <= 12; ++j) {
            const QPointF orig(i * sat_size.width() / 12.0,
                               j * sat_size.height() / 12.0);
            const QPointF back = Xinv.map(X.map(orig));
            worst = std::max(worst,
                             std::hypot(back.x() - orig.x(), back.y() - orig.y()));
        }
    }
    EXPECT_LT(worst, 1e-9) << "worst round trip over 169 points: " << worst << " px";
}

// The hazard this guards: the projection CHANGES when a pair is added. A pair
// stored under the old projection must still resolve to the same original
// pixel — and so the same ground point — under the new one.
TEST_F(AlignedCanvasTest, StoredPairSurvivesAProjectionIncrement) {
    const Similarity2D fit2 = fitFrom(pcd, 4.70, 0.74);  // a later increment
    const AlignedCanvas c2 = alignedCanvasFor(fit2, sat_size, kMaxDim);
    ASSERT_TRUE(c2.valid);
    const QTransform X2 = c2.transform;

    const QPointF stored(830.0, 412.0);  // stored in ORIGINAL px
    const QPointF shown_old = X.map(stored);
    const QPointF shown_new = X2.map(stored);
    const QPointF recovered_old = X.inverted().map(shown_old);
    const QPointF recovered_new = X2.inverted().map(shown_new);

    EXPECT_LT(std::hypot(recovered_old.x() - stored.x(),
                         recovered_old.y() - stored.y()),
              1e-9);
    EXPECT_LT(std::hypot(recovered_new.x() - stored.x(),
                         recovered_new.y() - stored.y()),
              1e-9);
    EXPECT_GT(std::hypot(shown_new.x() - shown_old.x(),
                         shown_new.y() - shown_old.y()),
              1.0)
        << "and the projection really did move";
}

// The point of re-projecting: the panes share orientation. A vector between
// two ground points is the same bearing in the aligned satellite canvas as in
// the point cloud raster.
TEST_F(AlignedCanvasTest, AlignedPaneMatchesTheCloudPaneInOrientation) {
    const QPointF ma(-20.0, 10.0), mb(15.0, -5.0);
    const QPointF vs = satPx(mb) - satPx(ma);
    const QPointF vp = pcdWorldToImage(pcd_bounds, pcd_size, mb) -
                       pcdWorldToImage(pcd_bounds, pcd_size, ma);
    const double ang_s = std::atan2(vs.y(), vs.x());
    const double ang_p = std::atan2(vp.y(), vp.x());
    EXPECT_NEAR(ang_s, ang_p, 1e-9)
        << "sat canvas " << ang_s << " rad, pcd raster " << ang_p << " rad";
}

// Scale is independent by design: the canvas carries satellite detail, the
// raster carries cloud detail, and each pane zooms on its own.
TEST_F(AlignedCanvasTest, CanvasIsOnePixelPerSatellitePixel) {
    EXPECT_NEAR(mpp, 1.0 / fit.scalePxPerM(), 1e-12) << "no detail lost";
}

// Robot +X runs along canvas +u and +Y up the canvas — the cloud raster's
// convention, so neither pane is mirrored against the other.
TEST_F(AlignedCanvasTest, RobotAxesMapToTheCloudRasterConvention) {
    const QPointF o = satPx(QPointF(0, 0));
    const QPointF ex = satPx(QPointF(1, 0)) - o;
    const QPointF ey = satPx(QPointF(0, 1)) - o;

    EXPECT_GT(ex.x(), 0.99 * (1.0 / mpp)) << "robot +X maps to canvas +u";
    EXPECT_LT(std::abs(ex.y()), 1e-6);
    EXPECT_LT(ey.y(), -0.99 * (1.0 / mpp)) << "robot +Y maps to canvas -v (north up)";
    EXPECT_LT(std::abs(ey.x()), 1e-6);
}

// Canvas size tracks the image, not the cloud: a rotation of a 1400x900 image
// cannot exceed its diagonal on either side.
TEST_F(AlignedCanvasTest, CanvasIsBoundedByTheImageDiagonal) {
    const QRectF bb =
        X.map(QPolygonF(QRectF(0, 0, sat_size.width(), sat_size.height())))
            .boundingRect();
    const double diag = std::hypot(sat_size.width(), sat_size.height());
    EXPECT_LE(bb.width(), diag + 2) << "canvas " << bb.width() << " x " << bb.height()
                                    << " px, diagonal " << diag;
    EXPECT_LE(bb.height(), diag + 2);
}

// A far-flung fit must not blow the canvas budget.
//
// DIVERGES from upstream, which asserted this on the IMAGE's extent in canvas
// pixels — equivalent back when the canvas was that extent. This copy crops the
// canvas inside the turned footprint, so the image deliberately runs past the
// canvas edges and that measurement now reports the image, not the budget. The
// budget is the canvas, so that is what is asserted here.
TEST_F(AlignedCanvasTest, WideImageryCoarsensInsteadOfExploding) {
    const Similarity2D tiny = fitFrom(pcd, 0.35, 0.2);  // imagery covers km
    const AlignedCanvas c3 = alignedCanvasFor(tiny, QSize(4000, 3000), kMaxDim);
    ASSERT_TRUE(c3.valid);
    EXPECT_LE(std::max(c3.width, c3.height), kMaxDim)
        << "coarse case: " << c3.width << " x " << c3.height << " px at "
        << c3.metres_per_px << " m/px";
    // And the crop is what keeps it there: a 4000x3000 image turned 0.2 rad has
    // a 4516 px bounding box, so pre-crop this case had to be resampled to fit.
    EXPECT_NEAR(std::sqrt(std::abs(c3.transform.determinant())), 1.0, 1e-9)
        << "cap no longer binds, so canvas px still == original px";
}
