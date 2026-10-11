/**
 * @file gps_prealign_tests.cpp
 * @brief GPS seed for the satellite / point-cloud alignment.
 *
 * The seed's whole job is to be right about DIRECTION and roughly right about
 * position, so most of these are about which way things face — north vs up,
 * east vs right, +Y vs the robot's left — and about never producing a mirror.
 * The rest pin the cases a real collection throws up: no fix, a fix with no
 * heading, a fix the receiver itself did not believe, and a fix that is not on
 * the imagery the plan downloaded.
 */

#include "gps_prealign.hpp"
#include "satellite_geo_math.hpp"
#include "similarity_2d.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace f2c_cpp;

namespace {

constexpr double kLat = 40.44062;
constexpr double kLon = -79.99589;
constexpr double kPxPerM = 4.0;  // 0.25 m/px, about z19 at this latitude

/// A north-up 2000x1500 stitch whose centre pixel is (kLat, kLon).
StitchGeoRef stitch() {
    StitchGeoRef g;
    g.image_size = QSize(2000, 1500);
    const double norm_per_px = 1.0 / (kPxPerM * geo::metersPerNormUnit(kLat));
    g.bounds = QRectF(geo::lonToNormX(kLon) - 1000.0 * norm_per_px,
                      geo::latToNormY(kLat) - 750.0 * norm_per_px,
                      2000.0 * norm_per_px, 1500.0 * norm_per_px);
    return g;
}

GpsSeed seedAt(double east_m, double north_m, double heading_deg,
               bool has_heading = true) {
    const geo::GeoPoint p =
        geo::geoFromEnu(geo::GeoPoint{kLat, kLon}, east_m, north_m);
    return makeGpsSeed(true, p.lat, p.lon, has_heading, heading_deg,
                       QStringLiteral("rtk"), 0.02);
}

double det(const Similarity2D& f) { return f.a00 * f.a11 - f.a01 * f.a10; }

}  // namespace

// ---- which fixes are usable -------------------------------------------------

TEST(GpsSeed, CompleteFixIsUsable) {
    const GpsSeed s = makeGpsSeed(true, kLat, kLon, true, 143.25,
                                  QStringLiteral("rtk"), 0.02);
    EXPECT_TRUE(s.valid);
    EXPECT_TRUE(s.has_heading);
    EXPECT_DOUBLE_EQ(s.heading_deg, 143.25);
}

TEST(GpsSeed, NotRecordedIsNotUsable) {
    EXPECT_FALSE(makeGpsSeed(false, kLat, kLon, true, 10.0,
                             QStringLiteral("rtk"), 0.02)
                     .valid);
}

TEST(GpsSeed, NoFixStatusIsRejectedEvenWhenFlaggedValid) {
    // The robot sets gps_valid whenever a sample arrived, including samples
    // the receiver flagged NO_FIX.
    EXPECT_FALSE(makeGpsSeed(true, kLat, kLon, true, 10.0,
                             QStringLiteral("none"), 0.0)
                     .valid);
}

TEST(GpsSeed, NullIslandAndGarbageAreRejected) {
    const QString rtk = QStringLiteral("rtk");
    EXPECT_FALSE(makeGpsSeed(true, 0.0, 0.0, false, 0.0, rtk, 0.0).valid);
    EXPECT_FALSE(makeGpsSeed(true, std::nan(""), kLon, false, 0.0, rtk, 0.0).valid);
    EXPECT_FALSE(makeGpsSeed(true, 91.0, kLon, false, 0.0, rtk, 0.0).valid);
    EXPECT_FALSE(makeGpsSeed(true, kLat, 181.0, false, 0.0, rtk, 0.0).valid);
}

TEST(GpsSeed, HopelesslyCoarseFixIsRejected) {
    EXPECT_FALSE(makeGpsSeed(true, kLat, kLon, true, 10.0,
                             QStringLiteral("3d"), kSeedMaxHaccM + 1.0)
                     .valid);
    EXPECT_TRUE(makeGpsSeed(true, kLat, kLon, true, 10.0,
                            QStringLiteral("3d"), kSeedMaxHaccM - 1.0)
                    .valid);
}

TEST(GpsSeed, BadHeadingDropsOnlyTheHeading) {
    const GpsSeed s = makeGpsSeed(true, kLat, kLon, true, std::nan(""),
                                  QStringLiteral("3d"), 1.0);
    EXPECT_TRUE(s.valid);
    EXPECT_FALSE(s.has_heading);
}

TEST(GpsSeed, SignedBearingIsNormalised) {
    const GpsSeed s = makeGpsSeed(true, kLat, kLon, true, -90.0,
                                  QStringLiteral("rtk"), 0.02);
    EXPECT_NEAR(s.heading_deg, 270.0, 1e-9);
}

TEST(GpsSeed, UnknownCovarianceIsNotTreatedAsPerfect) {
    const GpsSeed plain = makeGpsSeed(true, kLat, kLon, false, 0.0,
                                      QStringLiteral("3d"), 0.0);
    const GpsSeed rtk = makeGpsSeed(true, kLat, kLon, false, 0.0,
                                    QStringLiteral("rtk"), 0.0);
    EXPECT_GT(seedPositionSigmaM(plain), 2.0);
    EXPECT_LT(seedPositionSigmaM(rtk), 1.0);
    // Never below the along-track bias, however good the receiver.
    EXPECT_GE(seedPositionSigmaM(rtk), kSeedTrackBiasM);
}

// ---- georeference -----------------------------------------------------------

TEST(StitchGeoRef, ScaleIsThePixelGroundSizeNotANominalOne) {
    const StitchGeoRef g = stitch();
    ASSERT_TRUE(g.valid());
    EXPECT_NEAR(g.metresPerPixel(kLat), 1.0 / kPxPerM, 1e-9);
    EXPECT_NEAR(g.centreLatDeg(), kLat, 1e-6);
}

TEST(StitchGeoRef, PixelFromGeoPutsNorthUpAndEastRight) {
    const StitchGeoRef g = stitch();
    const geo::GeoPoint here{kLat, kLon};
    const QPointF centre = g.pixelFromGeo(kLat, kLon);
    EXPECT_NEAR(centre.x(), 1000.0, 1e-6);
    EXPECT_NEAR(centre.y(), 750.0, 1e-6);
    const geo::GeoPoint north = geo::geoFromEnu(here, 0.0, 100.0);
    const geo::GeoPoint east = geo::geoFromEnu(here, 100.0, 0.0);
    const QPointF n = g.pixelFromGeo(north.lat, north.lon);
    const QPointF e = g.pixelFromGeo(east.lat, east.lon);
    EXPECT_NEAR(n.x(), 1000.0, 1e-3);
    EXPECT_NEAR(n.y(), 750.0 - 100.0 * kPxPerM, 0.05);
    EXPECT_NEAR(e.x(), 1000.0 + 100.0 * kPxPerM, 0.05);
    EXPECT_NEAR(e.y(), 750.0, 1e-3);
}

// ---- placement --------------------------------------------------------------

TEST(PlaceSeed, NoFixIsNoFix) {
    EXPECT_EQ(placeSeed(GpsSeed{}, stitch()).status, SeedStatus::NoFix);
}

TEST(PlaceSeed, FixWithoutGeoreferenceCannotBePlaced) {
    EXPECT_EQ(placeSeed(seedAt(0, 0, 0), StitchGeoRef{}).status,
              SeedStatus::NoGeoRef);
}

TEST(PlaceSeed, FixOffTheImageryIsCalledOut) {
    const SeedPlacement p = placeSeed(seedAt(900.0, 0.0, 0.0), stitch());
    EXPECT_EQ(p.status, SeedStatus::OutsideImagery);
    EXPECT_FALSE(p.hasPosition());
    EXPECT_FALSE(p.fit.valid);
}

TEST(PlaceSeed, PositionWithoutHeadingMarksButDoesNotTurn) {
    const SeedPlacement p =
        placeSeed(seedAt(20.0, 10.0, 0.0, /*has_heading=*/false), stitch());
    EXPECT_EQ(p.status, SeedStatus::PositionOnly);
    EXPECT_TRUE(p.hasPosition());
    EXPECT_FALSE(p.fit.valid);
    EXPECT_NEAR(p.origin_px.x(), 1000.0 + 20.0 * kPxPerM, 0.05);
    EXPECT_NEAR(p.origin_px.y(), 750.0 - 10.0 * kPxPerM, 0.05);
    EXPECT_GT(p.sigma_px, 0.0);
}

TEST(PlaceSeed, HeadingNorthPutsForwardUpAndLeftLeft) {
    const SeedPlacement p = placeSeed(seedAt(0, 0, 0.0), stitch());
    ASSERT_EQ(p.status, SeedStatus::Oriented);
    const QPointF o = p.fit.apply(QPointF(0, 0));
    const QPointF fwd = p.fit.apply(QPointF(10, 0)) - o;   // +X = forward
    const QPointF left = p.fit.apply(QPointF(0, 10)) - o;  // +Y = robot's left
    EXPECT_NEAR(fwd.x(), 0.0, 1e-6);
    EXPECT_NEAR(fwd.y(), -10.0 * kPxPerM, 1e-6);   // up the image
    EXPECT_NEAR(left.x(), -10.0 * kPxPerM, 1e-6);  // west = image left
    EXPECT_NEAR(left.y(), 0.0, 1e-6);
}

TEST(PlaceSeed, HeadingEastPutsForwardRightAndLeftUp) {
    const SeedPlacement p = placeSeed(seedAt(0, 0, 90.0), stitch());
    ASSERT_EQ(p.status, SeedStatus::Oriented);
    const QPointF o = p.fit.apply(QPointF(0, 0));
    const QPointF fwd = p.fit.apply(QPointF(10, 0)) - o;
    const QPointF left = p.fit.apply(QPointF(0, 10)) - o;
    EXPECT_NEAR(fwd.x(), 10.0 * kPxPerM, 1e-6);
    EXPECT_NEAR(fwd.y(), 0.0, 1e-6);
    EXPECT_NEAR(left.x(), 0.0, 1e-6);
    EXPECT_NEAR(left.y(), -10.0 * kPxPerM, 1e-6);  // north = up
}

TEST(PlaceSeed, IsNeverAMirrorAtAnyHeading) {
    for (double h = 0.0; h < 360.0; h += 17.0) {
        const SeedPlacement p = placeSeed(seedAt(3.0, -4.0, h), stitch());
        ASSERT_EQ(p.status, SeedStatus::Oriented) << h;
        // Metres Y-up into pixels Y-down: reversing, and by exactly the scale.
        // (Tolerance covers Mercator's stretch over the seed's 4 m of latitude.)
        EXPECT_NEAR(det(p.fit), -kPxPerM * kPxPerM, 1e-3) << h;
        EXPECT_TRUE(p.fit.reflected) << h;
        EXPECT_NEAR(p.fit.scalePxPerM(), kPxPerM, 1e-4) << h;
    }
}

TEST(PlaceSeed, AgreesWithTheSolverOnTheSameGeometry) {
    // The seed and the pick-based fit must describe one transform when they
    // are given the same truth, or the hand-over from one to the other at the
    // second pick would visibly jump.
    const SeedPlacement p = placeSeed(seedAt(12.0, -7.0, 63.0), stitch());
    ASSERT_EQ(p.status, SeedStatus::Oriented);
    const QVector<QPointF> pcd{{-20, -10}, {25, -12}, {18, 21}, {-22, 17}};
    QVector<QPointF> sat;
    for (const QPointF& m : pcd) {
        sat.append(p.fit.apply(m));
    }
    const auto solved = estimateSimilarity2D(pcd, sat, QVector<double>(),
                                             FitHandedness::Reversing);
    ASSERT_TRUE(solved.has_value());
    EXPECT_NEAR(solved->rmse_m, 0.0, 1e-9);
    const SeedAgreement a = compareToSeed(p, solved->transform);
    ASSERT_TRUE(a.valid);
    EXPECT_NEAR(a.origin_offset_m, 0.0, 1e-6);
    EXPECT_NEAR(a.heading_delta_deg, 0.0, 1e-6);
}

// ---- first pick -------------------------------------------------------------

TEST(RefitThroughPair, MovesPositionAndKeepsHeadingAndScale) {
    const SeedPlacement p = placeSeed(seedAt(0, 0, 30.0), stitch());
    const QPointF pcd_m(8.0, -3.0);
    // The pick says this cloud point is 5 m east, 2 m south of where GPS has it.
    const QPointF sat_px =
        p.fit.apply(pcd_m) + QPointF(5.0 * kPxPerM, 2.0 * kPxPerM);
    const Similarity2D pinned = refitSeedThroughPair(p.fit, pcd_m, sat_px);
    ASSERT_TRUE(pinned.valid);
    const QPointF hit = pinned.apply(pcd_m);
    EXPECT_NEAR(hit.x(), sat_px.x(), 1e-9);
    EXPECT_NEAR(hit.y(), sat_px.y(), 1e-9);
    EXPECT_DOUBLE_EQ(pinned.a00, p.fit.a00);
    EXPECT_DOUBLE_EQ(pinned.a01, p.fit.a01);
    EXPECT_DOUBLE_EQ(pinned.a10, p.fit.a10);
    EXPECT_DOUBLE_EQ(pinned.a11, p.fit.a11);
    const SeedAgreement a = compareToSeed(p, pinned);
    EXPECT_NEAR(a.origin_offset_m, std::hypot(5.0, 2.0), 1e-6);
    EXPECT_NEAR(a.heading_delta_deg, 0.0, 1e-9);
}

TEST(RefitThroughPair, InvalidSeedStaysInvalid) {
    EXPECT_FALSE(
        refitSeedThroughPair(Similarity2D{}, QPointF(1, 1), QPointF(2, 2)).valid);
}

// ---- cross-check ------------------------------------------------------------

TEST(CompareToSeed, CatchesAFitTurnedHalfWayRound) {
    const SeedPlacement p = placeSeed(seedAt(0, 0, 40.0), stitch());
    Similarity2D turned = p.fit;
    turned.a00 = -turned.a00;
    turned.a01 = -turned.a01;
    turned.a10 = -turned.a10;
    turned.a11 = -turned.a11;
    const SeedAgreement a = compareToSeed(p, turned);
    ASSERT_TRUE(a.valid);
    ASSERT_TRUE(a.has_heading);
    EXPECT_NEAR(a.heading_delta_deg, 180.0, 1e-6);
}

TEST(CompareToSeed, HeadingDeltaWrapsThroughNorth) {
    const SeedPlacement a = placeSeed(seedAt(0, 0, 350.0), stitch());
    const SeedPlacement b = placeSeed(seedAt(0, 0, 10.0), stitch());
    EXPECT_NEAR(compareToSeed(a, b.fit).heading_delta_deg, 20.0, 1e-6);
}

TEST(CompareToSeed, PositionOnlySeedStillChecksTheOrigin) {
    const SeedPlacement pos =
        placeSeed(seedAt(0, 0, 0.0, /*has_heading=*/false), stitch());
    const SeedPlacement elsewhere = placeSeed(seedAt(30.0, 40.0, 75.0), stitch());
    const SeedAgreement a = compareToSeed(pos, elsewhere.fit);
    ASSERT_TRUE(a.valid);
    EXPECT_FALSE(a.has_heading);
    EXPECT_NEAR(a.origin_offset_m, 50.0, 0.05);
}

TEST(CompareToSeed, NothingToCompareWithoutAPosition) {
    const SeedPlacement good = placeSeed(seedAt(0, 0, 0.0), stitch());
    EXPECT_FALSE(compareToSeed(SeedPlacement{}, good.fit).valid);
    EXPECT_FALSE(
        compareToSeed(placeSeed(seedAt(900.0, 0, 0), stitch()), good.fit).valid);
    EXPECT_FALSE(compareToSeed(good, Similarity2D{}).valid);
}
