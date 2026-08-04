/**
 * @file geo_math.hpp
 * @brief Web-Mercator and local-tangent-plane math shared by the map widget,
 *        ROI model, and the geo -> robot_init export transform.
 *
 * Conventions:
 *  - Headings are compass degrees: 0 = North, 90 = East (clockwise).
 *  - Local offsets are ENU meters (E = east, N = north) around an anchor.
 *  - robot_init is the ROS body frame at launch: x = robot heading, y = left.
 */

#pragma once

#include <QPointF>

#include <cmath>

namespace f2c_cpp {
namespace geo {

constexpr double kEarthCircumferenceM = 40075016.686;
constexpr double kDegToRad = M_PI / 180.0;

struct GeoPoint {
    double lat = 0.0;
    double lon = 0.0;
};

/** Compass-heading pose used for the robot marker and mission anchor. */
struct GeoPose {
    double lat = 0.0;
    double lon = 0.0;
    double heading_deg = 0.0;
    bool valid = false;
};

// ---- Web Mercator normalized [0,1] world coordinates -----------------------

inline double lonToNormX(double lon) { return (lon + 180.0) / 360.0; }

inline double latToNormY(double lat) {
    const double rad = lat * kDegToRad;
    const double merc = std::log(std::tan(rad) + 1.0 / std::cos(rad));
    return (1.0 - merc / M_PI) / 2.0;
}

inline double normXToLon(double nx) { return nx * 360.0 - 180.0; }

inline double normYToLat(double ny) {
    const double merc = M_PI * (1.0 - 2.0 * ny);
    return std::atan(std::sinh(merc)) / kDegToRad;
}

/** Ground meters covered by one full normalized world unit at a latitude. */
inline double metersPerNormUnit(double lat) {
    return kEarthCircumferenceM * std::cos(lat * kDegToRad);
}

/** Ground meters per screen pixel at a latitude and integer zoom. */
inline double metersPerPixel(double lat, int zoom) {
    return metersPerNormUnit(lat) / (256.0 * double(1 << zoom));
}

// ---- Local tangent plane (ENU meters around an anchor) ---------------------

/** ENU offset in meters from anchor to point (small-area approximation). */
inline QPointF enuFromGeo(const GeoPoint& anchor, const GeoPoint& point) {
    const double m_per_norm = metersPerNormUnit(anchor.lat);
    const double east =
        (lonToNormX(point.lon) - lonToNormX(anchor.lon)) * m_per_norm;
    const double north =
        -(latToNormY(point.lat) - latToNormY(anchor.lat)) * m_per_norm;
    return QPointF(east, north);
}

inline GeoPoint geoFromEnu(const GeoPoint& anchor, double east, double north) {
    const double m_per_norm = metersPerNormUnit(anchor.lat);
    const double nx = lonToNormX(anchor.lon) + east / m_per_norm;
    const double ny = latToNormY(anchor.lat) - north / m_per_norm;
    return GeoPoint{normYToLat(ny), normXToLon(nx)};
}

// ---- Body frame (robot_init) <-> ENU ----------------------------------------

/**
 * ENU offset -> body frame for a robot facing `heading_deg` (compass).
 * Body x = forward (along heading), body y = left.
 */
inline QPointF bodyFromEnu(const QPointF& enu, double heading_deg) {
    const double s = std::sin(heading_deg * kDegToRad);
    const double c = std::cos(heading_deg * kDegToRad);
    return QPointF(enu.x() * s + enu.y() * c,    // forward
                   -enu.x() * c + enu.y() * s);  // left
}

inline QPointF enuFromBody(const QPointF& body, double heading_deg) {
    const double s = std::sin(heading_deg * kDegToRad);
    const double c = std::cos(heading_deg * kDegToRad);
    return QPointF(body.x() * s - body.y() * c,
                   body.x() * c + body.y() * s);
}

}  // namespace geo
}  // namespace f2c_cpp
