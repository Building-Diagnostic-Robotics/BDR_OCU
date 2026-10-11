#include "gps_prealign.hpp"

#include "satellite_geo_math.hpp"

#include <algorithm>
#include <cmath>

namespace f2c_cpp {

GpsSeed makeGpsSeed(bool recorded, double lat_deg, double lon_deg,
                    bool heading_recorded, double heading_deg,
                    const QString& fix_type, double hacc_m) {
    GpsSeed seed;
    if (!recorded || !std::isfinite(lat_deg) || !std::isfinite(lon_deg)) {
        return seed;
    }
    // Web Mercator has no pole, and nothing here is ever scanned near one.
    if (std::abs(lat_deg) > 85.0 || std::abs(lon_deg) > 180.0) {
        return seed;
    }
    // An uninitialised NavSatFix. A real fix at exactly (0, 0) is a buoy in
    // the Gulf of Guinea.
    if (lat_deg == 0.0 && lon_deg == 0.0) {
        return seed;
    }
    const QString fix = fix_type.trimmed();
    if (fix.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0) {
        return seed;
    }
    const double hacc =
        (std::isfinite(hacc_m) && hacc_m > 0.0) ? hacc_m : 0.0;
    if (hacc > kSeedMaxHaccM) {
        return seed;
    }
    seed.valid = true;
    seed.lat_deg = lat_deg;
    seed.lon_deg = lon_deg;
    seed.fix_type = fix;
    seed.hacc_m = hacc;
    if (heading_recorded && std::isfinite(heading_deg)) {
        seed.has_heading = true;
        // Normalised here so a writer that ever emits a signed bearing does
        // not reach the trigonometry as one.
        seed.heading_deg = std::fmod(std::fmod(heading_deg, 360.0) + 360.0, 360.0);
    }
    return seed;
}

double seedPositionSigmaM(const GpsSeed& seed) {
    double hacc = 0.0;
    if (seed.hacc_m > 0.0 && std::isfinite(seed.hacc_m)) {
        hacc = seed.hacc_m;
    } else if (seed.fix_type.startsWith(QLatin1String("rtk"),
                                        Qt::CaseInsensitive)) {
        hacc = 0.05;
    } else if (seed.fix_type.compare(QLatin1String("sbas"),
                                     Qt::CaseInsensitive) == 0) {
        hacc = 1.5;
    } else {
        hacc = 3.0;
    }
    return std::sqrt(hacc * hacc + kSeedTrackBiasM * kSeedTrackBiasM);
}

bool StitchGeoRef::valid() const {
    return bounds.width() > 0.0 && bounds.height() > 0.0 &&
           image_size.width() > 0 && image_size.height() > 0;
}

QPointF StitchGeoRef::pixelFromGeo(double lat_deg, double lon_deg) const {
    // Inverse of TileService::geoFromStitchPixel.
    const double u = (geo::lonToNormX(lon_deg) - bounds.x()) / bounds.width();
    const double v = (geo::latToNormY(lat_deg) - bounds.y()) / bounds.height();
    return QPointF(u * image_size.width(), v * image_size.height());
}

double StitchGeoRef::metresPerPixel(double lat_deg) const {
    if (!valid()) {
        return 0.0;
    }
    return bounds.width() / double(image_size.width()) *
           geo::metersPerNormUnit(lat_deg);
}

double StitchGeoRef::centreLatDeg() const {
    return geo::normYToLat(bounds.y() + 0.5 * bounds.height());
}

SeedPlacement placeSeed(const GpsSeed& seed, const StitchGeoRef& geo) {
    SeedPlacement out;
    if (!seed.valid) {
        return out;  // NoFix
    }
    if (!geo.valid()) {
        out.status = SeedStatus::NoGeoRef;
        return out;
    }
    const double mpp = geo.metresPerPixel(seed.lat_deg);
    if (!(mpp > 1e-9) || !std::isfinite(mpp)) {
        out.status = SeedStatus::NoGeoRef;
        return out;
    }
    out.origin_px = geo.pixelFromGeo(seed.lat_deg, seed.lon_deg);
    out.sigma_px = seedPositionSigmaM(seed) / mpp;
    if (!QRectF(QPointF(0.0, 0.0), QSizeF(geo.image_size))
             .contains(out.origin_px)) {
        out.status = SeedStatus::OutsideImagery;
        return out;
    }
    if (!seed.has_heading) {
        out.status = SeedStatus::PositionOnly;
        return out;
    }

    // Point cloud +X is the robot's forward axis, at compass bearing h; +Y is
    // a quarter turn to its left. So a cloud point (x, y) is this far east and
    // north of the origin:
    //     east  = x sin h - y cos h
    //     north = x cos h + y sin h
    // The stitch is north-up with Y down, so a pixel moves by (east, -north).
    const double h = seed.heading_deg * geo::kDegToRad;
    const double sin_h = std::sin(h);
    const double cos_h = std::cos(h);
    const double px_per_m = 1.0 / mpp;
    out.fit.a00 = sin_h * px_per_m;
    out.fit.a01 = -cos_h * px_per_m;
    out.fit.a10 = -cos_h * px_per_m;
    out.fit.a11 = -sin_h * px_per_m;
    out.fit.tx = out.origin_px.x();
    out.fit.ty = out.origin_px.y();
    // Determinant is -px_per_m^2 for every heading: right-handed robot XY into
    // a Y-down raster. Never a choice, so never a mirror.
    out.fit.reflected = true;
    out.fit.valid = true;
    out.status = SeedStatus::Oriented;
    return out;
}

Similarity2D refitSeedThroughPair(const Similarity2D& seed_fit,
                                  const QPointF& pcd_m, const QPointF& sat_px) {
    Similarity2D fit = seed_fit;
    if (!fit.valid) {
        return fit;
    }
    const QPointF miss = sat_px - seed_fit.apply(pcd_m);
    fit.tx += miss.x();
    fit.ty += miss.y();
    return fit;
}

SeedAgreement compareToSeed(const SeedPlacement& seed,
                            const Similarity2D& solved_fit) {
    SeedAgreement out;
    if (!seed.hasPosition() || !solved_fit.valid) {
        return out;
    }
    const double px_per_m = solved_fit.scalePxPerM();
    if (!(px_per_m > 1e-9) || !std::isfinite(px_per_m)) {
        return out;
    }
    const QPointF solved_origin = solved_fit.apply(QPointF(0.0, 0.0));
    const QPointF delta = solved_origin - seed.origin_px;
    // Measured in the solved fit's own scale: it is the one the committed
    // alignment is expressed in.
    out.origin_offset_m = std::hypot(delta.x(), delta.y()) / px_per_m;
    out.valid = true;

    if (seed.status != SeedStatus::Oriented || !seed.fit.valid) {
        return out;
    }
    const QPointF seed_x = seed.fit.apply(QPointF(1.0, 0.0)) - seed.origin_px;
    const QPointF solved_x =
        solved_fit.apply(QPointF(1.0, 0.0)) - solved_origin;
    double delta_deg = (std::atan2(solved_x.y(), solved_x.x()) -
                        std::atan2(seed_x.y(), seed_x.x())) /
                       geo::kDegToRad;
    delta_deg = std::fmod(std::fmod(delta_deg, 360.0) + 360.0, 360.0);
    if (delta_deg > 180.0) {
        delta_deg = 360.0 - delta_deg;
    }
    out.has_heading = true;
    out.heading_delta_deg = delta_deg;
    return out;
}

}  // namespace f2c_cpp
