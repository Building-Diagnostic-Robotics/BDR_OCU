#include "satellite_map_widget.hpp"

#include "satellite_palette.hpp"
#include "satellite_tile_service.hpp"

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

void SatelliteMapWidget::setView(double lat, double lon, int zoom) {
    center_nx_ = geo::lonToNormX(lon);
    center_ny_ = geo::latToNormY(lat);
    zoom_ = qBound(kMinZoom, zoom, kMaxZoom);
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
    update();
}

void SatelliteMapWidget::addRoiAtViewCenter() {
    roi_.valid = true;
    roi_.center = geo::GeoPoint{centerLat(), centerLon()};
    if (roi_.length_m <= 0.0) roi_.length_m = 20.0;
    if (roi_.width_m <= 0.0) roi_.width_m = 15.0;
    if (marker_.valid) {
        roi_.heading_deg = marker_.heading_deg;
    }
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
                                                     int* corner_index) const {
    if (corner_index) {
        *corner_index = -1;
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
    if (roi_.valid) {
        if (QLineF(pos, roiRotateHandleScreen()).length() <= kHitRadiusPx) {
            return Drag::RotateRoi;
        }
        const QVector<QPointF> corners = roiCornerScreenPoints();
        for (int i = 0; i < corners.size(); ++i) {
            if (QLineF(pos, corners[i]).length() <= kHitRadiusPx) {
                if (corner_index) {
                    *corner_index = i;
                }
                return Drag::ResizeRoiCorner;
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

void SatelliteMapWidget::updateCursorShape(const QPointF& pos) {
    if (place_marker_armed_) {
        setCursor(Qt::CrossCursor);
        return;
    }
    switch (hitTest(pos, nullptr)) {
        case Drag::RotateMarker:
        case Drag::RotateRoi:
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
    if (!roi_.valid) {
        return;
    }
    const QVector<QPointF> corners = roiCornerScreenPoints();
    if (corners.size() != 4) {
        return;
    }
    const QPolygonF poly(corners);

    QColor fill = satpal::accent();
    fill.setAlphaF(edit_locked_ ? 0.06 : 0.12);
    QColor edge = satpal::accent();

    painter.setBrush(fill);
    painter.setPen(QPen(edge, 2.0, edit_locked_ ? Qt::SolidLine : Qt::DashLine));
    painter.drawPolygon(poly);
    painter.setBrush(Qt::NoBrush);

    // Heading tick on the forward edge (midpoint corners[0]..corners[3]).
    const QPointF fwd_mid = (corners[0] + corners[3]) / 2.0;
    const QPointF rot_handle = roiRotateHandleScreen();
    if (!edit_locked_) {
        painter.setPen(QPen(edge, 1.5, Qt::DotLine));
        painter.drawLine(fwd_mid, rot_handle);
        painter.setBrush(satpal::cardBg());
        painter.setPen(QPen(edge, 2.0));
        painter.drawEllipse(rot_handle, kHandleRadiusPx, kHandleRadiusPx);

        painter.setBrush(satpal::cardBg());
        for (const QPointF& corner : corners) {
            painter.drawRect(QRectF(corner.x() - kHandleRadiusPx + 1,
                                    corner.y() - kHandleRadiusPx + 1,
                                    2 * kHandleRadiusPx - 2,
                                    2 * kHandleRadiusPx - 2));
        }
        painter.setBrush(Qt::NoBrush);
    }

    // Dimension labels on the two edge midpoints.
    QFont dim_font = font();
    dim_font.setPointSizeF(10.0);
    dim_font.setBold(true);
    painter.setFont(dim_font);
    const auto drawDim = [&](const QPointF& at, const QString& text) {
        const QFontMetricsF fm(dim_font);
        const QRectF box(at.x() - fm.horizontalAdvance(text) / 2.0 - 6,
                         at.y() - fm.height() / 2.0 - 3,
                         fm.horizontalAdvance(text) + 12, fm.height() + 6);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 170));
        painter.drawRoundedRect(box, 5, 5);
        painter.setPen(satpal::text());
        painter.drawText(box, Qt::AlignCenter, text);
    };
    // Length label on a side edge (corners[0]-[1]); width on forward edge.
    drawDim((corners[0] + corners[1]) / 2.0,
            QStringLiteral("%1 m").arg(roi_.length_m, 0, 'f', 1));
    drawDim((corners[3] + corners[0]) / 2.0,
            QStringLiteral("%1 m").arg(roi_.width_m, 0, 'f', 1));
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
    // Scale bar (bottom-left).
    const double mpp = metersPerPixelNow();
    double target_m = 100.0 * mpp;
    const double steps[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
    double chosen = steps[0];
    for (double step : steps) {
        if (step <= target_m) {
            chosen = step;
        }
    }
    const double bar_px = chosen / mpp;
    const QPointF base(14, height() - 18);
    painter.setPen(QPen(satpal::text(), 2));
    painter.drawLine(base, base + QPointF(bar_px, 0));
    painter.drawLine(base + QPointF(0, -4), base + QPointF(0, 4));
    painter.drawLine(base + QPointF(bar_px, -4), base + QPointF(bar_px, 4));
    QFont small = font();
    small.setPointSizeF(9.0);
    painter.setFont(small);
    painter.drawText(QRectF(base.x(), base.y() - 20, bar_px, 14),
                     Qt::AlignCenter,
                     chosen >= 1000.0
                         ? QStringLiteral("%1 km").arg(chosen / 1000.0)
                         : QStringLiteral("%1 m").arg(chosen));

    // Attribution (ToS requirement, bottom-right).
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
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (place_marker_armed_ && !edit_locked_) {
        const geo::GeoPoint point = geoFromScreen(event->pos());
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
    drag_ = hitTest(event->pos(), &drag_corner_);
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
            roi_.center = geo::geoFromEnu(roi_.center, enu.x(), enu.y());
            emit roiChanged();
            break;
        }
        case Drag::MoveMarker: {
            const QPointF enu(delta.x() * mpp, -delta.y() * mpp);
            const geo::GeoPoint moved = geo::geoFromEnu(
                geo::GeoPoint{marker_.lat, marker_.lon}, enu.x(), enu.y());
            marker_.lat = moved.lat;
            marker_.lon = moved.lon;
            emit markerChanged();
            break;
        }
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
        case Drag::ResizeRoiCorner: {
            // Center-fixed resize: half extents follow the cursor projection
            // onto the ROI's own axes.
            const QPointF base = screenFromGeo(roi_.center);
            const QPointF v_screen = pos - base;
            const QPointF v_enu(v_screen.x() * mpp, -v_screen.y() * mpp);
            const double s = std::sin(roi_.heading_deg * geo::kDegToRad);
            const double c = std::cos(roi_.heading_deg * geo::kDegToRad);
            const double along = v_enu.x() * s + v_enu.y() * c;
            const double across = v_enu.x() * c - v_enu.y() * s;
            roi_.length_m = qBound(2.0, std::abs(along) * 2.0, 500.0);
            roi_.width_m = qBound(2.0, std::abs(across) * 2.0, 500.0);
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
        drag_ = Drag::None;
        drag_corner_ = -1;
        updateCursorShape(event->pos());
    }
}

void SatelliteMapWidget::wheelEvent(QWheelEvent* event) {
    const int dz = event->angleDelta().y() > 0 ? 1 : -1;
    const int new_zoom = qBound(kMinZoom, zoom_ + dz, kMaxZoom);
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
