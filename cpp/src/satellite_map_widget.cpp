#include "satellite_map_widget.hpp"

#include "satellite_palette.hpp"
#include "satellite_tile_service.hpp"
#include "units_system.hpp"

#include <QBrush>
#include <QInputDialog>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <cmath>

namespace f2c_cpp {

namespace {

constexpr int kTileSize = 256;
constexpr double kHandleRadiusPx = 7.0;
constexpr double kHitRadiusPx = 12.0;
constexpr double kMarkerRadiusPx = 11.0;
constexpr double kMarkerArrowPx = 30.0;
constexpr double kRotateHandleOffsetPx = 28.0;

constexpr const char* kAttribution =
    "Esri, Maxar, Earthstar Geographics, and the GIS User Community";

double compassFromEnuVector(const QPointF& enu) {
    return std::fmod(std::atan2(enu.x(), enu.y()) / geo::kDegToRad + 360.0,
                     360.0);
}

}  // namespace

SatelliteMapWidget::SatelliteMapWidget(TileService* tiles, QWidget* parent)
    : QWidget(parent), tiles_(tiles) {
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setFocusPolicy(Qt::ClickFocus);
    connect(tiles_, &TileService::tileReady, this,
            [this](int, int, int) { update(); });
    // Dimension chips + scale bar are unit-aware; repaint on toggle.
    connect(UnitsProvider::instance(), &UnitsProvider::unitsChanged, this,
            [this](Units) { update(); });
    // Overzoom fallback chain: when a tile fails (usually because the zoom
    // exceeds the imagery's native LOD), pull its parent so paintTiles()
    // always has an ancestor to scale up. Cascades until a level that
    // exists; bounded by kMinZoom and the service's failed-tile memory.
    connect(tiles_, &TileService::tileFailed, this,
            [this](int z, int x, int y) {
                if (z > kMinZoom) {
                    tiles_->fetch(z - 1, x >> 1, y >> 1);
                }
                update();
            });
}

// ---- View state -------------------------------------------------------------

void SatelliteMapWidget::setImageryEnabled(bool enabled) {
    imagery_enabled_ = enabled;
    zoom_ = qBound(kMinZoom, zoom_, maxZoomNow());
    update();
}

int SatelliteMapWidget::maxZoomNow() const {
    if (!imagery_enabled_) {
        return kMaxZoomGrid;
    }
    if (tiles_ && tiles_->maxZoomCap() > 0) {
        return tiles_->maxZoomCap();
    }
    return kMaxZoom;
}

void SatelliteMapWidget::setView(double lat, double lon, int zoom) {
    center_nx_ = geo::lonToNormX(lon);
    center_ny_ = geo::latToNormY(lat);
    zoom_ = qBound(kMinZoom, zoom, maxZoomNow());
    clampCenter();
    update();
    emitViewChanged();
}

double SatelliteMapWidget::centerLat() const {
    return geo::normYToLat(center_ny_);
}

double SatelliteMapWidget::centerLon() const {
    return geo::normXToLon(center_nx_);
}

void SatelliteMapWidget::clampCenter() {
    center_ny_ = qBound(0.0, center_ny_, 1.0);
    center_nx_ = center_nx_ - std::floor(center_nx_);
}

void SatelliteMapWidget::emitViewChanged() {
    emit viewChanged(centerLat(), centerLon(), zoom_);
}

// ---- Coordinate helpers -----------------------------------------------------

QPointF SatelliteMapWidget::screenFromNorm(double nx, double ny) const {
    const double world_px = double(kTileSize) * (1 << zoom_);
    return QPointF((nx - center_nx_) * world_px + width() / 2.0,
                   (ny - center_ny_) * world_px + height() / 2.0);
}

QPointF SatelliteMapWidget::screenFromGeo(const geo::GeoPoint& point) const {
    return screenFromNorm(geo::lonToNormX(point.lon),
                          geo::latToNormY(point.lat));
}

geo::GeoPoint SatelliteMapWidget::geoFromScreen(const QPointF& pos) const {
    const double world_px = double(kTileSize) * (1 << zoom_);
    const double nx = center_nx_ + (pos.x() - width() / 2.0) / world_px;
    const double ny = center_ny_ + (pos.y() - height() / 2.0) / world_px;
    return geo::GeoPoint{geo::normYToLat(ny), geo::normXToLon(nx)};
}

QPointF SatelliteMapWidget::screenFromBody(const QPointF& body) const {
    const geo::GeoPoint anchor{mission_anchor_.lat, mission_anchor_.lon};
    const QPointF enu = geo::enuFromBody(body, mission_anchor_.heading_deg);
    return screenFromGeo(geo::geoFromEnu(anchor, enu.x(), enu.y()));
}

double SatelliteMapWidget::metersPerPixelNow() const {
    return geo::metersPerPixel(centerLat(), zoom_);
}

// ---- Plan objects -----------------------------------------------------------

void SatelliteMapWidget::setRoi(const RoiRect& roi) {
    roi_ = roi;
    if (roi.valid) {
        polygon_ = RoiPolygon::fromRect(roi);
    }
    update();
}

void SatelliteMapWidget::setPolygon(const RoiPolygon& poly) {
    polygon_ = poly;
    polygon_.ensureEdgeFlags();
    if (polygon_.valid() && polygon_.vertices.size() == 4) {
        // Keep the rectangle mirror in sync for length/width spinboxes.
        roi_.valid = true;
        roi_.center = polygon_.vertices[0];
        // Approximate: use first vertex as a corner, not exact.
    }
    update();
    emit roiChanged();
}

void SatelliteMapWidget::armPolygonDraw() {
    draw_polygon_armed_ = true;
    polygon_ = RoiPolygon{};
    setCursor(Qt::CrossCursor);
}

void SatelliteMapWidget::clearPolygon() {
    polygon_ = RoiPolygon{};
    roi_.valid = false;
    update();
    emit roiChanged();
}

bool SatelliteMapWidget::setEdgeLength(int edge, double meters) {
    const int n = polygon_.vertices.size();
    if (edge < 0 || edge >= n || meters < 0.5) {
        return false;
    }
    const geo::GeoPoint a = polygon_.vertices[edge];
    const geo::GeoPoint b = polygon_.vertices[(edge + 1) % n];
    const QPointF enu = geo::enuFromGeo(a, b);
    const double cur = std::hypot(enu.x(), enu.y());
    if (cur < 1e-6) {
        return false;
    }
    const double s = meters / cur;
    polygon_.vertices[(edge + 1) % n] =
        geo::geoFromEnu(a, enu.x() * s, enu.y() * s);
    update();
    emit roiChanged();
    return true;
}

QVector<QPointF> SatelliteMapWidget::polygonScreenPoints() const {
    QVector<QPointF> out;
    out.reserve(polygon_.vertices.size());
    for (const geo::GeoPoint& p : polygon_.vertices) {
        out.append(screenFromGeo(p));
    }
    return out;
}

void SatelliteMapWidget::addRoiAtViewCenter() {
    roi_.valid = true;
    roi_.center = geo::GeoPoint{centerLat(), centerLon()};
    if (roi_.length_m <= 0.0) roi_.length_m = 20.0;
    if (roi_.width_m <= 0.0) roi_.width_m = 15.0;
    if (marker_.valid) {
        roi_.heading_deg = marker_.heading_deg;
    }
    polygon_ = RoiPolygon::fromRect(roi_);
    update();
    emit roiChanged();
}

void SatelliteMapWidget::setMarker(const geo::GeoPose& marker) {
    marker_ = marker;
    update();
}

void SatelliteMapWidget::armMarkerPlacement() {
    place_marker_armed_ = true;
    setCursor(Qt::CrossCursor);
}

void SatelliteMapWidget::setEditLocked(bool locked) {
    edit_locked_ = locked;
    place_marker_armed_ = false;
    update();
}

// ---- Telemetry --------------------------------------------------------------

void SatelliteMapWidget::setMissionAnchor(const geo::GeoPose& anchor) {
    mission_anchor_ = anchor;
    trail_.clear();
    update();
}

void SatelliteMapWidget::clearMissionAnchor() {
    mission_anchor_ = geo::GeoPose{};
    update();
}

void SatelliteMapWidget::setGrid(const GridSnapshot& grid) {
    grid_ = grid;
    update();
}

void SatelliteMapWidget::setPath(const PolylineSet& path) {
    path_ = path;
    update();
}

void SatelliteMapWidget::setSwaths(const PolylineSet& swaths) {
    swaths_ = swaths;
    update();
}

void SatelliteMapWidget::setOdom(const OdomSnapshot& odom) {
    odom_ = odom;
    if (odom.valid) {
        const QPointF pos(odom.x, odom.y);
        if (trail_.isEmpty() ||
            QLineF(trail_.last(), pos).length() > 0.15) {
            trail_.append(pos);
            if (trail_.size() > 8000) {
                trail_.remove(0, 2000);
            }
        }
    }
    update();
}

void SatelliteMapWidget::clearTelemetry() {
    grid_ = GridSnapshot{};
    path_ = PolylineSet{};
    swaths_ = PolylineSet{};
    odom_ = OdomSnapshot{};
    trail_.clear();
    update();
}

// ---- Overlay screen geometry ------------------------------------------------

QVector<QPointF> SatelliteMapWidget::roiCornerScreenPoints() const {
    QVector<QPointF> out;
    for (const geo::GeoPoint& corner : roi_.corners()) {
        out.append(screenFromGeo(corner));
    }
    return out;
}

QPointF SatelliteMapWidget::roiRotateHandleScreen() const {
    // Beyond the forward (+heading) edge midpoint.
    const double s = std::sin(roi_.heading_deg * geo::kDegToRad);
    const double c = std::cos(roi_.heading_deg * geo::kDegToRad);
    const double extra_m =
        roi_.length_m / 2.0 + kRotateHandleOffsetPx * metersPerPixelNow();
    const geo::GeoPoint handle =
        geo::geoFromEnu(roi_.center, extra_m * s, extra_m * c);
    return screenFromGeo(handle);
}

QPointF SatelliteMapWidget::markerScreenPos() const {
    return screenFromGeo(geo::GeoPoint{marker_.lat, marker_.lon});
}

QPointF SatelliteMapWidget::markerArrowTipScreen() const {
    const QPointF base = markerScreenPos();
    const double rad = marker_.heading_deg * geo::kDegToRad;
    // Compass -> screen: north is -y, east is +x.
    return base + QPointF(std::sin(rad), -std::cos(rad)) * kMarkerArrowPx;
}

// ---- Hit testing ------------------------------------------------------------

SatelliteMapWidget::Drag SatelliteMapWidget::hitTest(const QPointF& pos,
                                                     int* corner_index,
                                                     int* edge_index) const {
    if (corner_index) {
        *corner_index = -1;
    }
    if (edge_index) {
        *edge_index = -1;
    }
    if (edit_locked_) {
        return Drag::Pan;
    }
    if (marker_.valid) {
        if (QLineF(pos, markerArrowTipScreen()).length() <= kHitRadiusPx) {
            return Drag::RotateMarker;
        }
        if (QLineF(pos, markerScreenPos()).length() <=
            kMarkerRadiusPx + 4.0) {
            return Drag::MoveMarker;
        }
    }
    const bool have_poly = polygon_.valid() || roi_.valid;
    if (have_poly) {
        if (!polygon_.valid() && roi_.valid) {
            if (QLineF(pos, roiRotateHandleScreen()).length() <= kHitRadiusPx) {
                return Drag::RotateRoi;
            }
        }
        for (int i = 0; i < dim_boxes_.size(); ++i) {
            if (dim_boxes_[i].contains(pos)) {
                if (edge_index) {
                    *edge_index = i;
                }
                return Drag::DimBadge;
            }
        }
        const QVector<QPointF> corners = polygon_.valid()
                                            ? polygonScreenPoints()
                                            : roiCornerScreenPoints();
        for (int i = 0; i < corners.size(); ++i) {
            if (QLineF(pos, corners[i]).length() <= kHitRadiusPx) {
                if (corner_index) {
                    *corner_index = i;
                }
                return polygon_.valid() ? Drag::MoveVertex : Drag::ResizeRoiCorner;
            }
        }
        for (int i = 0; i < corners.size(); ++i) {
            const QLineF edge(corners[i], corners[(i + 1) % corners.size()]);
            const QPointF ab = edge.p2() - edge.p1();
            const double len_sq = QPointF::dotProduct(ab, ab);
            if (len_sq < 1.0) {
                continue;
            }
            const double t = qBound(
                0.0, QPointF::dotProduct(pos - edge.p1(), ab) / len_sq, 1.0);
            const QPointF closest = edge.p1() + ab * t;
            if (QLineF(pos, closest).length() <= 6.0) {
                if (edge_index) {
                    *edge_index = i;
                }
                return Drag::EdgeTogglePending;
            }
        }
        QPainterPath path;
        path.addPolygon(QPolygonF(corners));
        if (path.contains(pos)) {
            return Drag::MoveRoi;
        }
    }
    return Drag::Pan;
}

geo::GeoPoint SatelliteMapWidget::maybeSnap(const geo::GeoPoint& point) const {
    if (imagery_enabled_) {
        return point;  // snapping against ±5 m imagery is meaningless
    }
    const geo::GeoPoint origin{0.0, 0.0};
    const QPointF enu = geo::enuFromGeo(origin, point);
    constexpr double kSnapM = 0.1;
    return geo::geoFromEnu(origin, std::round(enu.x() / kSnapM) * kSnapM,
                           std::round(enu.y() / kSnapM) * kSnapM);
}

void SatelliteMapWidget::updateCursorShape(const QPointF& pos) {
    if (place_marker_armed_) {
        setCursor(Qt::CrossCursor);
        return;
    }
    switch (hitTest(pos, nullptr)) {
        case Drag::RotateMarker:
        case Drag::RotateRoi:
        case Drag::EdgeTogglePending:
            setCursor(Qt::PointingHandCursor);
            break;
        case Drag::ResizeRoiCorner:
            setCursor(Qt::SizeAllCursor);
            break;
        case Drag::MoveRoi:
        case Drag::MoveMarker:
            setCursor(Qt::SizeAllCursor);
            break;
        default:
            setCursor(drag_ == Drag::Pan ? Qt::ClosedHandCursor
                                         : Qt::OpenHandCursor);
            break;
    }
}

// ---- Painting ---------------------------------------------------------------

void SatelliteMapWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(rect(), QColor(0x0b, 0x0b, 0x0b));

    paintTiles(painter);
    if (mission_anchor_.valid) {
        paintTelemetry(painter);
    }
    paintRoi(painter);
    paintMarker(painter);
    paintChrome(painter);
}

void SatelliteMapWidget::paintTiles(QPainter& painter) {
    if (!imagery_enabled_) {
        paintGrid(painter);
        return;
    }
    const int n = 1 << zoom_;
    const double world_px = double(kTileSize) * n;
    const double left = center_nx_ * world_px - width() / 2.0;
    const double top = center_ny_ * world_px - height() / 2.0;

    const int tx0 = int(std::floor(left / kTileSize));
    const int ty0 = int(std::floor(top / kTileSize));
    const int tx1 = int(std::floor((left + width()) / kTileSize));
    const int ty1 = int(std::floor((top + height()) / kTileSize));

    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (int ty = ty0; ty <= ty1; ++ty) {
        if (ty < 0 || ty >= n) {
            continue;
        }
        for (int tx = tx0; tx <= tx1; ++tx) {
            const int wrapped = ((tx % n) + n) % n;
            const QPointF dest(tx * double(kTileSize) - left,
                               ty * double(kTileSize) - top);
            const QPixmap tile = tiles_->cachedTile(zoom_, wrapped, ty);
            if (!tile.isNull()) {
                painter.drawPixmap(dest, tile);
                continue;
            }
            // Overzoom / not-yet-fetched fallback: draw the matching
            // sub-rect of the nearest cached ancestor scaled up, so the
            // map never blanks past the imagery's native LOD.
            //
            // Two very different cases reach here. A tile that simply hasn't
            // arrived yet is transient — the ancestor is a loading placeholder
            // and will be replaced in a moment. A tile the service has already
            // 404'd is PERMANENT: this LOD has no data here, and because World
            // Imagery is a mosaic of contributor layers the ancestor may be a
            // different flight from a different year. Marking the second case
            // is the point: otherwise the operator can draw an ROI spanning
            // two epochs with nothing on screen to say so.
            const bool permanent_substitute =
                tiles_->isFailedRecently(zoom_, wrapped, ty);
            bool drew_fallback = false;
            for (int up = 1; up <= 7 && zoom_ - up >= kMinZoom; ++up) {
                const int az = zoom_ - up;
                const int ax = wrapped >> up;
                const int ay = ty >> up;
                const QPixmap ancestor = tiles_->cachedTile(az, ax, ay);
                if (ancestor.isNull()) {
                    continue;
                }
                const int sub = kTileSize >> up;
                const QRectF source((wrapped - (ax << up)) * sub,
                                    (ty - (ay << up)) * sub, sub, sub);
                painter.drawPixmap(
                    QRectF(dest, QSizeF(kTileSize, kTileSize)), ancestor,
                    source);
                drew_fallback = true;
                break;
            }
            if (drew_fallback && permanent_substitute) {
                // Light diagonal hatch: readable over both bright roofs and
                // dark asphalt without obscuring what's underneath.
                painter.save();
                painter.setPen(Qt::NoPen);
                QBrush hatch(QColor(255, 255, 255, 28), Qt::BDiagPattern);
                painter.setBrush(hatch);
                painter.drawRect(QRectF(dest, QSizeF(kTileSize, kTileSize)));
                painter.restore();
            }
            if (!drew_fallback) {
                painter.fillRect(QRectF(dest, QSizeF(kTileSize, kTileSize)),
                                 QColor(0x14, 0x17, 0x1b));
                painter.setPen(QColor(0x22, 0x27, 0x2d));
                painter.drawRect(
                    QRectF(dest, QSizeF(kTileSize - 1, kTileSize - 1)));
            }
            tiles_->fetch(zoom_, wrapped, ty);
        }
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
}

void SatelliteMapWidget::setMapRaster(const QImage& image,
                                      const QRectF& bounds_m) {
    map_raster_ = image;
    map_raster_m_ = bounds_m;
    update();
}

void SatelliteMapWidget::clearMapRaster() {
    map_raster_ = QImage();
    map_raster_m_ = QRectF();
    update();
}

void SatelliteMapWidget::paintMapRaster(QPainter& painter) {
    if (map_raster_.isNull() || map_raster_m_.isEmpty()) {
        return;
    }
    // Raster row 0 is max northing, so the image's top-left corner is the
    // NW corner of the extent: (min_x, max_y). ENU axes align with screen
    // axes in this projection, so an axis-aligned blit is exact.
    const geo::GeoPoint origin{0.0, 0.0};
    const QPointF nw = screenFromGeo(geo::geoFromEnu(
        origin, map_raster_m_.left(), map_raster_m_.bottom()));
    const QPointF se = screenFromGeo(geo::geoFromEnu(
        origin, map_raster_m_.right(), map_raster_m_.top()));
    painter.drawImage(QRectF(nw, se).normalized(), map_raster_);
}

void SatelliteMapWidget::paintGrid(QPainter& painter) {
    // Measured (CAD) canvas: adaptive metric grid on a dark drafting
    // surface. Minor lines pick the smallest step that stays >= 24 px on
    // screen; major lines every 5 minors carry meter labels.
    painter.fillRect(rect(), QColor(0x10, 0x10, 0x14));
    // Under the grid, not over it: the grid lines are what the operator
    // measures against, and the cloud's transparent gaps let them read
    // through anyway.
    paintMapRaster(painter);
    const double mpp = metersPerPixelNow();

    double minor_m = 0.1;
    const double steps[] = {0.1, 0.5, 1.0, 5.0, 10.0, 50.0, 100.0, 500.0};
    for (double step : steps) {
        if (step / mpp >= 24.0) {
            minor_m = step;
            break;
        }
        minor_m = step;
    }
    const double major_m = minor_m * 5.0;

    // Visible ENU window around the grid origin (the measured anchor).
    const geo::GeoPoint origin{0.0, 0.0};
    const QPointF enu_tl =
        geo::enuFromGeo(origin, geoFromScreen(QPointF(0, 0)));
    const QPointF enu_br = geo::enuFromGeo(
        origin, geoFromScreen(QPointF(width(), height())));
    const double e_min = std::min(enu_tl.x(), enu_br.x());
    const double e_max = std::max(enu_tl.x(), enu_br.x());
    const double n_min = std::min(enu_tl.y(), enu_br.y());
    const double n_max = std::max(enu_tl.y(), enu_br.y());

    painter.setRenderHint(QPainter::Antialiasing, false);
    const QColor minor_color(255, 255, 255, 13);
    const QColor major_color(255, 255, 255, 26);
    for (double e = std::floor(e_min / minor_m) * minor_m; e <= e_max;
         e += minor_m) {
        const bool major =
            std::abs(std::remainder(e, major_m)) < minor_m * 0.25;
        const QPointF top =
            screenFromGeo(geo::geoFromEnu(origin, e, n_max));
        const QPointF bottom =
            screenFromGeo(geo::geoFromEnu(origin, e, n_min));
        painter.setPen(QPen(major ? major_color : minor_color, 1));
        painter.drawLine(top, bottom);
    }
    for (double n = std::floor(n_min / minor_m) * minor_m; n <= n_max;
         n += minor_m) {
        const bool major =
            std::abs(std::remainder(n, major_m)) < minor_m * 0.25;
        const QPointF left =
            screenFromGeo(geo::geoFromEnu(origin, e_min, n));
        const QPointF right =
            screenFromGeo(geo::geoFromEnu(origin, e_max, n));
        painter.setPen(QPen(major ? major_color : minor_color, 1));
        painter.drawLine(left, right);
    }
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Robot-frame axes once the marker is placed: the operator's reminder
    // that the drawn world is anchored to the robot at FAST-LIO init.
    if (marker_.valid) {
        const QPointF base = markerScreenPos();
        const double rad = marker_.heading_deg * geo::kDegToRad;
        const QPointF fwd(std::sin(rad), -std::cos(rad));
        const QPointF left(-fwd.y(), fwd.x());
        QColor axis(0x00, 0xBC, 0x7D, 70);
        painter.setPen(QPen(axis, 1, Qt::DashLine));
        painter.drawLine(base - fwd * 2000.0, base + fwd * 2000.0);
        axis.setAlpha(40);
        painter.setPen(QPen(axis, 1, Qt::DashLine));
        painter.drawLine(base - left * 2000.0, base + left * 2000.0);
    }
}

void SatelliteMapWidget::paintTelemetry(QPainter& painter) {
    const double mpp = metersPerPixelNow();
    const double heading_rad = mission_anchor_.heading_deg * geo::kDegToRad;
    const double s = std::sin(heading_rad);
    const double c = std::cos(heading_rad);

    // Occupancy grid: image u axis = +x body, v axis = +y body.
    if (!grid_.image.isNull()) {
        const double k = grid_.resolution / mpp;
        // body x axis (E,N) = (s, c) -> screen (s, -c); body y = (-c, s) -> (-c, -s)
        QTransform t(k * s, -k * c,   // image u axis in screen coords
                     -k * c, -k * s,  // image v axis in screen coords
                     0.0, 0.0);
        const QPointF origin_screen = screenFromBody(grid_.origin_body);
        t *= QTransform::fromTranslate(origin_screen.x(), origin_screen.y());
        painter.save();
        painter.setTransform(t, false);
        painter.drawImage(QPointF(0, 0), grid_.image);
        painter.restore();
    }

    // Planned swaths (fine lines beneath the connector path).
    auto drawLines = [&](const PolylineSet& set, double width_px,
                         double alpha) {
        for (int i = 0; i < set.lines.size(); ++i) {
            const QVector<QPointF>& line = set.lines[i];
            if (line.size() < 2) {
                continue;
            }
            QColor color = i < set.colors.size() ? set.colors[i]
                                                 : satpal::info();
            color.setAlphaF(alpha);
            painter.setPen(QPen(color, width_px));
            QPolygonF poly;
            poly.reserve(line.size());
            for (const QPointF& body : line) {
                poly.append(screenFromBody(body));
            }
            painter.drawPolyline(poly);
        }
    };
    drawLines(swaths_, 1.6, 0.85);
    drawLines(path_, 2.4, 0.95);

    // Odometry breadcrumb trail + live robot pose.
    if (trail_.size() >= 2) {
        QColor trail_color = satpal::accent();
        trail_color.setAlphaF(0.65);
        painter.setPen(QPen(trail_color, 2.0));
        QPolygonF poly;
        poly.reserve(trail_.size());
        for (const QPointF& body : trail_) {
            poly.append(screenFromBody(body));
        }
        painter.drawPolyline(poly);
    }
    if (odom_.valid) {
        const QPointF pos = screenFromBody(QPointF(odom_.x, odom_.y));
        const QPointF nose = screenFromBody(
            QPointF(odom_.x + 0.6 * std::cos(odom_.yaw),
                    odom_.y + 0.6 * std::sin(odom_.yaw)));
        painter.setBrush(satpal::accent());
        painter.setPen(QPen(Qt::white, 1.5));
        painter.drawEllipse(pos, 7.0, 7.0);
        painter.setPen(QPen(Qt::white, 2.5));
        painter.drawLine(pos, nose);
        painter.setBrush(Qt::NoBrush);
    }
}

void SatelliteMapWidget::paintRoi(QPainter& painter) {
    const QVector<QPointF> corners = polygon_.valid()
                                        ? polygonScreenPoints()
                                        : roiCornerScreenPoints();
    const int n = corners.size();
    if (n < 3 && !roi_.valid) {
        return;
    }
    if (n < 3) {
        return;
    }
    const QPolygonF poly(corners);

    QColor fill = satpal::accent();
    fill.setAlphaF(edit_locked_ ? 0.06 : 0.12);
    QColor edge = satpal::accent();

    painter.setBrush(fill);
    painter.setPen(Qt::NoPen);
    painter.drawPolygon(poly);
    painter.setBrush(Qt::NoBrush);

    for (int i = 0; i < n; ++i) {
        const bool marked =
            i < polygon_.roof_edges.size() ? polygon_.roof_edges[i]
            : (roi_.valid && i < 4 ? roi_.roof_edges[size_t(i)] : false);
        if (marked) {
            painter.setPen(
                QPen(satpal::danger(), 3.0, Qt::SolidLine, Qt::RoundCap));
        } else {
            painter.setPen(QPen(edge, 2.0,
                                edit_locked_ ? Qt::SolidLine : Qt::DashLine,
                                Qt::RoundCap));
        }
        painter.drawLine(corners[i], corners[(i + 1) % n]);
    }

    if (!edit_locked_ && roi_.valid && !polygon_.valid()) {
        const QPointF fwd_mid = (corners[0] + corners[3]) / 2.0;
        const QPointF rot_handle = roiRotateHandleScreen();
        painter.setPen(QPen(edge, 1.5, Qt::DotLine));
        painter.drawLine(fwd_mid, rot_handle);
        painter.setBrush(satpal::cardBg());
        painter.setPen(QPen(edge, 2.0));
        painter.drawEllipse(rot_handle, kHandleRadiusPx, kHandleRadiusPx);
        Q_UNUSED(fwd_mid);
    }

    if (!edit_locked_) {
        painter.setBrush(satpal::cardBg());
        painter.setPen(QPen(edge, 2.0));
        for (const QPointF& corner : corners) {
            painter.drawEllipse(corner, kHandleRadiusPx, kHandleRadiusPx);
        }
        painter.setBrush(Qt::NoBrush);
    }

    QFont dim_font = font();
    dim_font.setPointSizeF(10.0);
    dim_font.setBold(true);
    painter.setFont(dim_font);
    dim_boxes_.fill(QRectF(), n);
    dim_boxes_.resize(n);
    for (int i = 0; i < n; ++i) {
        const geo::GeoPoint a =
            polygon_.valid() ? polygon_.vertices[i]
                             : roi_.corners()[i];
        const geo::GeoPoint b =
            polygon_.valid() ? polygon_.vertices[(i + 1) % n]
                             : roi_.corners()[(i + 1) % 4];
        const QPointF enu = geo::enuFromGeo(a, b);
        const double len = std::hypot(enu.x(), enu.y());
        const QString text = units::formatLength(len, 1);
        const QPointF at = (corners[i] + corners[(i + 1) % n]) / 2.0;
        const QFontMetricsF fm(dim_font);
        const QRectF box(at.x() - fm.horizontalAdvance(text) / 2.0 - 6,
                         at.y() - fm.height() / 2.0 - 3,
                         fm.horizontalAdvance(text) + 12, fm.height() + 6);
        dim_boxes_[i] = box;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 170));
        painter.drawRoundedRect(box, 5, 5);
        painter.setPen(satpal::text());
        painter.drawText(box, Qt::AlignCenter, text);
    }
}

void SatelliteMapWidget::paintMarker(QPainter& painter) {
    if (!marker_.valid) {
        return;
    }
    const QPointF pos = markerScreenPos();
    const QPointF tip = markerArrowTipScreen();

    QColor body = satpal::warning();
    painter.setPen(QPen(body, 2.5));
    painter.drawLine(pos, tip);
    // Arrow head.
    const QLineF shaft(pos, tip);
    const QLineF left = QLineF(tip, pos).normalVector();
    QLineF head1(tip, tip);
    head1.setAngle(shaft.angle() + 150);
    head1.setLength(9);
    QLineF head2(tip, tip);
    head2.setAngle(shaft.angle() - 150);
    head2.setLength(9);
    painter.drawLine(head1);
    painter.drawLine(head2);
    Q_UNUSED(left);

    painter.setBrush(body);
    painter.setPen(QPen(Qt::black, 1.5));
    painter.drawEllipse(pos, kMarkerRadiusPx, kMarkerRadiusPx);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::black, 2.0));
    painter.drawPoint(pos);

    QFont label_font = font();
    label_font.setPointSizeF(9.0);
    label_font.setBold(true);
    painter.setFont(label_font);
    painter.setPen(satpal::text());
    painter.drawText(
        QRectF(pos.x() - 60, pos.y() + kMarkerRadiusPx + 4, 120, 16),
        Qt::AlignHCenter,
        QStringLiteral("ROBOT %1°").arg(marker_.heading_deg, 0, 'f', 0));
}

void SatelliteMapWidget::paintChrome(QPainter& painter) {
    // Scale bar (bottom-left) — steps chosen in the operator's display
    // unit so the label reads as a round number in either system.
    const double mpp = metersPerPixelNow();
    const bool metric = UnitsProvider::instance()->isMetric();
    const double unit_m = metric ? 1.0 : 1.0 / units::kFeetPerMeter;
    const double target_units = 100.0 * mpp / unit_m;
    const double steps[] = {0.5, 1, 2,  5,   10,  20,   50,  100,
                            200, 500, 1000, 2000, 5000, 10000};
    double chosen = steps[0];
    for (double step : steps) {
        if (step <= target_units) {
            chosen = step;
        }
    }
    const double bar_px = chosen * unit_m / mpp;
    const QPointF base(14, height() - 18);
    painter.setPen(QPen(satpal::text(), 2));
    painter.drawLine(base, base + QPointF(bar_px, 0));
    painter.drawLine(base + QPointF(0, -4), base + QPointF(0, 4));
    painter.drawLine(base + QPointF(bar_px, -4), base + QPointF(bar_px, 4));
    QFont small = font();
    small.setPointSizeF(9.0);
    painter.setFont(small);
    QString label;
    if (metric) {
        label = chosen >= 1000.0
                    ? QStringLiteral("%1 km").arg(chosen / 1000.0)
                    : QStringLiteral("%1 m").arg(chosen);
    } else {
        label = QStringLiteral("%1 ft").arg(chosen);
    }
    painter.drawText(QRectF(base.x(), base.y() - 20, bar_px, 14),
                     Qt::AlignCenter, label);

    // Attribution (ToS requirement, bottom-right; imagery surfaces only).
    if (!imagery_enabled_) {
        return;
    }
    const QString attribution = QLatin1String(kAttribution);
    QFont attr_font = font();
    attr_font.setPointSizeF(8.0);
    painter.setFont(attr_font);
    const QFontMetrics fm(attr_font);
    const int text_w = fm.horizontalAdvance(attribution) + 12;
    const int text_h = fm.height() + 4;
    const QRect attr_rect(width() - text_w, height() - text_h, text_w, text_h);
    painter.fillRect(attr_rect, QColor(0, 0, 0, 150));
    painter.setPen(QColor(220, 220, 220));
    painter.drawText(attr_rect, Qt::AlignCenter, attribution);
}

// ---- Interaction ------------------------------------------------------------

void SatelliteMapWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton && draw_polygon_armed_) {
        draw_polygon_armed_ = false;
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (draw_polygon_armed_ && !edit_locked_) {
        polygon_.vertices.append(maybeSnap(geoFromScreen(event->pos())));
        polygon_.ensureEdgeFlags();
        update();
        emit roiChanged();
        return;
    }
    if (place_marker_armed_ && !edit_locked_) {
        const geo::GeoPoint point = maybeSnap(geoFromScreen(event->pos()));
        marker_.lat = point.lat;
        marker_.lon = point.lon;
        if (!marker_.valid && roi_.valid) {
            marker_.heading_deg = roi_.heading_deg;
        }
        marker_.valid = true;
        place_marker_armed_ = false;
        setCursor(Qt::OpenHandCursor);
        update();
        emit markerChanged();
        return;
    }
    drag_ = hitTest(event->pos(), &drag_corner_, &drag_edge_);
    drag_press_pos_ = event->pos();
    drag_last_ = event->pos();
    updateCursorShape(event->pos());
}

void SatelliteMapWidget::mouseMoveEvent(QMouseEvent* event) {
    if (drag_ == Drag::None) {
        updateCursorShape(event->pos());
        return;
    }
    const QPointF pos = event->pos();
    const QPoint delta = event->pos() - drag_last_;
    drag_last_ = event->pos();
    const double mpp = metersPerPixelNow();

    switch (drag_) {
        case Drag::Pan: {
            const double world_px = double(kTileSize) * (1 << zoom_);
            center_nx_ -= delta.x() / world_px;
            center_ny_ -= delta.y() / world_px;
            clampCenter();
            emitViewChanged();
            break;
        }
        case Drag::MoveRoi: {
            const QPointF enu(delta.x() * mpp, -delta.y() * mpp);
            if (polygon_.valid()) {
                for (geo::GeoPoint& p : polygon_.vertices) {
                    p = geo::geoFromEnu(p, enu.x(), enu.y());
                }
            } else {
                roi_.center =
                    maybeSnap(geo::geoFromEnu(roi_.center, enu.x(), enu.y()));
            }
            emit roiChanged();
            break;
        }
        case Drag::MoveMarker: {
            const QPointF enu(delta.x() * mpp, -delta.y() * mpp);
            const geo::GeoPoint moved = maybeSnap(geo::geoFromEnu(
                geo::GeoPoint{marker_.lat, marker_.lon}, enu.x(), enu.y()));
            marker_.lat = moved.lat;
            marker_.lon = moved.lon;
            emit markerChanged();
            break;
        }
        case Drag::EdgeTogglePending:
            // Toggles on release; a real drag from an edge does nothing.
            break;
        case Drag::RotateMarker: {
            const QPointF base = markerScreenPos();
            const QPointF v = pos - base;
            marker_.heading_deg =
                compassFromEnuVector(QPointF(v.x(), -v.y()));
            emit markerChanged();
            break;
        }
        case Drag::RotateRoi: {
            const QPointF base = screenFromGeo(roi_.center);
            const QPointF v = pos - base;
            roi_.heading_deg = compassFromEnuVector(QPointF(v.x(), -v.y()));
            emit roiChanged();
            break;
        }
        case Drag::MoveVertex: {
            if (drag_corner_ >= 0 && drag_corner_ < polygon_.vertices.size()) {
                polygon_.vertices[drag_corner_] =
                    maybeSnap(geoFromScreen(pos));
                emit roiChanged();
            }
            break;
        }
        case Drag::ResizeRoiCorner: {
            const QPointF base = screenFromGeo(roi_.center);
            const QPointF v_screen = pos - base;
            const QPointF v_enu(v_screen.x() * mpp, -v_screen.y() * mpp);
            const double s = std::sin(roi_.heading_deg * geo::kDegToRad);
            const double c = std::cos(roi_.heading_deg * geo::kDegToRad);
            const double along = v_enu.x() * s + v_enu.y() * c;
            const double across = v_enu.x() * c - v_enu.y() * s;
            roi_.length_m = qBound(2.0, std::abs(along) * 2.0, 500.0);
            roi_.width_m = qBound(2.0, std::abs(across) * 2.0, 500.0);
            polygon_ = RoiPolygon::fromRect(roi_);
            emit roiChanged();
            break;
        }
        default:
            break;
    }
    update();
}

void SatelliteMapWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (drag_ == Drag::EdgeTogglePending && drag_edge_ >= 0 &&
            (event->pos() - drag_press_pos_).manhattanLength() <= 4) {
            if (polygon_.valid()) {
                polygon_.ensureEdgeFlags();
                if (drag_edge_ < polygon_.roof_edges.size()) {
                    polygon_.roof_edges[drag_edge_] =
                        !polygon_.roof_edges[drag_edge_];
                }
            } else if (drag_edge_ < 4) {
                roi_.roof_edges[size_t(drag_edge_)] =
                    !roi_.roof_edges[size_t(drag_edge_)];
            }
            update();
            emit roiChanged();
        }
        if (drag_ == Drag::DimBadge && drag_edge_ >= 0 &&
            (event->pos() - drag_press_pos_).manhattanLength() <= 4) {
            const int n = polygon_.valid() ? polygon_.vertices.size() : 4;
            if (drag_edge_ < n) {
                geo::GeoPoint a, b;
                if (polygon_.valid()) {
                    a = polygon_.vertices[drag_edge_];
                    b = polygon_.vertices[(drag_edge_ + 1) % n];
                } else {
                    const auto c = roi_.corners();
                    a = c[drag_edge_];
                    b = c[(drag_edge_ + 1) % 4];
                }
                const QPointF enu = geo::enuFromGeo(a, b);
                const double cur = std::hypot(enu.x(), enu.y());
                bool ok = false;
                const QString prompt =
                    UnitsProvider::instance()->isMetric()
                        ? QStringLiteral("Edge length (m)")
                        : QStringLiteral("Edge length (ft)");
                const double shown = UnitsProvider::instance()->isMetric()
                                        ? cur
                                        : units::metersToFeet(cur);
                const double entered = QInputDialog::getDouble(
                    this, QStringLiteral("Edge length"), prompt, shown, 0.5,
                    500.0, 2, &ok);
                if (ok) {
                    const double meters = UnitsProvider::instance()->isMetric()
                                             ? entered
                                             : units::feetToMeters(entered);
                    if (polygon_.valid()) {
                        setEdgeLength(drag_edge_, meters);
                    } else {
                        polygon_ = RoiPolygon::fromRect(roi_);
                        if (setEdgeLength(drag_edge_, meters)) {
                            roi_.valid = false;
                        }
                    }
                }
            }
        }
        drag_ = Drag::None;
        drag_corner_ = -1;
        drag_edge_ = -1;
        updateCursorShape(event->pos());
    }
}

void SatelliteMapWidget::wheelEvent(QWheelEvent* event) {
    const int dz = event->angleDelta().y() > 0 ? 1 : -1;
    const int new_zoom = qBound(kMinZoom, zoom_ + dz, maxZoomNow());
    if (new_zoom == zoom_) {
        return;
    }
    const QPointF pos = event->position();
    const double world_before = double(kTileSize) * (1 << zoom_);
    const double nx_under =
        center_nx_ + (pos.x() - width() / 2.0) / world_before;
    const double ny_under =
        center_ny_ + (pos.y() - height() / 2.0) / world_before;

    zoom_ = new_zoom;
    const double world_after = double(kTileSize) * (1 << zoom_);
    center_nx_ = nx_under - (pos.x() - width() / 2.0) / world_after;
    center_ny_ = ny_under - (pos.y() - height() / 2.0) / world_after;

    clampCenter();
    update();
    emitViewChanged();
}

}  // namespace f2c_cpp
