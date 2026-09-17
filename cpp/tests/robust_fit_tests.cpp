/**
 * @file robust_fit_tests.cpp
 * @brief IRLS + leverage-corrected studentised outlier test.
 *
 * PORTED from the parallel OCU's tests/robust_fit_tests.cpp
 * (pilot_control/scripts/F2C/cpp, branch `autonomy`, commit a287113),
 * converted from that copy's hand-rolled check()/printf harness to GoogleTest.
 */

#include "similarity_2d.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

using f2c_cpp::estimateSimilarity2D;
using f2c_cpp::fitSimilarityRobust;
using f2c_cpp::Similarity2D;

namespace {

constexpr double kPxPerM = 4.63;
constexpr double kTh = 0.7;

QPointF truth(QPointF m) {
    return QPointF(
        1200.0 + kPxPerM * (std::cos(kTh) * m.x() - std::sin(kTh) * m.y()),
        900.0 - kPxPerM * (std::sin(kTh) * m.x() + std::cos(kTh) * m.y()));
}

double headingOf(const Similarity2D& t) {
    const QPointF o = t.apply(QPointF(0, 0));
    const QPointF d = t.apply(QPointF(1, 0)) - o;
    return std::atan2(d.y(), d.x());
}

QVector<double> precisionFor(const QVector<double>& sigma) {
    QVector<double> w;
    for (double s : sigma) w.push_back(1.0 / (s * s));
    return w;
}

class RobustFit : public ::testing::Test {
  protected:
    void SetUp() override {
        pcd = {{-30, -20}, {35, -25}, {28, 31}, {-33, 27}, {2, -38}, {-12, 40}};
        for (const auto& m : pcd) sat.push_back(truth(m));
        sigma = QVector<double>(pcd.size(), 0.15);
        const auto clean = estimateSimilarity2D(pcd, sat);
        ASSERT_TRUE(clean.has_value());
        h_true = headingOf(clean->transform);
    }

    QVector<QPointF> pcd;
    QVector<QPointF> sat;
    QVector<double> sigma;
    double h_true = 0.0;
};

}  // namespace

// On clean data IRLS must be a NO-OP: nothing downweighted, nothing flagged,
// and the transform identical to the plain weighted fit.
TEST_F(RobustFit, IsANoOpOnCleanData) {
    const auto r = fitSimilarityRobust(pcd, sat, sigma);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->converged);

    for (double w : r->robust_weight) EXPECT_GE(w, 0.999) << "no pair downweighted";
    EXPECT_TRUE(r->outliers.isEmpty()) << "nothing flagged (no crying wolf)";

    const auto plain = estimateSimilarity2D(pcd, sat, precisionFor(sigma));
    ASSERT_TRUE(plain.has_value());
    EXPECT_NEAR(r->fit.transform.a00, plain->transform.a00, 1e-12);
    EXPECT_NEAR(r->fit.transform.tx, plain->transform.tx, 1e-9);

    double leverage_sum = 0.0;
    for (double v : r->leverage) leverage_sum += v;
    EXPECT_NEAR(leverage_sum, 2.0, 1e-9) << "two complex parameters";
}

// THE CASE THIS WAS BUILT FOR: the bad pair is the FAR one. The fit chases
// it, so its raw residual is suppressed and raw ranking blames an innocent
// near pair. Leverage correction has to unmask it.
TEST_F(RobustFit, StudentizedResidualUnmasksAHighLeverageBlunder) {
    const QVector<QPointF> p{{-3, 2}, {4, -3}, {2, 4}, {-4, -2}, {60, 55}};
    QVector<QPointF> q;
    for (const auto& m : p) q.push_back(truth(m));
    q[4] += QPointF(1.2 * kPxPerM, -0.9 * kPxPerM);  // 1.5 m off
    const QVector<double> sg(p.size(), 0.15);

    const auto r = fitSimilarityRobust(p, q, sg);
    ASSERT_TRUE(r.has_value());

    int raw_worst = 0, studentized_worst = 0;
    for (int i = 1; i < p.size(); ++i) {
        if (r->fit.residuals_m[i] > r->fit.residuals_m[raw_worst]) raw_worst = i;
        if (r->studentized[i] > r->studentized[studentized_worst]) {
            studentized_worst = i;
        }
    }
    EXPECT_NE(raw_worst, 4) << "raw residual is fooled by the far pair";
    EXPECT_EQ(studentized_worst, 4)
        << "leverage of the far pair " << r->leverage[4] << "; raw blames pair "
        << raw_worst + 1;

    // At h = 0.99 the fit passes through the point almost regardless of
    // whether it is right, so NO residual test can check it. The honest
    // answer is to say so, not to report an all-clear.
    EXPECT_TRUE(r->weakly_checked.contains(4)) << "reported as weakly checked";
    EXPECT_GT(r->min_detectable_m[4], 4.0 * r->min_detectable_m[0])
        << "detectability floor " << r->min_detectable_m[4] << " m vs "
        << r->min_detectable_m[0] << " m for pair 1";
}

// With a realistic spread the same blunder IS caught outright.
TEST_F(RobustFit, AWellSpreadSetCatchesTheSameBlunder) {
    const QVector<QPointF> p{{-30, -20}, {35, -25}, {28, 31}, {-33, 27}, {2, -38}};
    QVector<QPointF> q;
    for (const auto& m : p) q.push_back(truth(m));
    q[2] += QPointF(1.2 * kPxPerM, -0.9 * kPxPerM);  // the same 1.5 m
    const QVector<double> sg(p.size(), 0.15);

    const auto r = fitSimilarityRobust(p, q, sg);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->outliers.contains(2))
        << "h=" << r->leverage[2] << ", studentised " << r->studentized[2]
        << ", floor " << r->min_detectable_m[2] << " m";
    EXPECT_TRUE(r->weakly_checked.isEmpty());
}

// IRLS limits a blunder's pull without ever deleting it.
TEST_F(RobustFit, LimitsABlundersPullWithoutRemovingIt) {
    QVector<QPointF> q = sat;
    q[2] += QPointF(3.0 * kPxPerM, -2.0 * kPxPerM);  // ~3.6 m mispick

    const auto plain = estimateSimilarity2D(pcd, q, precisionFor(sigma));
    const auto r = fitSimilarityRobust(pcd, q, sigma);
    ASSERT_TRUE(plain.has_value());
    ASSERT_TRUE(r.has_value());

    const double e_plain = std::abs(headingOf(plain->transform) - h_true);
    const double e_irls = std::abs(headingOf(r->fit.transform) - h_true);
    EXPECT_LT(e_irls, 0.35 * e_plain)
        << "heading error: weighted " << e_plain * 180 / M_PI << " deg -> IRLS "
        << e_irls * 180 / M_PI << " deg";

    EXPECT_LT(r->robust_weight[2], 0.5) << "the blunder is downweighted";
    EXPECT_GT(r->robust_weight[2], 0.0) << "but never removed (keeps its spread)";

    int others_touched = 0;
    for (int i = 0; i < r->robust_weight.size(); ++i) {
        if (i != 2 && r->robust_weight[i] < 0.999) ++others_touched;
    }
    EXPECT_EQ(others_touched, 0) << "good pairs keep full weight";
    ASSERT_EQ(r->outliers.size(), 1);
    EXPECT_EQ(r->outliers[0], 2);
}

// False-positive rate on clean noisy data: the absolute test must not fire
// just because one pair happens to sit worst.
TEST_F(RobustFit, RarelyFlagsCleanNoisyData) {
    std::mt19937 rng(7);
    constexpr int kTrials = 4000;
    int flagged = 0;
    for (int t = 0; t < kTrials; ++t) {
        QVector<QPointF> q;
        for (int i = 0; i < pcd.size(); ++i) {
            std::normal_distribution<double> nz(0.0, sigma[i] * kPxPerM);
            q.push_back(truth(pcd[i]) + QPointF(nz(rng), nz(rng)));
        }
        const auto r = fitSimilarityRobust(pcd, q, sigma);
        if (r && !r->outliers.isEmpty()) ++flagged;
    }
    const double rate = double(flagged) / kTrials;
    EXPECT_LT(rate, 0.05) << "false-flag rate " << rate * 100 << "% of fits";
}

// Below 4 pairs the test abstains rather than guessing. Stage 6 allows a
// 3-pair GPS-seeded fit, so this is the path that minimum actually takes.
TEST_F(RobustFit, AbstainsBelowFourPairs) {
    const QVector<QPointF> p{pcd[0], pcd[1], pcd[2]};
    QVector<QPointF> q{sat[0], sat[1], sat[2]};
    q[2] += QPointF(5 * kPxPerM, 5 * kPxPerM);
    const QVector<double> sg(3, 0.15);

    const auto r = fitSimilarityRobust(p, q, sg);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->outliers.isEmpty());
}
