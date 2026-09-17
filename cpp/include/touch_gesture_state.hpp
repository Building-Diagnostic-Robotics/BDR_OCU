/**
 * @file touch_gesture_state.hpp
 * @brief Pure touchscreen pan / pinch bookkeeping — no Qt, no ROS.
 *
 * Shared by the three canvases that own a zoom/pan transform
 * (SatelliteMapWidget, PlotWidget, PanZoomImageWidget). It holds only the
 * gesture's state; each widget keeps its own coordinate maths and zoom
 * limits, so there stays exactly one definition of each.
 *
 * Two finger-count regimes:
 *  - One finger pans. A finger that never travels past `kTapSlopPx` is
 *    reported as a tap, but NO canvas turns that into a click: placing a
 *    polygon corner, toggling a roof edge and picking an alignment
 *    correspondence are all precision acts, and a fingertip that lands
 *    ~10 px from where the operator meant is worse than useless there —
 *    a stray vertex is visible, a flipped fall-hazard flag is not. Those
 *    stay trackpad / mouse work; a finger only ever moves the view. The
 *    tap is still reported because two screen-level actions key off it
 *    (committing the inline dimension editor, and handing control back
 *    from manual override on the scan step), neither of which cares where
 *    on the canvas it landed.
 *  - Two fingers pinch, pan and twist at once — they are independent
 *    measurements of the same pair (separation, midpoint, angle), so a
 *    widget does not have to choose between them. `takeLevelSteps` serves a
 *    discrete tile pyramid and `takeScaleFactor` a continuous scale; both
 *    drain the same accumulator, so a widget picks one and ignores the
 *    other.
 */

#pragma once

#include <algorithm>
#include <cmath>

namespace f2c_cpp {
namespace touch_gestures {

/** A touch position in widget pixels. */
struct Point {
    double x = 0.0;
    double y = 0.0;
};

enum class Phase { Idle, Single, Pinch };

class GestureState {
public:
    /** A finger that never travels this far is a tap, not a pan. Sized for
        a finger on a 307 mm direct-touch panel, where a press the operator
        means to hold still still wanders several pixels. */
    static constexpr double kTapSlopPx = 12.0;
    /** One zoom level per sqrt(2) of finger separation. Stepping per
        doubling is the honest mapping for an integer tile pyramid, but it
        needs roughly 5 cm -> 10 cm of travel per level and reads as a dead
        gesture in between. */
    static constexpr double kPinchStepLog2 = 0.5;
    /** Below this a separation ratio carries no information: two fingertips
        almost touching report a handful of pixels apart and jitter hard. */
    static constexpr double kMinSeparationPx = 16.0;
    /**
     * Accumulated finger-pair rotation before a twist engages, degrees.
     *
     * Two fingers report separation, midpoint and angle at once, so without
     * a dead zone every pinch and every two-finger pan would impart a few
     * degrees and the map would creep off north over a session. Past the
     * threshold the whole accumulated angle is released, so the gesture does
     * not lose the travel that armed it.
     */
    static constexpr double kTwistDeadZoneDeg = 12.0;
    /** Rotation is only trustworthy with the fingers well apart: near the
        separation floor a millimetre of jitter is tens of degrees. */
    static constexpr double kMinTwistSeparationPx = 60.0;

    struct Motion {
        bool pan = false;         // one finger, past the slop circle
        Point pan_delta{};
        bool pinch = false;       // two fingers down
        Point pinch_center{};     // live midpoint, widget pixels
        Point pinch_pan_delta{};  // midpoint travel since the last update
        /** Finger-pair rotation since the last update, degrees clockwise.
            Zero until the dead zone is cleared. */
        double twist_deg = 0.0;
    };

    Phase phase() const noexcept { return phase_; }

    /**
     * Feeds the currently-pressed points, ordered by touch id so index 0
     * and 1 keep their slots across events. Fingers past the second are
     * ignored: they carry nothing a pinch needs, and letting them re-seed
     * the reference separation makes the zoom jump.
     */
    Motion update(const Point* points, int count);

    /** Call when the last finger lifts. Returns true if the gesture never
        left the slop circle. Deliberately reports no position: a tap places
        nothing on any canvas, so a consumer that wanted one would be
        reintroducing the fingertip-accuracy problem this avoids. Resets. */
    bool release();

    /** Call on a cancelled sequence: the gesture is void, so unlike
        release() this reports no tap. */
    void reset() noexcept;

    /**
     * Consumes whole zoom levels, for a discrete (tile pyramid) canvas.
     * A positive `max_magnitude` caps one call and banks the remainder, so
     * a dropped frame cannot fling several levels at once.
     *
     * Subtracting exactly what it returns is also the hysteresis: after a
     * step the accumulator sits near zero, so the next step in EITHER
     * direction needs a further `kPinchStepLog2` and a finger trembling on
     * a level boundary cannot oscillate.
     */
    int takeLevelSteps(int max_magnitude = 0) noexcept;

    /** Consumes the pending zoom as a multiplicative factor, for a
        continuous-scale canvas. */
    double takeScaleFactor() noexcept;

private:
    /** Shortest signed difference between two finger-pair angles, radians.
        atan2 wraps at +-pi, so a pair crossing that boundary would otherwise
        report a ~360 deg jump. */
    static double angleDelta(double to, double from) noexcept;

    Phase phase_ = Phase::Idle;
    Point press_{};
    Point last_{};
    Point last_mid_{};
    bool past_slop_ = false;
    double ref_separation_ = 0.0;
    double pending_log2_ = 0.0;
    double last_angle_ = 0.0;
    double pending_twist_deg_ = 0.0;
    bool twist_engaged_ = false;
};

inline GestureState::Motion GestureState::update(const Point* points,
                                                 int count) {
    Motion motion;
    if (points == nullptr || count <= 0) {
        return motion;
    }

    if (count == 1) {
        const Point p = points[0];
        if (phase_ != Phase::Single) {
            // Either a fresh press, or a pinch that lost a finger. In the
            // second case the survivor must not pan by the gap it inherited
            // from the midpoint, and the gesture can no longer be a tap.
            const bool from_pinch = phase_ == Phase::Pinch;
            phase_ = Phase::Single;
            last_ = p;
            if (from_pinch) {
                past_slop_ = true;
            } else {
                press_ = p;
                past_slop_ = false;
            }
            return motion;
        }
        const Point delta{p.x - last_.x, p.y - last_.y};
        last_ = p;
        if (!past_slop_ &&
            std::hypot(p.x - press_.x, p.y - press_.y) > kTapSlopPx) {
            past_slop_ = true;
        }
        if (past_slop_) {
            motion.pan = true;
            motion.pan_delta = delta;
        }
        return motion;
    }

    const Point a = points[0];
    const Point b = points[1];
    const double separation = std::hypot(b.x - a.x, b.y - a.y);
    const Point mid{0.5 * (a.x + b.x), 0.5 * (a.y + b.y)};
    const double angle = std::atan2(b.y - a.y, b.x - a.x);
    if (phase_ != Phase::Pinch) {
        // Second finger down — or one finger of a pinch lifted and came
        // back. Either way the separation and angle baselines are re-seeded
        // from the new pair, so the handover reports no zoom and no twist.
        // The pending tap is suppressed here too, so a pinch can never also
        // commit the dimension editor.
        phase_ = Phase::Pinch;
        past_slop_ = true;
        ref_separation_ = separation;
        last_mid_ = mid;
        last_angle_ = angle;
        pending_twist_deg_ = 0.0;
        twist_engaged_ = false;
        return motion;
    }

    motion.pinch = true;
    motion.pinch_center = mid;
    motion.pinch_pan_delta = Point{mid.x - last_mid_.x, mid.y - last_mid_.y};
    last_mid_ = mid;
    if (separation >= kMinSeparationPx &&
        ref_separation_ >= kMinSeparationPx) {
        pending_log2_ += std::log2(separation / ref_separation_);
    }
    if (separation >= kMinTwistSeparationPx &&
        ref_separation_ >= kMinTwistSeparationPx) {
        constexpr double kRadToDeg = 57.29577951308232;
        pending_twist_deg_ += angleDelta(angle, last_angle_) * kRadToDeg;
        if (!twist_engaged_ &&
            std::abs(pending_twist_deg_) >= kTwistDeadZoneDeg) {
            twist_engaged_ = true;
        }
        if (twist_engaged_) {
            motion.twist_deg = pending_twist_deg_;
            pending_twist_deg_ = 0.0;
        }
    }
    last_angle_ = angle;
    ref_separation_ = separation;
    return motion;
}

inline double GestureState::angleDelta(double to, double from) noexcept {
    constexpr double kPi = 3.141592653589793;
    double delta = to - from;
    while (delta > kPi) {
        delta -= 2.0 * kPi;
    }
    while (delta < -kPi) {
        delta += 2.0 * kPi;
    }
    return delta;
}

inline bool GestureState::release() {
    const bool tap = phase_ == Phase::Single && !past_slop_;
    reset();
    return tap;
}

inline void GestureState::reset() noexcept {
    phase_ = Phase::Idle;
    past_slop_ = false;
    ref_separation_ = 0.0;
    pending_log2_ = 0.0;
    last_angle_ = 0.0;
    pending_twist_deg_ = 0.0;
    twist_engaged_ = false;
}

inline int GestureState::takeLevelSteps(int max_magnitude) noexcept {
    int steps = static_cast<int>(pending_log2_ / kPinchStepLog2);
    if (max_magnitude > 0) {
        steps = std::max(-max_magnitude, std::min(max_magnitude, steps));
    }
    pending_log2_ -= steps * kPinchStepLog2;
    return steps;
}

inline double GestureState::takeScaleFactor() noexcept {
    const double factor = std::exp2(pending_log2_);
    pending_log2_ = 0.0;
    return factor;
}

}  // namespace touch_gestures
}  // namespace f2c_cpp
