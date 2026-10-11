/**
 * @file gps_prealign.hpp
 * @brief Seeds the satellite / point-cloud alignment from the map-collection GPS fix.
 *
 * Map collection drives a short forward/back leg after the 360 pivot so the
 * GNSS receiver gets a track, and writes a position, a heading and how good
 * both are into the collection manifest. Together with the stitched site
 * image's own georeference that is a complete similarity — position, heading
 * and scale — so the correspondence step can open with the imagery already
 * turned into the robot's frame instead of the operator picking blind.
 *
 * What this is NOT: the alignment. The seed carries GPS-grade error (metres
 * on a 3D fix) and a heading from a sub-metre baseline, so it is a starting
 * view and an independent cross-check, never what Align commits. The
 * operator's picks remain the only thing the committed fit is solved from.
 *
 * ADAPTED from the parallel OCU's module of the same name
 * (pilot_control/scripts/F2C/cpp). Unlike `similarity_2d`, this one is not a
 * line-for-line port, because the two screens georeference their imagery
 * differently — that one freezes an ArcGIS view, this one has a north-up
 * stitch with a Web Mercator extent — and because the cases below were added
 * here:
 *
 *  - a fix that exists but is not usable (no fix status, null island, a
 *    position off the downloaded imagery) is told apart from no fix at all,
 *    so the screen can say which it was;
 *  - a position with no heading still seeds — it cannot turn the imagery, but
 *    it marks where to look and still cross-checks the solved origin;
 *  - the first pick re-pins the seed's position (`refitSeedThroughPair`),
 *    since one pair beats metres of GPS error on translation while leaving
 *    the heading it cannot observe alone;
 *  - geodetic offsets go through Web Mercator proper instead of a flat-earth
 *    constant, because the stitch already is Web Mercator.
 *
 * Pure functions of a fix, a georeference and a fit: no widget, no network,
 * no robot. Tested by `tests/gps_prealign_tests.cpp`.
 */

#pragma once

#include "similarity_2d.hpp"

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>

namespace f2c_cpp {

/**
 * The seed fix is the MEAN of an out-and-back track, so it sits roughly half
 * a baseline ahead of the map origin along the heading. The manifest does not
 * carry the baseline and the OCU deliberately never sets it — the launch owns
 * that default — so the offset is carried as uncertainty rather than
 * subtracted out. Half of the 1.0 m default leg.
 */
constexpr double kSeedTrackBiasM = 0.5;

/**
 * Horizontal 1-sigma past which a fix is too coarse to place anything. A
 * receiver reporting this is telling us it barely has a solution; seeding the
 * view from it would aim the operator at the wrong building with confidence.
 */
constexpr double kSeedMaxHaccM = 25.0;

/** The GPS block of a map-collection manifest, after sanity checks. */
struct GpsSeed {
    /** A usable position was recorded. False means there is nothing to seed from. */
    bool valid = false;
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    /**
     * False when the seed drive was skipped, refused, or too short to bear a
     * bearing. Position without heading cannot turn the imagery.
     */
    bool has_heading = false;
    /** Compass bearing (0 = north, 90 = east) of the point cloud's +X axis. */
    double heading_deg = 0.0;
    /** "rtk", "sbas", "3d", ... as the robot wrote it. */
    QString fix_type;
    /**
     * Horizontal 1-sigma reported by the receiver, in metres; 0 when it sent
     * no covariance. Zero means unknown, not perfect.
     */
    double hacc_m = 0.0;
};

/**
 * Builds a seed from raw manifest values, rejecting what cannot be trusted.
 *
 * `recorded` is the writer's own verdict (`gps_valid`). It is necessary but
 * not sufficient: the robot sets it whenever any sample arrived, including
 * samples the receiver itself flagged as no-fix, and those carry whatever
 * coordinates were in the message — often zeros. So a "none" fix type, a
 * non-finite or out-of-range coordinate, the exact (0, 0) of an uninitialised
 * message, and a horizontal sigma past `kSeedMaxHaccM` all come back invalid.
 *
 * A heading that is not finite is dropped on its own; the position survives.
 */
GpsSeed makeGpsSeed(bool recorded, double lat_deg, double lon_deg,
                    bool heading_recorded, double heading_deg,
                    const QString& fix_type, double hacc_m);

/**
 * 1-sigma of where the seed puts the point cloud's origin, in metres: the
 * receiver's horizontal sigma and the along-track bias above, in quadrature.
 *
 * A receiver that sent no covariance (`hacc_m == 0`) is assumed to be as good
 * as its fix type usually is and no better — centimetres for RTK, a couple of
 * metres otherwise. Treating "unknown" as zero would make the seed look
 * survey-grade exactly when least is known about it, and every cross-check
 * against it would then fire on an ordinary 3D fix.
 */
double seedPositionSigmaM(const GpsSeed& seed);

/**
 * Georeference of the stitched site image: a north-up raster whose extent is
 * known in normalized Web Mercator ([0,1] world units, Y down).
 */
struct StitchGeoRef {
    /** [min_nx, min_ny, width, height] of the stitch, normalized Web Mercator. */
    QRectF bounds;
    QSize image_size;

    bool valid() const;
    /** Stitch pixel holding a geographic point. May lie outside the image. */
    QPointF pixelFromGeo(double lat_deg, double lon_deg) const;
    /**
     * Ground metres per stitch pixel at a latitude. Web Mercator stretches
     * with latitude, so this is NOT the zoom level's nominal resolution and
     * is not the imagery source's native GSD either — it is what one pixel of
     * this particular image covers on the ground, which is the number a
     * metres -> pixels fit has to agree with.
     */
    double metresPerPixel(double lat_deg) const;
    /** Latitude of the stitch centre, for callers with no better anchor. */
    double centreLatDeg() const;
};

/** What the seed turned out to be good for, worst to best. */
enum class SeedStatus {
    /** No usable fix was recorded during collection. */
    NoFix,
    /** A fix exists but the imagery carries no georeference to place it on. */
    NoGeoRef,
    /**
     * The fix lands off the downloaded imagery. Either the plan is for a
     * different site than the robot is standing on, or the fix is wild; both
     * are worth telling the operator before they hunt for matching features.
     */
    OutsideImagery,
    /** Position only: marks where the robot is, cannot turn the imagery. */
    PositionOnly,
    /** Position and heading: a complete starting similarity. */
    Oriented,
};

/** The seed resolved against a particular stitch. */
struct SeedPlacement {
    SeedStatus status = SeedStatus::NoFix;
    /** Stitch pixel the seed puts the cloud origin at. Set from OutsideImagery up. */
    QPointF origin_px;
    /** `seedPositionSigmaM` in stitch pixels, for drawing the uncertainty. */
    double sigma_px = 0.0;
    /**
     * Point cloud metres -> stitch pixels from the seed alone. Valid only for
     * `Oriented`. Its scale is exactly the imagery's own ground scale, and it
     * is orientation-reversing by construction — the same and only branch the
     * pick-based fit is held to.
     */
    Similarity2D fit;

    /** The seed knows where the robot is on this image. */
    bool hasPosition() const {
        return status == SeedStatus::PositionOnly ||
               status == SeedStatus::Oriented;
    }
};

SeedPlacement placeSeed(const GpsSeed& seed, const StitchGeoRef& geo);

/**
 * The seed fit re-pinned through one operator pick: rotation and scale are
 * kept, translation is replaced so `pcd_m` lands exactly on `sat_px`.
 *
 * One pair observes position far better than a GPS fix does (a pick is worth
 * decimetres, a 3D fix metres) and observes heading not at all, so this is
 * the honest use of the first pick: it corrects what it can see and leaves
 * the rest to the seed until a second pair makes heading observable too.
 * Invalid in, invalid out.
 */
Similarity2D refitSeedThroughPair(const Similarity2D& seed_fit,
                                  const QPointF& pcd_m, const QPointF& sat_px);

/** How far a solved fit ended up from the seed that started it. */
struct SeedAgreement {
    /** The origins could be compared (the seed has a position). */
    bool valid = false;
    /** Distance between the seed's and the fit's cloud origin, in metres. */
    double origin_offset_m = 0.0;
    /** The headings could be compared (the seed has one). */
    bool has_heading = false;
    /** Unsigned angle between the two +X axes, in degrees, in [0, 180]. */
    double heading_delta_deg = 0.0;
};

/**
 * Compares a solved fit against the seed, both PCD metres -> stitch pixels.
 *
 * This is the cross-check the seed buys beyond the starting view: a handful
 * of picks can be mutually consistent and still be picks onto the wrong roof,
 * or onto the right roof turned half a way round. The seed is independent of
 * every one of them.
 */
SeedAgreement compareToSeed(const SeedPlacement& seed,
                            const Similarity2D& solved_fit);

}  // namespace f2c_cpp
