#include "similarity_2d.hpp"

#include <gtest/gtest.h>

#include <cmath>

using f2c_cpp::estimateSimilarity2D;

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
