/**
 * @file teleop_ramp.hpp
 * @brief Pure WASD teleop rate limiter — no Qt, no ROS.
 *
 * Exploration's post-launch FPV teleop turns held keys into a target twist
 * (the Satellite and Planner teleops are deliberately unchanged). Publishing that target
 * directly steps /cmd_vel from 0 to full speed on a key press, and back to 0
 * on release, where the robot's diff_drive_controller drops to zero torque
 * and the robot coasts. This ramps the published twist toward the target
 * instead, so velocity is continuous, and decelerates harder than it
 * accelerates so a release brakes to a stop rather than coasting.
 *
 * Matches pilot_control host_teleop's ramp (same rates, same reversal rule),
 * so a robot drives the same from either teleop.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace f2c_cpp {
namespace teleop_ramp {

/** Exploration FPV setpoints (0/9 still adjust the angular one at runtime). */
constexpr double kLinearSpeedMps = 0.65;
constexpr double kAngularSpeedRps = 2.0;

constexpr double kLinearAccel = 0.8;   ///< m/s^2   0 -> 0.65 m/s in ~0.8 s
constexpr double kLinearDecel = 1.6;   ///< m/s^2   0.65 m/s -> 0 in ~0.4 s
constexpr double kAngularAccel = 4.0;  ///< rad/s^2 0 -> 2.0 rad/s in 0.5 s
constexpr double kAngularDecel = 8.0;  ///< rad/s^2 2.0 rad/s -> 0 in 0.25 s

/** Publish period for the teleop timer that drives the ramp (10 Hz, unchanged). */
constexpr int kTickMs = 100;

/** A stalled UI thread must not turn into one large velocity step. */
constexpr double kMaxStepSeconds = 0.15;

/** Move `current` toward `target` by at most rate * dt. Shrinking the
 *  magnitude (stopping, or the first leg of a reversal) uses `decel`;
 *  growing it uses `accel`. A reversal stops at zero for one tick so the new
 *  direction starts on the accel rate. */
inline double step(double current, double target, double accel, double decel, double dt) {
    const bool slowing = (current > 0.0 && target < current) || (current < 0.0 && target > current);
    const double max_step = (slowing ? decel : accel) * dt;
    double next = target > current ? std::min(target, current + max_step)
                                   : std::max(target, current - max_step);
    if (slowing && current * next < 0.0) {
        next = 0.0;
    }
    return next;
}

class Ramp {
public:
    using Clock = std::chrono::steady_clock;

    /** Advance toward the target twist; returns the twist to publish. The
     *  first call after construction or reset() has no elapsed time and
     *  stays at rest. */
    void advance(double target_linear, double target_angular, Clock::time_point now) {
        double dt = 0.0;
        if (last_) {
            dt = std::clamp(std::chrono::duration<double>(now - *last_).count(), 0.0, kMaxStepSeconds);
        }
        last_ = now;
        linear_ = step(linear_, target_linear, kLinearAccel, kLinearDecel, dt);
        angular_ = step(angular_, target_angular, kAngularAccel, kAngularDecel, dt);
    }

    /** Disarm, E-Stop and losing teleop ownership stop at once: the next
     *  command ramps up from rest. */
    void reset() {
        linear_ = 0.0;
        angular_ = 0.0;
        last_.reset();
    }

    double linear() const { return linear_; }
    double angular() const { return angular_; }
    bool atRest() const { return linear_ == 0.0 && angular_ == 0.0; }

private:
    double linear_ = 0.0;
    double angular_ = 0.0;
    std::optional<Clock::time_point> last_;
};

}  // namespace teleop_ramp
}  // namespace f2c_cpp
