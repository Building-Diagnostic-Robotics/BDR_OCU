/**
 * @file touch_gesture_state_tests.cpp
 * @brief Touch pan / pinch bookkeeping: slop, hysteresis, finger handover.
 */

#include "touch_gesture_state.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using f2c_cpp::touch_gestures::GestureState;
using f2c_cpp::touch_gestures::Phase;
using f2c_cpp::touch_gestures::Point;

namespace {

GestureState::Motion feed(GestureState& state,
                          const std::vector<Point>& points) {
    return state.update(points.data(), static_cast<int>(points.size()));
}

// A pinch at `separation` px, centred on `center`, along the x axis.
std::vector<Point> pinch(Point center, double separation) {
    return {Point{center.x - separation / 2.0, center.y},
            Point{center.x + separation / 2.0, center.y}};
}

// The same pair rotated `degrees` clockwise about its midpoint.
std::vector<Point> twisted(Point center, double separation, double degrees) {
    const double rad = degrees * 3.141592653589793 / 180.0;
    const double hx = separation / 2.0 * std::cos(rad);
    const double hy = separation / 2.0 * std::sin(rad);
    return {Point{center.x - hx, center.y - hy},
            Point{center.x + hx, center.y + hy}};
}

// Spins the pair to `total` in one-degree increments and sums whatever
// twist comes back, which is how a real gesture arrives — one small delta
// per touch event. Signed: negative totals spin the other way.
double twistOver(GestureState& state, double total) {
    const double step = total < 0.0 ? -1.0 : 1.0;
    double applied = 0.0;
    double sum = 0.0;
    while (std::abs(applied) < std::abs(total)) {
        applied += step;
        if (std::abs(applied) > std::abs(total)) {
            applied = total;
        }
        sum += feed(state, twisted(Point{0.0, 0.0}, 200.0, applied)).twist_deg;
    }
    return sum;
}

}  // namespace

TEST(TouchGestureState, StartsIdleAndIgnoresEmptyInput) {
    GestureState state;
    EXPECT_EQ(state.phase(), Phase::Idle);
    const GestureState::Motion motion = state.update(nullptr, 0);
    EXPECT_FALSE(motion.pan);
    EXPECT_FALSE(motion.pinch);
    EXPECT_EQ(state.phase(), Phase::Idle);
}

// The first event only seeds the press: a pan delta measured against
// nothing would jump the view by the whole press position.
TEST(TouchGestureState, FirstSingleTouchDoesNotPan) {
    GestureState state;
    const GestureState::Motion motion = feed(state, {Point{100.0, 100.0}});
    EXPECT_FALSE(motion.pan);
    EXPECT_EQ(state.phase(), Phase::Single);
}

TEST(TouchGestureState, MotionInsideTheSlopCircleDoesNotPan) {
    GestureState state;
    feed(state, {Point{100.0, 100.0}});
    // Well inside kTapSlopPx, which is what a finger the operator means to
    // hold still actually does on a direct-touch panel.
    const GestureState::Motion motion = feed(state, {Point{104.0, 103.0}});
    EXPECT_FALSE(motion.pan);
}

TEST(TouchGestureState, PanStartsOnceTheSlopCircleIsLeft) {
    GestureState state;
    feed(state, {Point{100.0, 100.0}});
    const GestureState::Motion motion = feed(state, {Point{140.0, 100.0}});
    ASSERT_TRUE(motion.pan);
    EXPECT_DOUBLE_EQ(motion.pan_delta.x, 40.0);
    EXPECT_DOUBLE_EQ(motion.pan_delta.y, 0.0);
}

// Deltas are measured event to event, not from the press, so the view
// tracks the finger instead of accelerating away from it.
TEST(TouchGestureState, PanDeltaIsIncremental) {
    GestureState state;
    feed(state, {Point{0.0, 0.0}});
    feed(state, {Point{40.0, 0.0}});
    const GestureState::Motion motion = feed(state, {Point{50.0, 5.0}});
    ASSERT_TRUE(motion.pan);
    EXPECT_DOUBLE_EQ(motion.pan_delta.x, 10.0);
    EXPECT_DOUBLE_EQ(motion.pan_delta.y, 5.0);
}

// A sub-slop gesture is a tap. No canvas turns one into a click — it only
// commits the inline dimension editor and hands back from manual override.
TEST(TouchGestureState, StationaryFingerReleasesAsATap) {
    GestureState state;
    feed(state, {Point{200.0, 150.0}});
    feed(state, {Point{202.0, 151.0}});
    EXPECT_TRUE(state.release());
    EXPECT_EQ(state.phase(), Phase::Idle);
}

TEST(TouchGestureState, DraggedFingerDoesNotReleaseAsATap) {
    GestureState state;
    feed(state, {Point{200.0, 150.0}});
    feed(state, {Point{260.0, 150.0}});
    EXPECT_FALSE(state.release());
}

// Once past the slop circle the gesture is a pan for good, even if the
// finger wanders back to where it started before lifting.
TEST(TouchGestureState, ReturningToTheOriginIsStillNotATap) {
    GestureState state;
    feed(state, {Point{100.0, 100.0}});
    feed(state, {Point{180.0, 100.0}});
    feed(state, {Point{100.0, 100.0}});
    EXPECT_FALSE(state.release());
}

TEST(TouchGestureState, CancelReportsNoTap) {
    GestureState state;
    feed(state, {Point{200.0, 150.0}});
    state.reset();
    EXPECT_EQ(state.phase(), Phase::Idle);
    EXPECT_FALSE(state.release());
}

// The second finger landing suppresses the pending tap, so a pinch cannot
// also commit the dimension editor or drop out of manual override.
TEST(TouchGestureState, SecondFingerSuppressesThePendingTap) {
    GestureState state;
    feed(state, {Point{200.0, 150.0}});
    feed(state, pinch(Point{200.0, 150.0}, 100.0));
    EXPECT_EQ(state.phase(), Phase::Pinch);
    feed(state, pinch(Point{200.0, 150.0}, 140.0));
    EXPECT_FALSE(state.release());
}

TEST(TouchGestureState, FirstPinchEventOnlySeeds) {
    GestureState state;
    const GestureState::Motion motion =
        feed(state, pinch(Point{300.0, 200.0}, 100.0));
    EXPECT_FALSE(motion.pinch);
    EXPECT_EQ(state.takeLevelSteps(), 0);
    EXPECT_DOUBLE_EQ(state.takeScaleFactor(), 1.0);
}

TEST(TouchGestureState, PinchCenterIsTheMidpointBetweenFingers) {
    GestureState state;
    feed(state, pinch(Point{300.0, 200.0}, 100.0));
    const GestureState::Motion motion =
        feed(state, pinch(Point{300.0, 200.0}, 120.0));
    ASSERT_TRUE(motion.pinch);
    EXPECT_DOUBLE_EQ(motion.pinch_center.x, 300.0);
    EXPECT_DOUBLE_EQ(motion.pinch_center.y, 200.0);
}

// Both fingers travelling together is a pan, which is what makes the
// gesture feel like a phone map rather than a zoom-only pinch.
TEST(TouchGestureState, TravellingMidpointReportsAPanDelta) {
    GestureState state;
    feed(state, pinch(Point{300.0, 200.0}, 100.0));
    const GestureState::Motion motion =
        feed(state, pinch(Point{340.0, 180.0}, 100.0));
    ASSERT_TRUE(motion.pinch);
    EXPECT_DOUBLE_EQ(motion.pinch_pan_delta.x, 40.0);
    EXPECT_DOUBLE_EQ(motion.pinch_pan_delta.y, -20.0);
    EXPECT_EQ(state.takeLevelSteps(), 0);  // separation never changed
}

TEST(TouchGestureState, SpreadingBySqrtTwoStepsOneLevelIn) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 100.0));
    feed(state, pinch(Point{0.0, 0.0}, 141.5));
    EXPECT_EQ(state.takeLevelSteps(), 1);
}

TEST(TouchGestureState, PinchingBySqrtTwoStepsOneLevelOut) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 200.0));
    feed(state, pinch(Point{0.0, 0.0}, 141.0));
    EXPECT_EQ(state.takeLevelSteps(), -1);
}

TEST(TouchGestureState, JustUnderTheThresholdStepsNothing) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 100.0));
    feed(state, pinch(Point{0.0, 0.0}, 140.0));  // 1.40 < sqrt(2)
    EXPECT_EQ(state.takeLevelSteps(), 0);
}

// The gesture is continuous even though the tile pyramid is not: many small
// separation changes have to add up to a level.
TEST(TouchGestureState, SmallSpreadsAccumulateIntoALevel) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 100.0));
    double separation = 100.0;
    int steps = 0;
    for (int i = 0; i < 20; ++i) {
        separation *= 1.02;
        feed(state, pinch(Point{0.0, 0.0}, separation));
        steps += state.takeLevelSteps();
    }
    EXPECT_EQ(steps, 1);  // 1.02^20 = 1.49, one sqrt(2) crossing
}

// Subtracting exactly what it returns is the hysteresis: after a step the
// accumulator sits near zero, so a finger trembling on a level boundary
// needs a further sqrt(2) in either direction and cannot oscillate.
TEST(TouchGestureState, TremorOnALevelBoundaryDoesNotOscillate) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 100.0));
    feed(state, pinch(Point{0.0, 0.0}, 141.5));
    ASSERT_EQ(state.takeLevelSteps(), 1);

    int further = 0;
    for (int i = 0; i < 10; ++i) {
        // +-2 px of jitter, far short of the next sqrt(2) either way.
        feed(state, pinch(Point{0.0, 0.0}, i % 2 == 0 ? 143.5 : 139.5));
        further += state.takeLevelSteps();
    }
    EXPECT_EQ(further, 0);
}

TEST(TouchGestureState, LevelCapBanksTheRemainderRatherThanDroppingIt) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 50.0));
    feed(state, pinch(Point{0.0, 0.0}, 800.0));  // 4 doublings = 8 steps
    EXPECT_EQ(state.takeLevelSteps(2), 2);
    EXPECT_EQ(state.takeLevelSteps(2), 2);
    EXPECT_EQ(state.takeLevelSteps(), 4);
    EXPECT_EQ(state.takeLevelSteps(), 0);
}

TEST(TouchGestureState, ScaleFactorTracksTheSeparationRatio) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 100.0));
    feed(state, pinch(Point{0.0, 0.0}, 250.0));
    EXPECT_NEAR(state.takeScaleFactor(), 2.5, 1e-9);
    EXPECT_DOUBLE_EQ(state.takeScaleFactor(), 1.0);  // drained
}

TEST(TouchGestureState, ScaleFactorAndLevelStepsShareOneAccumulator) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 100.0));
    feed(state, pinch(Point{0.0, 0.0}, 200.0));
    EXPECT_NEAR(state.takeScaleFactor(), 2.0, 1e-9);
    EXPECT_EQ(state.takeLevelSteps(), 0);
}

// Fingertips almost touching report a few pixels apart and jitter hard, so
// that range must not drive the zoom.
TEST(TouchGestureState, SeparationBelowTheFloorIsNotTrusted) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 100.0));
    feed(state, pinch(Point{0.0, 0.0}, 4.0));
    EXPECT_EQ(state.takeLevelSteps(), 0);
    EXPECT_DOUBLE_EQ(state.takeScaleFactor(), 1.0);
}

// Lifting one finger of a pinch must not pan by the gap between the
// midpoint and the surviving finger — that would fling the view sideways.
TEST(TouchGestureState, DroppingToOneFingerReseedsWithoutPanning) {
    GestureState state;
    feed(state, pinch(Point{300.0, 200.0}, 200.0));
    feed(state, pinch(Point{300.0, 200.0}, 220.0));
    state.takeLevelSteps();

    const GestureState::Motion handover = feed(state, {Point{410.0, 200.0}});
    EXPECT_FALSE(handover.pan);
    EXPECT_EQ(state.phase(), Phase::Single);

    // It pans normally from there, measured against the new seed.
    const GestureState::Motion after = feed(state, {Point{450.0, 200.0}});
    ASSERT_TRUE(after.pan);
    EXPECT_DOUBLE_EQ(after.pan_delta.x, 40.0);
}

TEST(TouchGestureState, GoingBackToTwoFingersReseedsTheSeparation) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 100.0));
    feed(state, pinch(Point{0.0, 0.0}, 141.5));
    ASSERT_EQ(state.takeLevelSteps(), 1);

    feed(state, {Point{0.0, 0.0}});                    // one finger lifts
    feed(state, pinch(Point{0.0, 0.0}, 300.0));        // and comes back
    EXPECT_EQ(state.takeLevelSteps(), 0);              // no jump on re-seed
    feed(state, pinch(Point{0.0, 0.0}, 425.0));
    EXPECT_EQ(state.takeLevelSteps(), 1);
}

// ---- Twist ------------------------------------------------------------------

// Without a dead zone every pinch and every two-finger pan would impart a
// few degrees, and the map would creep off north over a session.
TEST(TouchGestureState, SmallTwistsInsideTheDeadZoneReportNothing) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 200.0));
    const double applied =
        twistOver(state, GestureState::kTwistDeadZoneDeg - 2.0);
    EXPECT_DOUBLE_EQ(applied, 0.0);
}

// Clearing the dead zone releases everything that armed it, so the gesture
// does not silently eat the first 12 degrees of a deliberate twist.
TEST(TouchGestureState, ClearingTheDeadZoneReleasesTheBankedAngle) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 200.0));
    const double applied = twistOver(state, 30.0);
    EXPECT_NEAR(applied, 30.0, 1e-6);
}

// Once engaged it stays engaged: re-arming the dead zone mid-twist would
// make the map stutter every time the operator paused.
TEST(TouchGestureState, TwistStaysEngagedAfterTheFirstRelease) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 200.0));
    ASSERT_NEAR(twistOver(state, 20.0), 20.0, 1e-6);
    const GestureState::Motion motion =
        feed(state, twisted(Point{0.0, 0.0}, 200.0, 21.0));
    EXPECT_NEAR(motion.twist_deg, 1.0, 1e-6);
}

// atan2 wraps at +-pi. A pair crossing that boundary must report the short
// way round, not a ~360 degree fling.
TEST(TouchGestureState, TwistAcrossThePiBoundaryTakesTheShortWay) {
    GestureState state;
    feed(state, twisted(Point{0.0, 0.0}, 200.0, 170.0));
    // Past 180 deg the pair's angle flips sign in atan2's range.
    double sum = 0.0;
    for (double a = 175.0; a <= 200.0; a += 5.0) {
        sum += feed(state, twisted(Point{0.0, 0.0}, 200.0, a)).twist_deg;
    }
    EXPECT_NEAR(sum, 30.0, 1e-6);
}

TEST(TouchGestureState, TwistIsSignedWithTheDirectionOfTheSpin) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 200.0));
    const double applied = twistOver(state, -30.0);
    EXPECT_NEAR(applied, -30.0, 1e-6);
}

// Near the separation floor a millimetre of jitter is tens of degrees, so
// rotation is only read with the fingers well apart.
TEST(TouchGestureState, TwistIsIgnoredWithTheFingersTooCloseTogether) {
    GestureState state;
    const double narrow = GestureState::kMinTwistSeparationPx - 10.0;
    feed(state, twisted(Point{0.0, 0.0}, narrow, 0.0));
    double sum = 0.0;
    for (double a = 5.0; a <= 90.0; a += 5.0) {
        sum += feed(state, twisted(Point{0.0, 0.0}, narrow, a)).twist_deg;
    }
    EXPECT_DOUBLE_EQ(sum, 0.0);
}

// A pinch is a pure scale change and must not smuggle in a bearing.
TEST(TouchGestureState, PureSpreadReportsNoTwist) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 100.0));
    double sum = 0.0;
    for (double sep = 120.0; sep <= 400.0; sep += 20.0) {
        sum += feed(state, pinch(Point{0.0, 0.0}, sep)).twist_deg;
    }
    EXPECT_DOUBLE_EQ(sum, 0.0);
}

// Both fingers travelling together is a pan, and a pan must not rotate.
TEST(TouchGestureState, TwoFingerPanReportsNoTwist) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 200.0));
    double sum = 0.0;
    for (double x = 20.0; x <= 200.0; x += 20.0) {
        sum += feed(state, pinch(Point{x, x / 2.0}, 200.0)).twist_deg;
    }
    EXPECT_DOUBLE_EQ(sum, 0.0);
}

// Lifting a finger and putting it back re-seeds the angle baseline, or the
// map would snap by whatever the pair's orientation changed to meanwhile.
TEST(TouchGestureState, FingerHandoverReseedsTheTwistBaseline) {
    GestureState state;
    feed(state, twisted(Point{0.0, 0.0}, 200.0, 0.0));
    ASSERT_NEAR(twistOver(state, 30.0), 30.0, 1e-6);

    feed(state, {Point{0.0, 0.0}});  // one finger lifts
    // ...and the pair comes back at a completely different orientation.
    const GestureState::Motion back =
        feed(state, twisted(Point{0.0, 0.0}, 200.0, 120.0));
    EXPECT_DOUBLE_EQ(back.twist_deg, 0.0);
    // The dead zone is re-armed with it, so the next small spin is absorbed.
    EXPECT_DOUBLE_EQ(
        feed(state, twisted(Point{0.0, 0.0}, 200.0, 122.0)).twist_deg, 0.0);
}

TEST(TouchGestureState, ReleaseClearsTheTwistAccumulator) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 200.0));
    twistOver(state, 30.0);
    state.release();

    feed(state, twisted(Point{0.0, 0.0}, 200.0, 0.0));
    EXPECT_DOUBLE_EQ(
        feed(state, twisted(Point{0.0, 0.0}, 200.0, 2.0)).twist_deg, 0.0);
}

// Fingers past the second carry nothing a pinch needs, and letting them
// re-seed the reference separation makes the zoom jump.
TEST(TouchGestureState, ExtraFingersAreIgnored) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 100.0));
    const GestureState::Motion motion = state.update(
        std::vector<Point>{Point{-70.75, 0.0}, Point{70.75, 0.0},
                           Point{500.0, 400.0}}
            .data(),
        3);
    ASSERT_TRUE(motion.pinch);
    EXPECT_DOUBLE_EQ(motion.pinch_center.x, 0.0);
    EXPECT_EQ(state.takeLevelSteps(), 1);
}

TEST(TouchGestureState, PinchThenFullLiftLeavesNoResidualZoom) {
    GestureState state;
    feed(state, pinch(Point{0.0, 0.0}, 100.0));
    feed(state, pinch(Point{0.0, 0.0}, 300.0));
    state.release();
    EXPECT_EQ(state.takeLevelSteps(), 0);
    EXPECT_DOUBLE_EQ(state.takeScaleFactor(), 1.0);
}

// A second gesture in the same session must behave like the first.
TEST(TouchGestureState, StateIsReusableAcrossGestures) {
    GestureState state;
    feed(state, {Point{10.0, 10.0}});
    feed(state, {Point{90.0, 10.0}});
    ASSERT_FALSE(state.release());

    feed(state, {Point{500.0, 300.0}});
    EXPECT_TRUE(state.release());
}
