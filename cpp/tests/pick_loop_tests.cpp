/**
 * @file pick_loop_tests.cpp
 * @brief End-to-end simulation of the correspondence pick loop.
 *
 * The operator clicks a feature in whatever canvas is currently displayed,
 * production maps it back to ORIGINAL satellite pixels, the fit is recomputed
 * and the canvas re-warps. With exact picks the residual must stay at zero at
 * every increment.
 *
 * PORTED from the parallel OCU's tests/pick_loop_tests.cpp
 * (pilot_control/scripts/F2C/cpp, branch `autonomy`, commit a287113),
 * converted from that copy's hand-rolled check()/printf harness to GoogleTest.
 */

#include "alignment_geometry.hpp"
#include "similarity_2d.hpp"

#include <gtest/gtest.h>

#include <QRectF>
#include <QTransform>

#include <cmath>

using f2c_cpp::AlignedCanvas;
using f2c_cpp::alignedCanvasFor;
using f2c_cpp::estimateSimilarity2D;

namespace {

constexpr int kMaxDim = 4096;
constexpr double kPxPerM = 4.63;
constexpr double kTh = 0.7;

/// The true metres -> ORIGINAL satellite pixel mapping (reflected, as the
/// frames require).
QPointF truth(QPointF m) {
    return QPointF(
        1200.0 + kPxPerM * (std::cos(kTh) * m.x() - std::sin(kTh) * m.y()),
        900.0 - kPxPerM * (std::sin(kTh) * m.x() + std::cos(kTh) * m.y()));
}

const QVector<QPointF> kFeatures{{-30, -20}, {35, -25}, {28, 31}, {-33, 27},
                                 {2, -38},   {-12, 40}, {40, 5}};
const QSize kSatSize(1400, 900);

}  // namespace

// Exact picks: the stored pair is recovered from the displayed click without
// drift, so RMSE stays at zero however often the canvas re-warps.
TEST(PickLoop, RmseStaysAtZeroAcrossRewarpIncrements) {
    QVector<QPointF> stored_sat;   // Correspondence::sat_px (ORIGINAL px)
    QVector<QPointF> stored_pcd;   // Correspondence::pcd_m
    QTransform sat_view_xform;     // identity until a fit exists

    double worst_rmse = 0.0;
    for (int i = 0; i < kFeatures.size(); ++i) {
        // The operator sees the feature at its position in the CURRENT canvas
        // and clicks there; onSatellitePicked maps it back to original pixels.
        const QPointF display_pt = sat_view_xform.map(truth(kFeatures[i]));
        bool ok = false;
        const QTransform inv = sat_view_xform.inverted(&ok);
        const QPointF recovered = ok ? inv.map(display_pt) : display_pt;
        stored_sat.push_back(recovered);
        stored_pcd.push_back(kFeatures[i]);

        EXPECT_LT(std::hypot(recovered.x() - truth(kFeatures[i]).x(),
                             recovered.y() - truth(kFeatures[i]).y()),
                  1e-6)
            << "pair " << i + 1 << " drifted from truth";

        if (stored_sat.size() < 2) continue;
        const auto fit = estimateSimilarity2D(stored_pcd, stored_sat);
        ASSERT_TRUE(fit.has_value()) << "no fit at pair " << i + 1;
        worst_rmse = std::max(worst_rmse, fit->rmse_m);

        // updateSatelliteAlignmentView re-warps on the new fit.
        const AlignedCanvas c = alignedCanvasFor(fit->transform, kSatSize, kMaxDim);
        if (c.valid) sat_view_xform = c.transform;
    }
    EXPECT_LT(worst_rmse, 1e-6) << "worst RMSE " << worst_rmse << " m";
}

// The same loop with the click quantised to integer screen pixels through the
// widget's screenToImage at a plausible zoom — the real source of pick noise.
// It must not compound as the canvas re-warps under the operator.
TEST(PickLoop, ClickQuantisationDoesNotCompound) {
    QVector<QPointF> stored_sat;
    QVector<QPointF> stored_pcd;
    QTransform sat_view_xform;
    constexpr double kWidgetScale = 0.75;  // screen px per canvas px

    double worst = 0.0;
    for (int i = 0; i < kFeatures.size(); ++i) {
        const QPointF disp = sat_view_xform.map(truth(kFeatures[i]));
        // imageToScreen -> integer mouse position -> screenToImage
        const QPointF screen(std::round(disp.x() * kWidgetScale),
                             std::round(disp.y() * kWidgetScale));
        const QPointF back(screen.x() / kWidgetScale, screen.y() / kWidgetScale);
        bool ok = false;
        const QTransform inv = sat_view_xform.inverted(&ok);
        stored_sat.push_back(ok ? inv.map(back) : back);
        stored_pcd.push_back(kFeatures[i]);

        if (stored_sat.size() < 2) continue;
        const auto fit = estimateSimilarity2D(stored_pcd, stored_sat);
        ASSERT_TRUE(fit.has_value());
        worst = std::max(worst, fit->rmse_m);
        const AlignedCanvas c = alignedCanvasFor(fit->transform, kSatSize, kMaxDim);
        if (c.valid) sat_view_xform = c.transform;
    }
    EXPECT_LT(worst, 0.15) << "worst RMSE with quantisation " << worst << " m";
}
