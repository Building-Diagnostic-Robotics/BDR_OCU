/**
 * @file teleop_ramp_tests.cpp
 * @brief Post-launch WASD teleop rate limiter.
 */

#include "teleop_ramp.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace ramp = f2c_cpp::teleop_ramp;

namespace {

using Clock = ramp::Ramp::Clock;
constexpr auto kTick = std::chrono::milliseconds(ramp::kTickMs);
constexpr double kTickSeconds = ramp::kTickMs / 1000.0;

/** Tick until the ramp reaches the target; returns elapsed seconds. */
double runUntil(ramp::Ramp& r, Clock::time_point& t, double lin, double ang, double* max_jump = nullptr) {
    double elapsed = 0.0;
    for (int i = 0; i < 200; ++i) {
        const double prev_lin = r.linear();
        const double prev_ang = r.angular();
        t += kTick;
        r.advance(lin, ang, t);
        elapsed += kTickSeconds;
        if (max_jump) {
            *max_jump = std::max({*max_jump, std::fabs(r.linear() - prev_lin), std::fabs(r.angular() - prev_ang)});
        }
        if (r.linear() == lin && r.angular() == ang) {
            return elapsed;
        }
    }
    return -1.0;
}

}  // namespace

TEST(TeleopRamp, FirstAdvanceAfterResetStaysAtRest) {
    ramp::Ramp r;
    r.advance(ramp::kLinearSpeedMps, ramp::kAngularSpeedRps, Clock::now());
    EXPECT_TRUE(r.atRest());
}

TEST(TeleopRamp, AcceleratesGraduallyAndBrakesFaster) {
    ramp::Ramp r;
    auto t = Clock::now();
    r.advance(0.0, 0.0, t);
    const double up = runUntil(r, t, ramp::kLinearSpeedMps, 0.0);
    const double down = runUntil(r, t, 0.0, 0.0);
    EXPECT_NEAR(up, ramp::kLinearSpeedMps / ramp::kLinearAccel, kTickSeconds);
    EXPECT_NEAR(down, ramp::kLinearSpeedMps / ramp::kLinearDecel, kTickSeconds);
    EXPECT_LT(down, up);
    EXPECT_TRUE(r.atRest());
}

TEST(TeleopRamp, VelocityNeverSteps) {
    ramp::Ramp r;
    auto t = Clock::now();
    r.advance(0.0, 0.0, t);
    double max_jump = 0.0;
    runUntil(r, t, ramp::kLinearSpeedMps, ramp::kAngularSpeedRps, &max_jump);
    runUntil(r, t, -ramp::kLinearSpeedMps, -ramp::kAngularSpeedRps, &max_jump);
    runUntil(r, t, 0.0, 0.0, &max_jump);
    EXPECT_LE(max_jump, ramp::kAngularDecel * kTickSeconds + 1e-9);
}

TEST(TeleopRamp, ReversalPassesThroughZero) {
    ramp::Ramp r;
    auto t = Clock::now();
    r.advance(0.0, 0.0, t);
    runUntil(r, t, ramp::kLinearSpeedMps, 0.0);
    bool saw_zero = false;
    for (int i = 0; i < 100 && r.linear() > -ramp::kLinearSpeedMps; ++i) {
        t += kTick;
        r.advance(-ramp::kLinearSpeedMps, 0.0, t);
        saw_zero = saw_zero || r.linear() == 0.0;
    }
    EXPECT_TRUE(saw_zero);
    EXPECT_DOUBLE_EQ(r.linear(), -ramp::kLinearSpeedMps);
}

TEST(TeleopRamp, StalledTickIsBounded) {
    ramp::Ramp r;
    auto t = Clock::now();
    r.advance(0.0, 0.0, t);
    t += std::chrono::seconds(5);
    r.advance(ramp::kLinearSpeedMps, 0.0, t);
    EXPECT_NEAR(r.linear(), ramp::kLinearAccel * ramp::kMaxStepSeconds, 1e-9);
}

TEST(TeleopRamp, ResetStopsImmediately) {
    ramp::Ramp r;
    auto t = Clock::now();
    r.advance(0.0, 0.0, t);
    runUntil(r, t, ramp::kLinearSpeedMps, ramp::kAngularSpeedRps);
    r.reset();
    EXPECT_TRUE(r.atRest());
    r.advance(ramp::kLinearSpeedMps, 0.0, t + kTick);
    EXPECT_TRUE(r.atRest());
}
