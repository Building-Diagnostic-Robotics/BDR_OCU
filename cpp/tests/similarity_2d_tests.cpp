/**
 * @file similarity_2d_tests.cpp
 * @brief Weighted Umeyama fit + leave-one-out outlier detection.
 *
 * The weighting and leave-one-out cases are PORTED from the parallel OCU's
 * tests/similarity_2d_tests.cpp (pilot_control/scripts/F2C/cpp, branch
 * `autonomy`, commit a287113), converted from that copy's hand-rolled
 * check()/printf harness to GoogleTest. The three cases at the bottom are
 * this repo's originals and predate the port.
 */

#include "similarity_2d.hpp"

#include <gtest/gtest.h>

#include <cmath>

using f2c_cpp::estimateSimilarity2D;
using f2c_cpp::leaveOneOutOutlier;
using f2c_cpp::SimilarityFit;

namespace {

/// Ground truth: robot XY (right-handed) -> image px (Y down). The Y flip is
/// what makes the correct transform orientation-reversing.
constexpr double kPxPerM = 4.63;   // z19 at lat 43.6
constexpr double kHeading = 0.7;   // rad

QPointF truth(const QPointF& m) {
    const double c = std::cos(kHeading), s = std::sin(kHeading);
    const double e = c * m.x() - s * m.y();
    const double n = s * m.x() + c * m.y();
    return QPointF(1200.0 + kPxPerM * e, 900.0 - kPxPerM * n);
}

double headingOf(const SimilarityFit& f) {
    const QPointF o = f.transform.apply(QPointF(0, 0));
    const QPointF d = f.transform.apply(QPointF(1, 0)) - o;
    return std::atan2(d.y(), d.x());
}

/// Five well-spread pairs and their exact satellite pixels.
class WeightedFit : public ::testing::Test {
  protected:
    void SetUp() override {
        pcd = {{-30, -20}, {35, -25}, {28, 31}, {-33, 27}, {2, -38}};
        for (const auto& p : pcd) sat.push_back(truth(p));
        exact = estimateSimilarity2D(pcd, sat);
        ASSERT_TRUE(exact.has_value());
    }

    QVector<QPointF> pcd;
    QVector<QPointF> sat;
    std::optional<SimilarityFit> exact;
};

}  // namespace

TEST_F(WeightedFit, RecoversTheTransformExactly) {
    EXPECT_TRUE(exact->transform.valid);
    EXPECT_LT(exact->rmse_m, 1e-9);
    EXPECT_TRUE(exact->transform.reflected) << "Y-down flip is a reflection";
    EXPECT_NEAR(exact->transform.scalePxPerM(), kPxPerM, 1e-9);
}

// Existing callers pass no weights and must see no change whatsoever.
TEST_F(WeightedFit, UniformWeightsReproduceTheUnweightedFit) {
    const QVector<double> uniform(pcd.size(), 1.0);
    const auto w1 = estimateSimilarity2D(pcd, sat, uniform);
    ASSERT_TRUE(w1.has_value());
    EXPECT_NEAR(w1->transform.a00, exact->transform.a00, 1e-12);
    EXPECT_NEAR(w1->transform.tx, exact->transform.tx, 1e-9);

    // Only ratios matter, so a common factor must not move the solve.
    const QVector<double> uniform7(pcd.size(), 7.0);
    const auto w7 = estimateSimilarity2D(pcd, sat, uniform7);
    ASSERT_TRUE(w7.has_value());
    EXPECT_NEAR(w7->transform.a00, exact->transform.a00, 1e-12);

    EXPECT_NEAR(exact->weighted_rmse_m, exact->rmse_m, 1e-12);
}

// Malformed weights fall back to uniform rather than failing the solve: a
// caller that has not built its weights yet keeps working.
TEST_F(WeightedFit, MalformedWeightsFallBackToUniform) {
    const auto negative =
        estimateSimilarity2D(pcd, sat, QVector<double>{1.0, -3.0, 1.0, 1.0, 1.0});
    ASSERT_TRUE(negative.has_value());
    EXPECT_NEAR(negative->transform.a00, exact->transform.a00, 1e-12);

    const auto wrong_length =
        estimateSimilarity2D(pcd, sat, QVector<double>{1.0, 2.0});
    ASSERT_TRUE(wrong_length.has_value());
    EXPECT_NEAR(wrong_length->transform.a00, exact->transform.a00, 1e-12);
}

// Pair 0 was picked zoomed out and is 2 m off. Down-weighting it has to pull
// the fit back toward truth — this is the whole point of the sigma model.
TEST_F(WeightedFit, PrecisionWeightingBeatsUnweighted) {
    QVector<QPointF> noisy = sat;
    noisy[0] += QPointF(2.0 * kPxPerM, 1.0 * kPxPerM);

    QVector<double> precision(pcd.size(), 1.0 / (0.10 * 0.10));  // good picks
    precision[0] = 1.0 / (1.50 * 1.50);                          // the sloppy one

    const auto unweighted = estimateSimilarity2D(pcd, noisy);
    const auto weighted = estimateSimilarity2D(pcd, noisy, precision);
    ASSERT_TRUE(unweighted.has_value());
    ASSERT_TRUE(weighted.has_value());

    const double h_true = headingOf(*exact);
    const double e_unweighted = std::abs(headingOf(*unweighted) - h_true);
    const double e_weighted = std::abs(headingOf(*weighted) - h_true);
    EXPECT_LT(e_weighted, e_unweighted)
        << "heading error: unweighted " << e_unweighted * 180.0 / M_PI
        << " deg, weighted " << e_weighted * 180.0 / M_PI << " deg";

    const double s_unweighted =
        std::abs(unweighted->transform.scalePxPerM() - kPxPerM);
    const double s_weighted =
        std::abs(weighted->transform.scalePxPerM() - kPxPerM);
    EXPECT_LT(s_weighted, s_unweighted);
}

// Residuals are what the operator is shown, so they stay in input order and
// stay unweighted: an imprecise pick's miss must not look smaller than it is.
TEST_F(WeightedFit, ResidualsAreInInputOrderAndFingerTheBadPair) {
    QVector<QPointF> noisy = sat;
    noisy[0] += QPointF(2.0 * kPxPerM, 1.0 * kPxPerM);
    const auto fit = estimateSimilarity2D(pcd, noisy);
    ASSERT_TRUE(fit.has_value());

    ASSERT_EQ(fit->residuals_m.size(), pcd.size());
    int worst = 0;
    for (int i = 1; i < fit->residuals_m.size(); ++i) {
        if (fit->residuals_m[i] > fit->residuals_m[worst]) worst = i;
    }
    EXPECT_EQ(worst, 0);
}

// Leave-one-out has to find the outlier even though the fit has been dragged
// toward it, which shrinks its own residual.
TEST_F(WeightedFit, LeaveOneOutIdentifiesAGrossOutlier) {
    QVector<QPointF> gross = sat;
    gross[3] += QPointF(9.0 * kPxPerM, -7.0 * kPxPerM);

    const auto report = leaveOneOutOutlier(pcd, gross);
    EXPECT_TRUE(report.valid) << "runs at 5 pairs";
    EXPECT_EQ(report.worst_index, 3);
    EXPECT_TRUE(report.flagged)
        << "full " << report.full_rmse_m << " m -> without #"
        << report.worst_index + 1 << " " << report.best_rmse_without_m << " m";
}

TEST_F(WeightedFit, LeaveOneOutDoesNotCryWolfOnACleanSet) {
    EXPECT_FALSE(leaveOneOutOutlier(pcd, sat).flagged);
}

// 3 pairs leaves 2 after dropping one, and 2 pairs fit a similarity exactly:
// the answer would be zero-residual and meaningless, so it abstains.
TEST_F(WeightedFit, LeaveOneOutRefusesBelowFourPairs) {
    const QVector<QPointF> p3{pcd[0], pcd[1], pcd[2]};
    const QVector<QPointF> s3{sat[0], sat[1], sat[2]};
    EXPECT_FALSE(leaveOneOutOutlier(p3, s3).valid);
}

TEST(Similarity2D, IdentityFromTwoPoints) {
    const QVector<QPointF> pcd{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}};
    const QVector<QPointF> sat{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}};
    const auto fit = estimateSimilarity2D(pcd, sat);
    ASSERT_TRUE(fit.has_value());
    EXPECT_NEAR(fit->rmse_m, 0.0, 1e-9);
    EXPECT_NEAR(fit->transform.scalePxPerM(), 1.0, 1e-9);
}

TEST(Similarity2D, ScaledRotated) {
    const QVector<QPointF> pcd{{0.0, 0.0}, {2.0, 0.0}, {0.0, 2.0}};
    const QVector<QPointF> sat{{0.0, 0.0}, {0.0, 10.0}, {-10.0, 0.0}};
    const auto fit = estimateSimilarity2D(pcd, sat);
    ASSERT_TRUE(fit.has_value());
    EXPECT_LT(fit->rmse_px, 1e-6);
    EXPECT_NEAR(fit->transform.scalePxPerM(), 5.0, 1e-6);
}

TEST(Similarity2D, RejectsDegenerate) {
    const QVector<QPointF> pcd{{1.0, 1.0}, {1.0, 1.0}};
    const QVector<QPointF> sat{{0.0, 0.0}, {4.0, 0.0}};
    EXPECT_FALSE(estimateSimilarity2D(pcd, sat).has_value());
}
