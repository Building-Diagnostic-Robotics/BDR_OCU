#include "planner_screen.hpp"

#include "components/auto_hide_scroll_bar.hpp"
#include "components/bdr_input_dialog.hpp"
#include "components/bdr_message_box.hpp"
#include "components/fpv_camera_view.hpp"
#include "components/pre_scan_checklist_dialog.hpp"
#include "components/svg_icon_button.hpp"
#include "coverage_gui.hpp"
#include "settings_constants.hpp"

#include <algorithm>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDoubleValidator>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsBlurEffect>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QShowEvent>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QtConcurrent>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <cstdlib>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QSvgRenderer>
#else
#include <QtSvg/QSvgRenderer>
#endif

namespace f2c_cpp {

namespace {

constexpr size_t kPreviewTargetPointCount = 180000;
constexpr double kVoxelSizeMin = 0.01;
constexpr double kVoxelSizeMax = 0.20;
constexpr double kZFilterMin = -0.50;
constexpr double kZFilterMax = 0.50;
constexpr double kHullAlphaMin = 0.10;
constexpr double kHullAlphaMax = 3.00;
constexpr double kCoveragePathSpacingMin = 0.20;
constexpr double kCoveragePathSpacingMax = 1.00;
constexpr double kCoverageHeadlandMin = 0.10;
constexpr double kCoverageHeadlandMax = 1.00;
// Robot cruise-speed slider envelope. 0.1 m/s steps. The hard ceiling
// matches the working MPC tune envelope on the platform — controller
// guards against exceeding this internally too.
constexpr double kCoverageScanSpeedMin = 0.30;
constexpr double kCoverageScanSpeedMax = 0.60;
constexpr double kCoverageScanSpeedStep = 0.10;
constexpr double kCoverageScanSpeedDefault = 0.40;
// Pre-planning waypoint-count estimate spacing for the metrics card only.
// The actual coverage pipeline runs with `cfg.waypoint_spacing = 0.0`
// (resampling disabled) so axial-turn corners are preserved exactly.
constexpr double kCoveragePathEstimateSpacingMeters = 0.10;
constexpr int kPreviewOverlayMargin = 16;
constexpr int kPlannerLeftRailWidth = 384;
constexpr int kPlannerScanLeftRailWidth = 320;
constexpr int kPlannerRightRailWidth = 380;
constexpr int kStageRowWidth = 668;

// BDR_REWIRE: Dev bypass — temporarily allow the operator to navigate into
// Stage 3 (Scan Splitting) and Stage 4 (Scan) even when there is no saved map,
// no completed coverage plan, or no published waypoints. Lets the UI be poked
// at offline without a robot. Flip to `false` to restore the proper
// preconditions (planning_complete for Stage 3, scan_waypoints_published for
// Stage 4). See docs/DEV_BYPASSES.md.
constexpr bool kBypassPlannerStageGates = true;
constexpr int kFooterCallToActionWidth = 278;

// QSettings key for the globally-shared selected preset name (not per-robot).
constexpr const char* kGlobalSelectedPresetKey = "planner/coverage_selected_preset";

// =============================================================================
// Coverage preset vocab translators.
//
// UI-side strings (kept for backwards compat with existing buttons / preset
// rows): "boustro" | "snake" | "spiral", and "parallel" | "perpendicular".
// PresetManager JSON canonical strings (PlanningPreset schema): "boustrophedon"
// | "snake" | "spiral", and "longest" | "perpendicular". Keep the boundary
// translation here so all on-disk preset files use the canonical schema and
// can be exported/imported across builds.
// =============================================================================
QString uiPatternToCanonical(const QString& ui) {
    if (ui == QLatin1String("boustro")) return QStringLiteral("boustrophedon");
    return ui;
}
QString canonicalToUiPattern(const QString& canon) {
    if (canon == QLatin1String("boustrophedon")) return QStringLiteral("boustro");
    return canon;
}
QString uiAxisToDirection(const QString& ui) {
    if (ui == QLatin1String("parallel")) return QStringLiteral("longest");
    return ui;
}
QString directionToUiAxis(const QString& canon) {
    if (canon == QLatin1String("longest")) return QStringLiteral("parallel");
    return canon;
}
// Keep the planner header clear of the frameless shell controls overlay.
constexpr int kWindowControlsReservedWidth = 184;
constexpr int kStatusItemHeight = 20;
constexpr int kStatusBatteryMinWidth = 52;
constexpr int kStatusSignalMinWidth = 69;
constexpr int kStatusLockMinWidth = 69;
constexpr int kStatusMotorsChipMinWidth = 96;
constexpr int kStatusMotorsChipHeight = 20;
constexpr int kStatusMotorsChipHorizontalPadding = 9;
constexpr int kStatusMotorsChipSpacing = 6;

class PlannerPreviewHost : public QWidget {
public:
    explicit PlannerPreviewHost(QWidget* parent = nullptr) : QWidget(parent) {}

    void setBaseWidget(QWidget* widget) {
        base_widget_ = widget;
        if (!base_widget_) {
            return;
        }
        base_widget_->setParent(this);
        base_widget_->show();
        base_widget_->lower();
        relayout();
    }

    void setTopRightOverlay(QWidget* widget) {
        top_right_overlay_ = widget;
        prepareOverlay(top_right_overlay_);
        relayout();
    }

    void setTopLeftOverlay(QWidget* widget) {
        top_left_overlay_ = widget;
        prepareOverlay(top_left_overlay_);
        relayout();
    }

    void setBottomLeftOverlay(QWidget* widget) {
        bottom_left_overlay_ = widget;
        prepareOverlay(bottom_left_overlay_);
        relayout();
    }

    // Public hook for callers that toggle overlay visibility from outside the
    // host (e.g. PlannerScreen on stage change). Qt's layout system does not
    // forward grandchild visibility changes back up to this host's resizeEvent,
    // so without this an overlay that becomes visible mid-session keeps the
    // size/position computed when it was hidden (zero or stale).
    void relayoutOverlays() { relayout(); }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        relayout();
    }

private:
    void prepareOverlay(QWidget* widget) {
        if (!widget) {
            return;
        }
        widget->setParent(this);
        widget->show();
        widget->raise();
    }

    QSize overlaySize(QWidget* widget) const {
        if (!widget) {
            return QSize();
        }
        widget->adjustSize();
        return widget->size().expandedTo(widget->minimumSize()).expandedTo(widget->sizeHint());
    }

    void relayout() {
        if (base_widget_) {
            base_widget_->setGeometry(rect());
        }

        if (top_right_overlay_) {
            const QSize size = overlaySize(top_right_overlay_);
            top_right_overlay_->resize(size);
            top_right_overlay_->move(
                std::max(kPreviewOverlayMargin, width() - size.width() - kPreviewOverlayMargin),
                kPreviewOverlayMargin);
            top_right_overlay_->raise();
        }

        if (top_left_overlay_) {
            const QSize size = overlaySize(top_left_overlay_);
            top_left_overlay_->resize(size);
            top_left_overlay_->move(kPreviewOverlayMargin, kPreviewOverlayMargin);
            top_left_overlay_->raise();
        }

        if (bottom_left_overlay_) {
            const QSize size = overlaySize(bottom_left_overlay_);
            bottom_left_overlay_->resize(size);
            bottom_left_overlay_->move(
                kPreviewOverlayMargin,
                std::max(kPreviewOverlayMargin, height() - size.height() - kPreviewOverlayMargin));
            bottom_left_overlay_->raise();
        }
    }

    QPointer<QWidget> base_widget_;
    QPointer<QWidget> top_right_overlay_;
    QPointer<QWidget> top_left_overlay_;
    QPointer<QWidget> bottom_left_overlay_;
};

// QSS `border-radius` clips the background brush with a 1-bit mask in Qt 5,
// which leaves visibly aliased corner pixels showing the parent widget's
// background through the rounded curve. On a high-contrast surface (e.g. the
// near-black map at #09090B) those exposed pixels read as dirty "corner
// fragments" that no border / radius / opacity tweak can hide — the
// geometry of a rounded rect over a contrasting parent always exposes
// triangular corner regions of size r²(1 - π/4) per corner.
//
// This widget paints its own rounded background through QPainter with
// QPainter::Antialiasing enabled, which produces 8-bit alpha-blended edges
// instead of a 1-bit mask. The exposed corner pixels still belong to the
// parent, but the antialiased blend along the curve fades the pill colour
// smoothly into the parent colour, eliminating the visible artifact.
class RoundedFillWidget : public QWidget {
public:
    RoundedFillWidget(QColor fill, qreal radius, QWidget* parent = nullptr)
        : QWidget(parent), fill_(std::move(fill)), radius_(radius) {
        // Disable Qt's default styled background so the QSS path can't paint
        // a competing 1-bit-clipped fill underneath ours.
        setAttribute(Qt::WA_StyledBackground, false);
    }

    void setFillColor(const QColor& color) {
        if (fill_ == color) {
            return;
        }
        fill_ = color;
        update();
    }

    void setRadius(qreal radius) {
        if (qFuzzyCompare(radius_, radius)) {
            return;
        }
        radius_ = radius;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill_);
        // Inset by 0.5px so the antialiased edge sits inside the widget rect
        // — otherwise Qt rounds the half-pixel toward the bounding box and
        // we lose the alpha-fade row, recreating the same hard-edge problem.
        const QRectF rect = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.drawRoundedRect(rect, radius_, radius_);
    }

private:
    QColor fill_;
    qreal radius_;
};

bool sanitizePlannerParameters(double* voxel_size,
                               double* z_min,
                               double* z_max,
                               double* alpha,
                               QStringList* warnings = nullptr) {
    auto sanitize_value =
        [warnings](double* value,
                   double fallback,
                   double minimum,
                   double maximum,
                   const QString& warning_text) {
            bool adjusted = false;
            if (!std::isfinite(*value)) {
                *value = fallback;
                adjusted = true;
            }
            const double clamped = std::max(minimum, std::min(maximum, *value));
            if (*value != clamped) {
                *value = clamped;
                adjusted = true;
            }
            if (adjusted && warnings) {
                warnings->append(warning_text);
            }
            return adjusted;
        };

    bool adjusted = false;
    adjusted |= sanitize_value(voxel_size,
                               0.05,
                               kVoxelSizeMin,
                               kVoxelSizeMax,
                               QStringLiteral("Voxel size was adjusted into 0.01m-0.20m."));
    adjusted |= sanitize_value(z_min,
                               -0.10,
                               kZFilterMin,
                               kZFilterMax,
                               QStringLiteral("Z Min was adjusted into -0.50m-0.50m."));
    adjusted |= sanitize_value(z_max,
                               0.10,
                               kZFilterMin,
                               kZFilterMax,
                               QStringLiteral("Z Max was adjusted into -0.50m-0.50m."));
    adjusted |= sanitize_value(alpha,
                               1.50,
                               kHullAlphaMin,
                               kHullAlphaMax,
                               QStringLiteral("Alpha was adjusted into 0.10-3.00."));

    if (*z_min > *z_max) {
        *z_max = *z_min;
        adjusted = true;
        if (warnings) {
            warnings->append(QStringLiteral("Z Max was raised to match Z Min."));
        }
    }

    return adjusted;
}

PointCloudPtr makeFinitePointCloud(const PointCloudPtr& cloud) {
    if (!cloud || cloud->empty()) {
        return cloud;
    }

    bool needs_filter = false;
    for (const auto& point : cloud->points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
            needs_filter = true;
            break;
        }
    }
    if (!needs_filter) {
        return cloud;
    }

    auto finite_cloud = std::make_shared<PointCloud>();
    finite_cloud->points.reserve(cloud->points.size());
    for (const auto& point : cloud->points) {
        if (std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)) {
            finite_cloud->points.push_back(point);
        }
    }
    finite_cloud->width = static_cast<std::uint32_t>(finite_cloud->points.size());
    finite_cloud->height = finite_cloud->points.empty() ? 0U : 1U;
    finite_cloud->is_dense = true;
    return finite_cloud;
}

QString formatCount(qsizetype value) {
    static const QLocale kLocale(QLocale::English);
    return kLocale.toString(static_cast<qlonglong>(value));
}

QString formatMeters(double value) {
    return QStringLiteral("%1m").arg(value, 0, 'f', 2);
}

QString formatParameter(double value) {
    return QStringLiteral("%1").arg(value, 0, 'f', 2);
}

QString formatPercent(double value) {
    return QStringLiteral("%1%").arg(value, 0, 'f', 0);
}

QString formatFileSize(double value_mb) {
    return QStringLiteral("%1 MB").arg(value_mb, 0, 'f', 1);
}

QString formatArea(double value_m2) {
    return QStringLiteral("%1 m²").arg(value_m2, 0, 'f', 1);
}

double clampValue(double value, double minimum, double maximum) {
    return std::max(minimum, std::min(maximum, value));
}

double computePathLength(const PathStateList& path) {
    if (path.size() < 2) {
        return 0.0;
    }

    double length = 0.0;
    for (size_t index = 1; index < path.size(); ++index) {
        const double dx = path[index].point.x - path[index - 1].point.x;
        const double dy = path[index].point.y - path[index - 1].point.y;
        length += std::hypot(dx, dy);
    }
    return length;
}

void clearLayout(QLayout* layout) {
    if (!layout) {
        return;
    }

    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QLayout* child_layout = item->layout()) {
            clearLayout(child_layout);
            delete child_layout;
        }
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
}

QPixmap makeCoveragePatternPreviewPixmap(const QString& pattern,
                                         const QColor& stroke,
                                         const QColor& guide) {
    constexpr int kWidth = 32;
    constexpr int kHeight = 24;
    QPixmap pixmap(kWidth, kHeight);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    QPen active_pen(stroke, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QPen guide_pen(guide, 2.0, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(active_pen);

    if (pattern == QStringLiteral("snake")) {
        painter.drawLine(QPointF(4.0, 6.0), QPointF(28.0, 6.0));
        painter.drawLine(QPointF(28.0, 6.0), QPointF(28.0, 14.0));
        painter.drawLine(QPointF(28.0, 14.0), QPointF(4.0, 14.0));
        painter.setPen(guide_pen);
        painter.drawLine(QPointF(10.0, 10.0), QPointF(22.0, 10.0));
        painter.drawLine(QPointF(10.0, 18.0), QPointF(22.0, 18.0));
        painter.setPen(active_pen);
        painter.setPen(Qt::NoPen);
        painter.setBrush(stroke);
        painter.drawEllipse(QPointF(4.0, 6.0), 2.0, 2.0);
        return pixmap;
    }
    if (pattern == QStringLiteral("spiral")) {
        QPainterPath path;
        path.moveTo(QPointF(4.0, 6.0));
        path.lineTo(QPointF(28.0, 6.0));
        path.lineTo(QPointF(28.0, 18.0));
        path.lineTo(QPointF(4.0, 18.0));
        path.lineTo(QPointF(4.0, 9.0));
        path.lineTo(QPointF(25.0, 9.0));
        path.lineTo(QPointF(25.0, 15.0));
        path.lineTo(QPointF(7.0, 15.0));
        path.lineTo(QPointF(7.0, 12.0));
        path.lineTo(QPointF(22.0, 12.0));
        painter.drawPath(path);
        painter.setPen(Qt::NoPen);
        painter.setBrush(stroke);
        painter.drawEllipse(QPointF(4.0, 6.0), 2.0, 2.0);
        return pixmap;
    }
    painter.drawLine(QPointF(4.0, 6.0), QPointF(28.0, 6.0));
    painter.drawLine(QPointF(28.0, 6.0), QPointF(28.0, 10.0));
    painter.drawLine(QPointF(28.0, 10.0), QPointF(4.0, 10.0));
    painter.drawLine(QPointF(4.0, 10.0), QPointF(4.0, 14.0));
    painter.drawLine(QPointF(4.0, 14.0), QPointF(28.0, 14.0));
    painter.drawLine(QPointF(28.0, 14.0), QPointF(28.0, 18.0));
    painter.drawLine(QPointF(28.0, 18.0), QPointF(4.0, 18.0));

    painter.setPen(Qt::NoPen);
    painter.setBrush(stroke);
    painter.drawEllipse(QPointF(4.0, 6.0), 2.0, 2.0);
    return pixmap;
}

QPixmap makeCoverageControlIconPixmap(const QString& kind, const QColor& color, int size = 16) {
    const double scale = static_cast<double>(size) / 16.0;
    auto s = [scale](double value) { return value * scale; };
    auto p = [s](double x, double y) { return QPointF(s(x), s(y)); };

    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    QPen pen(color,
             std::max(1.2, static_cast<double>(size) * 0.10),
             Qt::SolidLine,
             Qt::RoundCap,
             Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    if (kind == QStringLiteral("scan_complete")) {
        painter.drawLine(p(5.0, 2.0), p(2.0, 2.0));
        painter.drawLine(p(2.0, 2.0), p(2.0, 5.0));
        painter.drawLine(p(11.0, 2.0), p(14.0, 2.0));
        painter.drawLine(p(14.0, 2.0), p(14.0, 5.0));
        painter.drawLine(p(2.0, 11.0), p(2.0, 14.0));
        painter.drawLine(p(2.0, 14.0), p(5.0, 14.0));
        painter.drawLine(p(11.0, 14.0), p(14.0, 14.0));
        painter.drawLine(p(14.0, 11.0), p(14.0, 14.0));
        painter.drawRoundedRect(QRectF(s(5.5), s(5.5), s(5.0), s(5.0)), s(1.0), s(1.0));
        return pixmap;
    }

    if (kind == QStringLiteral("scan_roi") || kind == QStringLiteral("roi_rectangle") ||
        kind == QStringLiteral("obstacle_rectangle")) {
        painter.drawRoundedRect(QRectF(s(3.0), s(3.0), s(10.0), s(10.0)), s(1.2), s(1.2));
        return pixmap;
    }

    if (kind == QStringLiteral("roi_polygon") || kind == QStringLiteral("obstacle_polygon")) {
        QPainterPath path;
        path.moveTo(p(8.0, 2.5));
        path.lineTo(p(13.2, 6.0));
        path.lineTo(p(11.3, 13.0));
        path.lineTo(p(4.7, 13.0));
        path.lineTo(p(2.8, 6.0));
        path.closeSubpath();
        painter.drawPath(path);
        return pixmap;
    }

    if (kind == QStringLiteral("obstacle_circle")) {
        painter.drawEllipse(QRectF(s(3.0), s(3.0), s(10.0), s(10.0)));
        return pixmap;
    }

    if (kind == QStringLiteral("obstacle_auto")) {
        painter.drawArc(QRectF(s(3.0), s(3.0), s(10.0), s(10.0)), 18 * 16, 110 * 16);
        painter.drawArc(QRectF(s(5.2), s(5.2), s(5.6), s(5.6)), 18 * 16, 110 * 16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(p(8.0, 8.0), s(1.3), s(1.3));
        return pixmap;
    }

    if (kind == QStringLiteral("obstacle_manual")) {
        painter.drawLine(p(2.5, 5.0), p(13.5, 5.0));
        painter.drawLine(p(2.5, 11.0), p(13.5, 11.0));
        painter.setBrush(color);
        painter.drawEllipse(p(6.0, 5.0), s(1.4), s(1.4));
        painter.drawEllipse(p(10.0, 11.0), s(1.4), s(1.4));
        return pixmap;
    }

    if (kind == QStringLiteral("cpu")) {
        painter.drawRoundedRect(QRectF(s(4.0), s(4.0), s(8.0), s(8.0)), s(1.0), s(1.0));
        painter.drawLine(p(6.0, 1.5), p(6.0, 4.0));
        painter.drawLine(p(10.0, 1.5), p(10.0, 4.0));
        painter.drawLine(p(6.0, 12.0), p(6.0, 14.5));
        painter.drawLine(p(10.0, 12.0), p(10.0, 14.5));
        painter.drawLine(p(1.5, 6.0), p(4.0, 6.0));
        painter.drawLine(p(1.5, 10.0), p(4.0, 10.0));
        painter.drawLine(p(12.0, 6.0), p(14.5, 6.0));
        painter.drawLine(p(12.0, 10.0), p(14.5, 10.0));
        return pixmap;
    }

    if (kind == QStringLiteral("check")) {
        painter.drawEllipse(QRectF(s(2.2), s(2.2), s(11.6), s(11.6)));
        painter.drawLine(p(5.0, 8.4), p(7.1, 10.5));
        painter.drawLine(p(7.1, 10.5), p(11.4, 6.0));
        return pixmap;
    }

    return pixmap;
}

Polygon2D makeEllipsePolygonFromRectangle(const Polygon2D& rect, int segments = 24) {
    if (rect.size() < 4 || segments < 8) {
        return rect;
    }

    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
    polygonBounds(rect, min_x, min_y, max_x, max_y);

    const double radius_x = std::max(0.0, (max_x - min_x) * 0.5);
    const double radius_y = std::max(0.0, (max_y - min_y) * 0.5);
    if (radius_x <= 1e-6 || radius_y <= 1e-6) {
        return rect;
    }

    constexpr double kTau = 6.28318530717958647692;
    const double center_x = (min_x + max_x) * 0.5;
    const double center_y = (min_y + max_y) * 0.5;

    Polygon2D ellipse;
    ellipse.reserve(static_cast<size_t>(segments));
    for (int index = 0; index < segments; ++index) {
        const double angle = (kTau * static_cast<double>(index)) / static_cast<double>(segments);
        ellipse.push_back(
            {center_x + std::cos(angle) * radius_x, center_y + std::sin(angle) * radius_y});
    }
    return ellipse;
}

double estimateAreaFromPointCloudBounds(const PointCloudPtr& cloud) {
    if (!cloud || cloud->empty()) {
        return 0.0;
    }

    double min_x = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double min_y = std::numeric_limits<double>::max();
    double max_y = std::numeric_limits<double>::lowest();
    bool has_finite_point = false;
    for (const auto& point : cloud->points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            continue;
        }
        has_finite_point = true;
        min_x = std::min(min_x, static_cast<double>(point.x));
        max_x = std::max(max_x, static_cast<double>(point.x));
        min_y = std::min(min_y, static_cast<double>(point.y));
        max_y = std::max(max_y, static_cast<double>(point.y));
    }

    if (!has_finite_point) {
        return 0.0;
    }

    const double width = std::max(0.0, max_x - min_x);
    const double height = std::max(0.0, max_y - min_y);
    return width * height;
}

std::vector<Point2D> buildProjectedPointsFromCloud(const PointCloudPtr& cloud) {
    std::vector<Point2D> projected_points;
    if (!cloud || cloud->empty()) {
        return projected_points;
    }

    const size_t total_points = cloud->points.size();
    const size_t stride = std::max<size_t>(
        1,
        static_cast<size_t>(
            std::ceil(static_cast<double>(total_points) /
                      static_cast<double>(kPreviewTargetPointCount))));
    projected_points.reserve((total_points + stride - 1) / stride);
    for (size_t index = 0; index < total_points; index += stride) {
        const auto& point = cloud->points[index];
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            continue;
        }
        projected_points.emplace_back(point.x, point.y);
    }

    if (projected_points.empty() && !cloud->points.empty()) {
        for (const auto& point : cloud->points) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                continue;
            }
            projected_points.emplace_back(point.x, point.y);
            break;
        }
    }
    return projected_points;
}

QString deriveQualityLabel(qsizetype raw_count, qsizetype processed_count, double voxel_size) {
    if (raw_count <= 0 || processed_count <= 0) {
        return QStringLiteral("--");
    }

    const double retained_ratio = static_cast<double>(processed_count) / static_cast<double>(raw_count);
    if (retained_ratio >= 0.75 && voxel_size <= 0.04) {
        return QStringLiteral("High");
    }
    if (retained_ratio >= 0.40 && voxel_size <= 0.10) {
        return QStringLiteral("Medium");
    }
    return QStringLiteral("Low");
}

// Forward decl so the rotated variant can call into the standard renderer
// for callers that pass angle == 0 (unrotated path stays cheap).
QPixmap loadSvgPixmap(const QString& resource_path,
                      int width,
                      int height,
                      const QString& color);

QPixmap loadRotatedSvgPixmap(const QString& resource_path,
                             int width,
                             int height,
                             const QString& color,
                             qreal angle_deg) {
    QPixmap base = loadSvgPixmap(resource_path, width, height, color);
    if (base.isNull() || std::abs(angle_deg) < 0.001) {
        return base;
    }
    QPixmap out(width, height);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.translate(width / 2.0, height / 2.0);
    painter.rotate(angle_deg);
    painter.translate(-width / 2.0, -height / 2.0);
    painter.drawPixmap(0, 0, base);
    return out;
}

QPixmap loadSvgPixmap(const QString& resource_path,
                      int width,
                      int height,
                      const QString& color = QString()) {
    QFile file(resource_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }

    QByteArray data = file.readAll();
    file.close();
    QString svg = QString::fromUtf8(data);
    if (!color.isEmpty()) {
        svg.replace(QStringLiteral("currentColor"), color);
    }
    static const QRegularExpression kFigmaVarColorPattern(
        QStringLiteral(R"(var\(--(?:fill|stroke)-\d+,\s*(#[0-9A-Fa-f]{3,8})\s*\))"));
    QString resolved_svg;
    resolved_svg.reserve(svg.size());
    int cursor = 0;
    QRegularExpressionMatchIterator it = kFigmaVarColorPattern.globalMatch(svg);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const int start = match.capturedStart(0);
        const int end = match.capturedEnd(0);
        if (start < 0 || end < start) {
            continue;
        }
        resolved_svg += svg.mid(cursor, start - cursor);
        resolved_svg += color.isEmpty() ? match.captured(1) : color;
        cursor = end;
    }
    if (cursor > 0) {
        resolved_svg += svg.mid(cursor);
        svg = resolved_svg;
    }
    data = svg.toUtf8();

    QSvgRenderer renderer(data);
    if (!renderer.isValid()) {
        return QPixmap();
    }

    QPixmap pixmap(width, height);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    return pixmap;
}

QLabel* makeIconLabel(QWidget* parent, const QString& resource_path, int size,
                      const QString& color = QString()) {
    auto* label = new QLabel(parent);
    label->setFixedSize(size, size);
    label->setAlignment(Qt::AlignCenter);
    label->setAttribute(Qt::WA_TranslucentBackground, true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    label->setStyleSheet(QStringLiteral("background: transparent;"));
    label->setPixmap(loadSvgPixmap(resource_path, size, size, color));
    return label;
}

QLabel* makeTextLabel(QWidget* parent, const QString& text, const QString& style,
                      Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter) {
    auto* label = new QLabel(text, parent);
    label->setAlignment(alignment);
    label->setAttribute(Qt::WA_TranslucentBackground, true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    label->setStyleSheet(style + QStringLiteral(" background: transparent;"));
    return label;
}

QWidget* makeStatusItem(QWidget* parent,
                        const QString& resource_path,
                        int icon_size,
                        const QString& text,
                        int minimum_width,
                        const QString& text_style,
                        const QString& color = QString(),
                        QLabel** out_label = nullptr) {
    auto* item = new QWidget(parent);
    item->setFixedHeight(kStatusItemHeight);
    item->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    if (minimum_width > 0) {
        item->setMinimumWidth(minimum_width);
    }

    auto* layout = new QHBoxLayout(item);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(makeIconLabel(item, resource_path, icon_size, color), 0, Qt::AlignVCenter);
    auto* label = makeTextLabel(item, text, text_style);
    if (out_label) {
        *out_label = label;
    }
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    return item;
}

void applyDropShadow(QWidget* widget, int blur_radius, int y_offset, const QColor& color) {
    if (!widget) {
        return;
    }

    auto* effect = new QGraphicsDropShadowEffect(widget);
    effect->setBlurRadius(blur_radius);
    effect->setOffset(0, y_offset);
    effect->setColor(color);
    widget->setGraphicsEffect(effect);
}

}  // namespace

class PlannerTrackSlider : public QWidget {
public:
    explicit PlannerTrackSlider(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(kWidgetHeight);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
    }

    void setRange(double minimum, double maximum) {
        minimum_ = minimum;
        maximum_ = std::max(minimum_, maximum);
        setValue(value_);
    }

    void setStep(double step) { step_ = std::max(0.0, step); }
    double step() const { return step_; }
    void setDecimals(int decimals) { decimals_ = std::max(0, decimals); }
    void setDarkMode(bool dark_mode) {
        dark_mode_ = dark_mode;
        update();
    }

    void setValue(double value) {
        const double snapped = snapValue(value);
        if (std::abs(value_ - snapped) < 1e-9) {
            return;
        }
        value_ = snapped;
        update();
    }

    double value() const { return value_; }
    double minimum() const { return minimum_; }
    double maximum() const { return maximum_; }

    std::function<void(double)> on_value_changed;

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const qreal track_height = 8.0;
        const qreal track_radius = track_height / 2.0;
        const qreal track_left = kThumbRadius;
        const qreal track_right = width() - kThumbRadius;
        const qreal track_width = std::max<qreal>(0.0, track_right - track_left);
        const qreal track_y = (height() - track_height) / 2.0;
        const QRectF track_rect(track_left, track_y, track_width, track_height);

        const QColor track_unfilled = isEnabled()
                                          ? (dark_mode_ ? QColor(QStringLiteral("#3F3F47"))
                                                        : QColor(QStringLiteral("#D4D4D8")))
                                          : (dark_mode_ ? QColor(QStringLiteral("#27272A"))
                                                        : QColor(QStringLiteral("#E5E7EB")));
        const QColor accent = isEnabled()
                                  ? (dark_mode_ ? QColor(QStringLiteral("#00BC7D"))
                                                : QColor(QStringLiteral("#009966")))
                                  : (dark_mode_ ? QColor(QStringLiteral("#3F3F47"))
                                                : QColor(QStringLiteral("#A1A1AA")));

        painter.setPen(Qt::NoPen);
        painter.setBrush(track_unfilled);
        painter.drawRoundedRect(track_rect, track_radius, track_radius);

        const qreal handle_cx = trackValueX(value_, track_left, track_width);
        const qreal handle_cy = track_rect.center().y();

        const qreal filled_right = std::clamp<qreal>(handle_cx, track_left, track_right);
        if (filled_right > track_left) {
            QRectF filled_rect(track_left, track_y, filled_right - track_left, track_height);
            painter.setBrush(accent);
            painter.drawRoundedRect(filled_rect, track_radius, track_radius);
        }

        const qreal handle_radius = dragging_ ? kThumbRadius + 1.0 : kThumbRadius;
        const QPointF handle_center(handle_cx, handle_cy);

        const QColor shadow_color(0, 0, 0, dark_mode_ ? 90 : 60);
        painter.setBrush(shadow_color);
        painter.drawEllipse(handle_center + QPointF(0.0, 2.0),
                            handle_radius + 1.5, handle_radius + 1.5);

        painter.setBrush(accent);
        painter.drawEllipse(handle_center, handle_radius, handle_radius);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (!isEnabled() || event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }

        dragging_ = true;
        updateFromX(event->pos().x());
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_ && isEnabled()) {
            updateFromX(event->pos().x());
            event->accept();
            return;
        }

        QWidget::mouseMoveEvent(event);
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (dragging_ && event->button() == Qt::LeftButton) {
            dragging_ = false;
            update();
            event->accept();
            return;
        }

        QWidget::mouseReleaseEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        QWidget::leaveEvent(event);
        if (!dragging_) {
            update();
        }
    }

private:
    static constexpr int kWidgetHeight = 28;
    static constexpr qreal kThumbRadius = 10.0;

    double clampValue(double value) const {
        return std::min(maximum_, std::max(minimum_, value));
    }

    double snapValue(double value) const {
        const double clamped = clampValue(value);
        if (step_ <= 0.0 || maximum_ <= minimum_) {
            return clamped;
        }

        const double steps = std::round((clamped - minimum_) / step_);
        const double snapped = minimum_ + (steps * step_);
        const double factor = std::pow(10.0, decimals_);
        return std::round(clampValue(snapped) * factor) / factor;
    }

    qreal trackValueX(double value, qreal track_left, qreal track_width) const {
        if (maximum_ <= minimum_ || track_width <= 0.0) {
            return track_left;
        }
        const double t = (value - minimum_) / (maximum_ - minimum_);
        return track_left + std::clamp<qreal>(t, 0.0, 1.0) * track_width;
    }

    void updateFromX(double x) {
        const qreal track_left = kThumbRadius;
        const qreal track_right = width() - kThumbRadius;
        const qreal track_width = std::max<qreal>(0.0, track_right - track_left);
        if (maximum_ <= minimum_ || track_width <= 0.0) {
            return;
        }

        const qreal clamped = std::clamp<qreal>(x, track_left, track_right);
        const double t = (clamped - track_left) / track_width;
        const double next_value = snapValue(minimum_ + t * (maximum_ - minimum_));
        if (std::abs(next_value - value_) < 1e-9) {
            return;
        }

        value_ = next_value;
        if (on_value_changed) {
            on_value_changed(value_);
        }
        update();
    }

    double minimum_ = 0.0;
    double maximum_ = 1.0;
    double value_ = 0.0;
    double step_ = 0.01;
    int decimals_ = 2;
    bool dragging_ = false;
    bool dark_mode_ = true;
};

PlannerScreen::PlannerScreen(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    buildUi();
    autotest_enabled_ = qEnvironmentVariableIsSet("BDR_PLANNER_AUTOTEST_MAP");
    autotest_mode_ = qEnvironmentVariable("BDR_PLANNER_AUTOTEST_MODE", QStringLiteral("full"));

    // Preset persistence (PresetManager: JSON-per-file at
    // ~/.config/PilotControl/presets/<name>.json). Must be live before the
    // first call to restoreCurrentSession() / ensureCoverageDefaults() since
    // those drive reloadCoveragePresetsFromDisk().
    preset_manager_ = new PresetManager(this);
    connect(preset_manager_, &PresetManager::presetsChanged, this, [this]() {
        // Refresh every cached robot session so a preset added/removed on one
        // robot is visible immediately when the operator switches contexts.
        for (auto& cache : session_cache_) {
            reloadCoveragePresetsFromDisk(cache);
        }
        refreshCoveragePresetCombo();
        rebuildCoveragePresetRows();
        applySessionToUi();
    });

    setMapPath(QString());
}

void PlannerScreen::setDarkMode(bool dark_mode) {
    dark_mode_ = dark_mode;
    if (plot_) {
        plot_->setDarkMode(dark_mode_);
        plot_->setPlannerPreviewMode(true);
    }
    if (slider_voxel_) {
        slider_voxel_->setDarkMode(dark_mode_);
    }
    if (slider_z_min_) {
        slider_z_min_->setDarkMode(dark_mode_);
    }
    if (slider_z_max_) {
        slider_z_max_->setDarkMode(dark_mode_);
    }
    if (slider_alpha_) {
        slider_alpha_->setDarkMode(dark_mode_);
    }
    if (slider_coverage_path_spacing_) {
        slider_coverage_path_spacing_->setDarkMode(dark_mode_);
    }
    if (slider_coverage_headland_) {
        slider_coverage_headland_->setDarkMode(dark_mode_);
    }
    if (slider_coverage_scan_speed_) {
        slider_coverage_scan_speed_->setDarkMode(dark_mode_);
    }
    const auto auto_hide_bars = findChildren<AutoHideScrollBar*>();
    for (auto* bar : auto_hide_bars) {
        bar->setDarkMode(dark_mode_);
    }
    // Per-row Rename/Delete icon buttons in the Custom Presets card. They
    // re-render the cached tinted pixmap on theme flip.
    const auto svg_icon_buttons = findChildren<SvgIconButton*>();
    for (auto* btn : svg_icon_buttons) {
        btn->setDarkMode(dark_mode_);
    }
    applyStyle();
    updateHeaderForCurrentStep();
    updateStageSteps();
    updateFooter();
}

void PlannerScreen::applyToneToLabel(QLabel* label, ValueTone tone, bool emphasize) {
    if (!label) {
        return;
    }

    QString color = dark_mode_ ? QStringLiteral("#9F9FA9") : QStringLiteral("#6B7280");
    switch (tone) {
        case ValueTone::Good:
            color = QStringLiteral("#10B981");
            break;
        case ValueTone::Warning:
            color = QStringLiteral("#F59E0B");
            break;
        case ValueTone::Muted:
            break;
        case ValueTone::Error:
            color = QStringLiteral("#EF4444");
            break;
    }

    const int weight = emphasize ? 500 : 400;
    label->setStyleSheet(
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: %1; color: %2; "
                       "background: transparent;")
            .arg(weight)
            .arg(color));
}

void PlannerScreen::setTopSignalState(const QString& text, ValueTone tone) {
    if (top_signal_text_ == text && top_signal_tone_ == tone) {
        return;
    }
    top_signal_text_ = text;
    top_signal_tone_ = tone;
    if (!lbl_top_signal_) {
        return;
    }
    lbl_top_signal_->setText(text);
    applyToneToLabel(lbl_top_signal_, tone, false);
    QString icon_color = dark_mode_ ? QStringLiteral("#9F9FA9") : QStringLiteral("#6B7280");
    switch (tone) {
        case ValueTone::Good:
            icon_color = QStringLiteral("#10B981");
            break;
        case ValueTone::Warning:
            icon_color = QStringLiteral("#F59E0B");
            break;
        case ValueTone::Muted:
            break;
        case ValueTone::Error:
            icon_color = QStringLiteral("#EF4444");
            break;
    }
    if (QWidget* item = lbl_top_signal_->parentWidget()) {
        const auto icons = item->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
        for (QLabel* icon_label : icons) {
            if (icon_label && icon_label != lbl_top_signal_) {
                icon_label->setPixmap(loadSvgPixmap(
                    QStringLiteral(":/assets/missionplanner/status_dot.svg"), 8, 8, icon_color));
                break;
            }
        }
    }
}

void PlannerScreen::setTopLockChipState(const QString& text, ValueTone tone) {
    if (top_lock_text_ == text && top_lock_tone_ == tone) {
        return;
    }
    top_lock_text_ = text;
    top_lock_tone_ = tone;
    if (!lbl_top_lock_chip_) {
        return;
    }
    lbl_top_lock_chip_->setText(text);
    applyToneToLabel(lbl_top_lock_chip_, tone, true);
    QString icon_color = dark_mode_ ? QStringLiteral("#9F9FA9") : QStringLiteral("#6B7280");
    switch (tone) {
        case ValueTone::Good:
            icon_color = QStringLiteral("#10B981");
            break;
        case ValueTone::Warning:
            icon_color = QStringLiteral("#F59E0B");
            break;
        case ValueTone::Muted:
            break;
        case ValueTone::Error:
            icon_color = QStringLiteral("#EF4444");
            break;
    }
    if (QWidget* item = lbl_top_lock_chip_->parentWidget()) {
        const auto icons = item->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
        for (QLabel* icon_label : icons) {
            if (icon_label && icon_label != lbl_top_lock_chip_) {
                icon_label->setPixmap(
                    loadSvgPixmap(QStringLiteral(":/assets/missionplanner/lock.svg"), 16, 16, icon_color));
                break;
            }
        }
    }
}

void PlannerScreen::updateTopMotorsChipGeometry() {
    if (!top_motors_chip_ || !lbl_top_motors_text_ || !lbl_top_motors_dot_) {
        return;
    }

    lbl_top_motors_text_->adjustSize();
    const int text_width = lbl_top_motors_text_->sizeHint().width();
    const int dot_width = std::max(0, lbl_top_motors_dot_->width());
    const int chip_width =
        std::max(kStatusMotorsChipMinWidth,
                 (2 * kStatusMotorsChipHorizontalPadding) + dot_width + kStatusMotorsChipSpacing +
                     text_width + 4);
    const QSize next_size(chip_width, kStatusMotorsChipHeight);
    if (top_motors_chip_->size() != next_size) {
        top_motors_chip_->setFixedSize(next_size);
    }
}

void PlannerScreen::setTopMotorsChipState(const QString& text, ValueTone tone) {
    if (top_motors_text_ == text && top_motors_tone_ == tone) {
        return;
    }
    top_motors_text_ = text;
    top_motors_tone_ = tone;
    if (!top_motors_chip_ || !lbl_top_motors_dot_ || !lbl_top_motors_text_) {
        return;
    }

    QString bg = dark_mode_ ? QStringLiteral("rgba(113,113,123,0.18)")
                            : QStringLiteral("rgba(100,116,139,0.10)");
    QString border = dark_mode_ ? QStringLiteral("rgba(113,113,123,0.28)")
                                : QStringLiteral("rgba(100,116,139,0.18)");
    QString text_color = dark_mode_ ? QStringLiteral("#71717B") : QStringLiteral("#6B7280");
    switch (tone) {
        case ValueTone::Good:
            bg = dark_mode_ ? QStringLiteral("rgba(0,188,125,0.12)")
                            : QStringLiteral("rgba(5,150,105,0.10)");
            border = dark_mode_ ? QStringLiteral("rgba(0,188,125,0.24)")
                                : QStringLiteral("rgba(5,150,105,0.18)");
            text_color = dark_mode_ ? QStringLiteral("#00D492") : QStringLiteral("#059669");
            break;
        case ValueTone::Warning:
            bg = dark_mode_ ? QStringLiteral("rgba(245,158,11,0.12)")
                            : QStringLiteral("rgba(245,158,11,0.10)");
            border = dark_mode_ ? QStringLiteral("rgba(245,158,11,0.24)")
                                : QStringLiteral("rgba(245,158,11,0.18)");
            text_color = QStringLiteral("#F59E0B");
            break;
        case ValueTone::Muted:
            break;
        case ValueTone::Error:
            bg = dark_mode_ ? QStringLiteral("rgba(251,44,54,0.10)")
                            : QStringLiteral("rgba(239,68,68,0.10)");
            border = dark_mode_ ? QStringLiteral("rgba(251,44,54,0.20)")
                                : QStringLiteral("rgba(239,68,68,0.20)");
            text_color = dark_mode_ ? QStringLiteral("#FF6467") : QStringLiteral("#DC2626");
            break;
    }

    top_motors_chip_->setStyleSheet(
        QStringLiteral("background: %1; border: 1px solid %2; border-radius: 4px;")
            .arg(bg, border));
    lbl_top_motors_dot_->setPixmap(
        loadSvgPixmap(QStringLiteral(":/assets/missionplanner/motors_armed_dot.svg"), 6, 6, text_color));
    lbl_top_motors_text_->setText(text);
    lbl_top_motors_text_->setStyleSheet(
        QStringLiteral("font-family: 'Arimo'; font-size: 10px; font-weight: 700; color: %1; "
                       "letter-spacing: 0.5px; background: transparent;")
            .arg(text_color));
    updateTopMotorsChipGeometry();
}

void PlannerScreen::applyStyle() {
    const QString bg = dark_mode_ ? QStringLiteral("#09090B") : QStringLiteral("#F4F4F5");
    const QString header = dark_mode_ ? QStringLiteral("#18181B") : QStringLiteral("#FFFFFF");
    const QString border = dark_mode_ ? QStringLiteral("#27272A") : QStringLiteral("#D1D5DB");
    const QString left_surface = dark_mode_ ? QStringLiteral("#18181B") : QStringLiteral("#FAFAFA");
    const QString card_soft = dark_mode_ ? QStringLiteral("rgba(39,39,42,0.50)")
                                         : QStringLiteral("#FFFFFF");
    const QString title = dark_mode_ ? QStringLiteral("#FFFFFF") : QStringLiteral("#111827");
    const QString text = dark_mode_ ? QStringLiteral("#FFFFFF") : QStringLiteral("#111827");
    const QString muted = dark_mode_ ? QStringLiteral("#9F9FA9") : QStringLiteral("#475569");
    const QString submuted = dark_mode_ ? QStringLiteral("#71717B") : QStringLiteral("#64748B");
    const QString heading_tone = dark_mode_ ? QStringLiteral("#9F9FA9") : QStringLiteral("#475569");
    const QString accent = dark_mode_ ? QStringLiteral("#00BC7D") : QStringLiteral("#059669");
    const QString accent_hover = dark_mode_ ? QStringLiteral("#0ACB8B") : QStringLiteral("#047857");
    const QString accent_text = QStringLiteral("#FFFFFF");
    const QString accent_soft_bg = dark_mode_ ? QStringLiteral("rgba(0,188,125,0.10)")
                                              : QStringLiteral("rgba(5,150,105,0.08)");
    const QString accent_soft_border = dark_mode_ ? QStringLiteral("rgba(0,188,125,0.20)")
                                                  : QStringLiteral("rgba(5,150,105,0.18)");
    const QString center_bg = dark_mode_ ? QStringLiteral("#09090B") : QStringLiteral("#FFFFFF");
    const QString tool_bg = dark_mode_ ? QStringLiteral("rgba(39,39,42,0.90)")
                                       : QStringLiteral("rgba(255,255,255,0.92)");
    const QString tool_border = dark_mode_ ? QStringLiteral("#3F3F47") : QStringLiteral("#D4D4D8");
    const QString tool_hover = dark_mode_ ? QStringLiteral("#52525C") : QStringLiteral("#94A3B8");
    const QString surface_hover = dark_mode_ ? QStringLiteral("rgba(39,39,42,0.55)")
                                             : QStringLiteral("rgba(15,23,42,0.05)");
    const QString next_disabled = dark_mode_ ? QStringLiteral("#1F2937") : QStringLiteral("#CBD5E1");
    const QString neutral_button_bg = dark_mode_ ? QStringLiteral("#27272A") : QStringLiteral("#FFFFFF");
    const QString neutral_button_border = dark_mode_ ? QStringLiteral("#3F3F47")
                                                     : QStringLiteral("#D4D4D8");
    const QString neutral_button_disabled = dark_mode_ ? QStringLiteral("#1F1F23")
                                                       : QStringLiteral("#E5E7EB");
    const QString neutral_button_text = dark_mode_ ? QStringLiteral("#E4E4E7")
                                                   : QStringLiteral("#374151");
    setStyleSheet(QStringLiteral("background: %1;").arg(bg));

    if (top_bar_) {
        top_bar_->setStyleSheet(QStringLiteral("background: %1;").arg(header));
    }
    if (btn_back_) {
        btn_back_->setStyleSheet(
            QStringLiteral("QPushButton { background: transparent; border: none; border-radius: "
                           "10px; } QPushButton:hover { background: %1; }")
                .arg(surface_hover));
    }
    if (lbl_back_icon_) {
        lbl_back_icon_->setPixmap(
            loadSvgPixmap(QStringLiteral(":/assets/missionplanner/back.svg"), 16, 16, muted));
    }
    if (lbl_back_text_) {
        lbl_back_text_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 400; color: %1; "
                           "background: transparent;")
                .arg(muted));
    }
    if (title_divider_) {
        title_divider_->setStyleSheet(QStringLiteral("background: %1;").arg(border));
    }
    for (QWidget* separator : stage_separator_widgets_) {
        if (separator) {
            separator->setStyleSheet(
                QStringLiteral("background: %1;").arg(dark_mode_ ? QStringLiteral("#3F3F47")
                                                                 : QStringLiteral("#D4D4D8")));
        }
    }
    if (lbl_title_) {
        lbl_title_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 24px; font-weight: 700; color: %1; "
                           "background: transparent;")
                .arg(title));
    }
    if (lbl_top_battery_) {
        lbl_top_battery_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: %1; "
                           "background: transparent;")
                .arg(muted));
    }
    if (stage_header_) {
        stage_header_->setStyleSheet(QStringLiteral("background: %1;").arg(header));
    }
    if (left_header_) {
        left_header_->setStyleSheet(
            QStringLiteral("background: %1; border-top: 1px solid %2; border-right: 1px solid %2; "
                           "border-bottom: 1px solid %2;")
                .arg(left_surface, border));
    }
    if (stage_row_host_) {
        // This creates the single separator line at the top of the row
        stage_row_host_->setStyleSheet(
            QStringLiteral("background: %1; border-top: 1px solid %2; border-bottom: none;")
            .arg(header, border)
        );
    }
    
    if (stage_row_frame_) {
        // Remove ALL borders from the inner frame so it doesn't double up
        stage_row_frame_->setStyleSheet(
            QStringLiteral("background: %1; border: none;").arg(header)
        );
    }
    if (left_header_icon_box_) {
        left_header_icon_box_->setStyleSheet(
            QStringLiteral("background: %1; border: 1px solid %2; border-radius: 10px;")
                .arg(accent_soft_bg, accent_soft_border));
    }
    if (lbl_left_header_title_) {
        lbl_left_header_title_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; color: %1; "
                           "background: transparent;")
                .arg(title));
    }
    if (lbl_left_header_subtitle_) {
        lbl_left_header_subtitle_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: %1; "
                           "background: transparent;")
                .arg(submuted));
    }
    if (left_rail_) {
        left_rail_->setStyleSheet(
            QStringLiteral("background: %1; border-right: 1px solid %2;").arg(left_surface, border));
    }
    if (output_section_) {
        output_section_->setStyleSheet(
            QStringLiteral("background: transparent; border-top: 1px solid %1;").arg(border));
    }
    for (QWidget* card_widget : output_cards_) {
        if (!card_widget) {
            continue;
        }
        card_widget->setStyleSheet(
            QStringLiteral("background: %1; border: 1px solid %2; border-radius: 10px;")
                .arg(card_soft, border));
    }
    if (placeholder_card_) {
        placeholder_card_->setStyleSheet(
            QStringLiteral("background: %1; border: 1px solid %2; border-radius: 18px;")
                .arg(card_soft, border));
    }
    if (center_stage_) {
        center_stage_->setStyleSheet(QStringLiteral("background: %1;").arg(center_bg));
    }
    if (stats_chip_) {
        stats_chip_->setStyleSheet(
            QStringLiteral("background: %1; border: 1px solid %2; border-radius: 10px;")
                .arg(card_soft, border));
    }
    if (footer_) {
        footer_->setStyleSheet(QStringLiteral("background: %1;").arg(header));
    }

    for (QLabel* label : label12_labels_) {
        if (label) {
            label->setStyleSheet(
                QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: "
                               "%1; background: transparent;")
                    .arg(text));
        }
    }
    for (QLabel* label : label9_labels_) {
        if (label) {
            label->setStyleSheet(
                QStringLiteral("font-family: 'Arimo'; font-size: 11px; font-weight: 400; color: "
                               "%1; background: transparent;")
                    .arg(submuted));
        }
    }
    for (QLabel* label : label10_labels_) {
        if (label) {
            label->setStyleSheet(
                QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: "
                               "%1; background: transparent;")
                    .arg(submuted));
        }
    }
    for (QLabel* label : heading10_labels_) {
        if (label) {
            label->setStyleSheet(
                QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 700; color: "
                               "%1; letter-spacing: 0.5px; background: transparent;")
                    .arg(heading_tone));
        }
    }
    for (QLabel* label : mono9_labels_) {
        if (label) {
            label->setStyleSheet(
                QStringLiteral("font-family: 'Liberation Mono'; font-size: 11px; font-weight: 400; "
                               "color: %1; background: transparent;")
                    .arg(submuted));
        }
    }
    for (QLabel* label : mono12_muted_labels_) {
        if (label) {
            label->setStyleSheet(
                QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 400; "
                               "color: %1; background: transparent;")
                    .arg(muted));
        }
    }
    for (QLabel* label : mono12_white_labels_) {
        if (label) {
            label->setStyleSheet(
                QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 600; "
                               "color: %1; background: transparent;")
                    .arg(title));
        }
    }
    for (QLabel* label : mono12_accent_labels_) {
        if (label) {
            label->setStyleSheet(
                QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 600; "
                               "color: %1; background: transparent;")
                    .arg(accent));
        }
    }
    if (lbl_stats_points_) {
        lbl_stats_points_->setStyleSheet(
            QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 400; "
                           "color: %1; background: transparent;")
                .arg(text));
    }
    if (lbl_stats_area_) {
        lbl_stats_area_->setStyleSheet(
            QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 400; "
                           "color: %1; background: transparent;")
                .arg(text));
    }

    if (lbl_placeholder_title_) {
        lbl_placeholder_title_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 24px; font-weight: 700; color: %1; "
                           "background: transparent;")
                .arg(title));
    }
    if (lbl_stage2_message_) {
        lbl_stage2_message_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 13px; font-weight: 400; color: %1; "
                           "background: transparent;")
                .arg(muted));
    }
    if (lbl_process_icon_) {
        lbl_process_icon_->setPixmap(loadSvgPixmap(
            QStringLiteral(":/assets/missionplanner/process_point_cloud.svg"), 16, 16, accent_text));
    }
    if (lbl_process_text_) {
        lbl_process_text_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 700; color: %1; "
                           "background: transparent;")
                .arg(accent_text));
    }
    if (lbl_hull_icon_) {
        lbl_hull_icon_->setPixmap(loadSvgPixmap(
            QStringLiteral(":/assets/missionplanner/compute_hull.svg"), 14, 14, neutral_button_text));
    }
    if (lbl_hull_text_) {
        lbl_hull_text_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: %1; "
                           "background: transparent;")
                .arg(neutral_button_text));
    }
    if (btn_process_) {
        btn_process_->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; border: none; border-radius: 14px; } "
                           "QPushButton:hover { background: %2; } "
                           "QPushButton:disabled { background: %3; }")
                .arg(accent, accent_hover, next_disabled));
    }
    if (btn_hull_) {
        btn_hull_->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: "
                           "10px; } QPushButton:hover { border-color: %3; } "
                           "QPushButton:disabled { background: %4; border-color: %2; }")
                .arg(neutral_button_bg, neutral_button_border, tool_hover, neutral_button_disabled));
    }
    if (lbl_tool_zoom_in_icon_) {
        lbl_tool_zoom_in_icon_->setPixmap(loadSvgPixmap(
            QStringLiteral(":/assets/missionplanner/tool_zoom_in.svg"), 14, 14, neutral_button_text));
    }
    if (lbl_tool_fit_icon_) {
        lbl_tool_fit_icon_->setPixmap(loadSvgPixmap(
            QStringLiteral(":/assets/missionplanner/tool_fit.svg"), 14, 14, neutral_button_text));
    }
    if (lbl_tool_reset_icon_) {
        lbl_tool_reset_icon_->setPixmap(loadSvgPixmap(
            QStringLiteral(":/assets/missionplanner/tool_reset.svg"), 14, 14, neutral_button_text));
    }
    for (QPushButton* tool_button : {tool_zoom_in_, tool_fit_, tool_reset_}) {
        if (!tool_button) {
            continue;
        }
        tool_button->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: "
                           "10px; } QPushButton:hover { border-color: %3; }")
                .arg(tool_bg, tool_border, tool_hover));
    }

    if (btn_next_) {
        btn_next_->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; border: none; border-radius: 10px; } "
                           "QPushButton:hover { background: %2; } "
                           "QPushButton:disabled { background: %3; }")
                .arg(accent, accent_hover, next_disabled));
    }
    if (slider_voxel_) {
        slider_voxel_->setDarkMode(dark_mode_);
    }
    if (slider_z_min_) {
        slider_z_min_->setDarkMode(dark_mode_);
    }
    if (slider_z_max_) {
        slider_z_max_->setDarkMode(dark_mode_);
    }
    if (slider_alpha_) {
        slider_alpha_->setDarkMode(dark_mode_);
    }
    if (slider_coverage_path_spacing_) {
        slider_coverage_path_spacing_->setDarkMode(dark_mode_);
    }
    if (slider_coverage_headland_) {
        slider_coverage_headland_->setDarkMode(dark_mode_);
    }
    if (slider_coverage_scan_speed_) {
        slider_coverage_scan_speed_->setDarkMode(dark_mode_);
    }

    refreshStepperButtons();

    setTopSignalState(top_signal_text_, top_signal_tone_);
    setTopLockChipState(top_lock_text_, top_lock_tone_);
    setTopMotorsChipState(top_motors_text_, top_motors_tone_);
    updateCoveragePlanningUi();
}

void PlannerScreen::applyStepperButtonStyle(QPushButton* button) const {
    if (!button) {
        return;
    }
    const QString bg = dark_mode_ ? QStringLiteral("#27272A") : QStringLiteral("#FFFFFF");
    const QString hover_bg = dark_mode_ ? QStringLiteral("#3F3F47") : QStringLiteral("#F4F4F5");
    const QString pressed_bg = dark_mode_ ? QStringLiteral("#1F1F23") : QStringLiteral("#E4E4E7");
    const QString border = dark_mode_ ? QStringLiteral("#3F3F47") : QStringLiteral("#D4D4D8");
    const QString disabled_bg = dark_mode_ ? QStringLiteral("#1F1F23") : QStringLiteral("#F4F4F5");
    button->setStyleSheet(
        QStringLiteral("QPushButton[plannerStepper=\"true\"] { background: %1; border: 1px solid "
                       "%2; border-radius: 8px; padding: 0; } "
                       "QPushButton[plannerStepper=\"true\"]:hover { background: %3; } "
                       "QPushButton[plannerStepper=\"true\"]:pressed { background: %4; } "
                       "QPushButton[plannerStepper=\"true\"]:disabled { background: %5; }")
            .arg(bg, border, hover_bg, pressed_bg, disabled_bg));
}

void PlannerScreen::refreshStepperButtons() {
    const QString icon_color =
        dark_mode_ ? QStringLiteral("#D4D4D8") : QStringLiteral("#3F3F47");
    for (const auto& entry : stepper_buttons_) {
        if (!entry.button) {
            continue;
        }
        applyStepperButtonStyle(entry.button);
        if (entry.icon) {
            entry.icon->setPixmap(loadSvgPixmap(entry.icon_path, 14, 14, icon_color));
        }
    }
}

void PlannerScreen::setRobotId(const QString& robot_id) {
    robot_id_ = robot_id.trimmed();
    restoreCurrentSession();
}

void PlannerScreen::setMapPath(const QString& map_path) {
    map_path_ = QFileInfo(map_path.trimmed()).absoluteFilePath();
    if (map_path.trimmed().isEmpty()) {
        map_path_.clear();
    }
    restoreCurrentSession();
}

void PlannerScreen::setLiveRobotTelemetry(const std::optional<PathState>& pose,
                                          const std::vector<Point2D>& trail) {
    live_robot_pose_ = pose;
    live_robot_trail_ = trail;
    applyLiveOverlayToPlot();
    if (current_step_ != PlannerStep::Scan) {
        return;
    }

    SessionCache& cache = activeSession();
    if (cache.scan_run_state == ScanRunState::Running && pose.has_value()) {
        const QDateTime now = QDateTime::currentDateTime();
        if (last_telemetry_valid_) {
            const double dx = pose->point.x - last_telemetry_xy_.x;
            const double dy = pose->point.y - last_telemetry_xy_.y;
            const double ds = std::hypot(dx, dy);
            cache.scan_distance_traveled_m += ds;
        }
        last_telemetry_ts_ = now;
        last_telemetry_xy_ = pose->point;
        last_telemetry_valid_ = true;

        const int idx = cache.scan_active_segment_index;
        if (idx >= 0 && idx < static_cast<int>(cache.scan_segments.size())) {
            auto& seg = cache.scan_segments[static_cast<size_t>(idx)];
            if (!seg.completed && seg.length_m > 1e-6 && seg.path.size() >= 2) {
                // Progress = projection of current robot pose along active segment polyline.
                double best_dist = std::numeric_limits<double>::max();
                double along_at_best = 0.0;
                double cumulative = 0.0;
                size_t start_i = scan_active_segment_path_hint_;
                if (start_i >= seg.path.size()) {
                    start_i = 0;
                }
                for (size_t pass = 0; pass < 2; ++pass) {
                    size_t begin = (pass == 0 && seg.path.size() > 12)
                                       ? (start_i > 6 ? start_i - 6 : 0)
                                       : 0;
                    size_t end = (pass == 0 && seg.path.size() > 12)
                                     ? std::min(seg.path.size() - 1, start_i + 6)
                                     : seg.path.size() - 1;
                    cumulative = 0.0;
                    for (size_t i = 0; i + 1 < seg.path.size(); ++i) {
                        const auto& a = seg.path[i].point;
                        const auto& b = seg.path[i + 1].point;
                        const double dx = b.x - a.x;
                        const double dy = b.y - a.y;
                        const double len = std::hypot(dx, dy);
                        if (i < begin || i >= end || len < 1e-9) {
                            cumulative += len;
                            continue;
                        }
                        const double t = std::clamp(
                            ((pose->point.x - a.x) * dx + (pose->point.y - a.y) * dy) /
                                (len * len),
                            0.0,
                            1.0);
                        const double px = a.x + t * dx;
                        const double py = a.y + t * dy;
                        const double dist = std::hypot(pose->point.x - px, pose->point.y - py);
                        if (dist < best_dist) {
                            best_dist = dist;
                            along_at_best = cumulative + t * len;
                            scan_active_segment_path_hint_ = i;
                        }
                        cumulative += len;
                    }
                    if (best_dist <= 1.5 || pass == 1) {
                        break;
                    }
                }
                const double pct = std::clamp((along_at_best / seg.length_m) * 100.0, 0.0, 99.5);
                seg.progress_pct = std::max(seg.progress_pct, pct);
            }
        }

        maybeScheduleScanQualityUpdate();
        recomputeScanAggregateStats();
    }

    updateScanRunUi();
}

void PlannerScreen::setLiveRobotSpeedMps(double speed_mps) {
    // Clamp negatives to zero — we treat speed as scalar magnitude. The
    // odometry pipe in AppShellWindow already feeds |v|, but be defensive.
    const double clamped = (std::isfinite(speed_mps) && speed_mps > 0.0) ? speed_mps : 0.0;
    if (live_robot_speed_mps_ == clamped) {
        return;
    }
    live_robot_speed_mps_ = clamped;
    // ETA only refreshes when updateCoveragePlanningUi() runs (on regenerate
    // / step change). Don't churn UI on every odom tick — value is a pure
    // estimate, not a live readout.
}

double PlannerScreen::effectiveScanSpeedMps() const {
    // ETA priority:
    //   1) Live odom |v| while a scan is actually running — the ground truth.
    //   2) The configured slider value (cache.coverage_scan_speed_mps) —
    //      which is exactly what the controller will be told to cruise at on
    //      the next start-scan.
    //   3) Hard-coded 0.4 m/s default that matches
    //      `declare_parameter("max_linear_velocity", 0.4)` in
    //      mpc_accel_autonomous_controller.py.
    const SessionCache* cache = activeSessionPtr();
    const bool running = cache && cache->scan_run_state == ScanRunState::Running;
    if (running && live_robot_speed_mps_ > 0.0) {
        return live_robot_speed_mps_;
    }
    if (cache && std::isfinite(cache->coverage_scan_speed_mps) &&
        cache->coverage_scan_speed_mps > 0.0) {
        return cache->coverage_scan_speed_mps;
    }
    return 0.4;
}

bool PlannerScreen::eventFilter(QObject* watched, QEvent* event) {
    if (!event) {
        return QWidget::eventFilter(watched, event);
    }

    // Scan-Splitting list shortcuts: Space toggles current row,
    // Ctrl+A selects all (skips completed/locked), Esc clears all.
    // Arrow keys fall through to QListWidget default navigation.
    if (watched == list_scan_segments_ && event->type() == QEvent::KeyPress) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        if (key_event->key() == Qt::Key_Space) {
            if (list_scan_segments_) {
                const int row = list_scan_segments_->currentRow();
                SessionCache& cache = activeSession();
                if (row >= 0 && row < list_scan_segments_->count() &&
                    row < static_cast<int>(cache.scan_segments.size()) &&
                    !cache.scan_segments[static_cast<size_t>(row)].completed) {
                    auto* item = list_scan_segments_->item(row);
                    if (item) {
                        item->setSelected(!item->isSelected());
                    }
                }
            }
            key_event->accept();
            return true;
        }
        if (key_event->matches(QKeySequence::SelectAll)) {
            SessionCache& cache = activeSession();
            for (auto& seg : cache.scan_segments) {
                if (!seg.completed) {
                    seg.selected = true;
                }
            }
            refreshScanSegmentList();
            pushScanSegmentsToPlot();
            updateScanSplittingUi();
            key_event->accept();
            return true;
        }
        if (key_event->key() == Qt::Key_Escape) {
            SessionCache& cache = activeSession();
            for (auto& seg : cache.scan_segments) {
                seg.selected = false;
            }
            refreshScanSegmentList();
            pushScanSegmentsToPlot();
            updateScanSplittingUi();
            key_event->accept();
            return true;
        }
    }

    if (current_step_ == PlannerStep::Scan && event->type() == QEvent::MouseButtonPress) {
        QWidget* clicked_widget = nullptr;
        if (watched == this) {
            const auto* mouse_event = static_cast<const QMouseEvent*>(event);
            clicked_widget = childAt(mouse_event->pos());
        } else {
            clicked_widget = qobject_cast<QWidget*>(watched);
        }
        if (clicked_widget) {
            if (isDescendantOfScanCamera(clicked_widget)) {
                onScanCameraClicked();
            } else if (isDescendantOfScanMap(clicked_widget)) {
                onScanMapClicked();
            }
        }
    } else if (event->type() == QEvent::KeyPress && watched != this) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        keyPressEvent(key_event);
        if (key_event->isAccepted()) {
            return true;
        }
    } else if (event->type() == QEvent::KeyRelease && watched != this) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        keyReleaseEvent(key_event);
        if (key_event->isAccepted()) {
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void PlannerScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (current_step_ != PlannerStep::Scan || !scan_camera_view_) {
        return;
    }
    if (scan_camera_stream_requested_ && !scan_camera_view_->isPlaying()) {
        scan_camera_view_->startStream(5600);
    }
}

void PlannerScreen::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (current_step_ != PlannerStep::Scan || !scan_camera_view_) {
        return;
    }
    if (scan_camera_stream_requested_ || scan_camera_view_->isPlaying()) {
        scan_camera_view_->stopStream();
    }
}

void PlannerScreen::keyPressEvent(QKeyEvent* event) {
    if (!event) {
        return;
    }
    if (event->isAutoRepeat()) {
        event->ignore();
        return;
    }
    if (event->key() == Qt::Key_Space && current_step_ == PlannerStep::Scan) {
        onScanEmergencyStopClicked();
        event->accept();
        return;
    }
    if (!scanManualTeleopAllowed()) {
        QWidget::keyPressEvent(event);
        return;
    }

    switch (event->key()) {
        case Qt::Key_W:
            scan_key_w_down_ = true;
            emitScanTeleopTwistCommand();
            event->accept();
            return;
        case Qt::Key_A:
            scan_key_a_down_ = true;
            emitScanTeleopTwistCommand();
            event->accept();
            return;
        case Qt::Key_S:
            scan_key_s_down_ = true;
            emitScanTeleopTwistCommand();
            event->accept();
            return;
        case Qt::Key_D:
            scan_key_d_down_ = true;
            emitScanTeleopTwistCommand();
            event->accept();
            return;
        case Qt::Key_E:
            emit scanTeleopArmRequested();
            event->accept();
            return;
        case Qt::Key_Q:
            emit scanTeleopDisarmRequested();
            event->accept();
            return;
        case Qt::Key_O:
            emit scanTeleopGprPowerOffRequested();
            event->accept();
            return;
        case Qt::Key_0:
            scan_teleop_angular_speed_rps_ = std::min(scan_teleop_angular_speed_max_rps_,
                                                      scan_teleop_angular_speed_rps_ +
                                                          scan_teleop_angular_speed_step_rps_);
            updateScanManualOverrideIndicator();
            event->accept();
            return;
        case Qt::Key_9:
            scan_teleop_angular_speed_rps_ = std::max(scan_teleop_angular_speed_min_rps_,
                                                      scan_teleop_angular_speed_rps_ -
                                                          scan_teleop_angular_speed_step_rps_);
            updateScanManualOverrideIndicator();
            event->accept();
            return;
        default:
            break;
    }

    QWidget::keyPressEvent(event);
}

void PlannerScreen::keyReleaseEvent(QKeyEvent* event) {
    if (!event) {
        return;
    }
    if (event->isAutoRepeat()) {
        event->ignore();
        return;
    }

    bool handled = false;
    switch (event->key()) {
        case Qt::Key_W:
            scan_key_w_down_ = false;
            handled = true;
            break;
        case Qt::Key_A:
            scan_key_a_down_ = false;
            handled = true;
            break;
        case Qt::Key_S:
            scan_key_s_down_ = false;
            handled = true;
            break;
        case Qt::Key_D:
            scan_key_d_down_ = false;
            handled = true;
            break;
        default:
            break;
    }

    if (handled) {
        if (scanManualTeleopAllowed()) {
            emitScanTeleopTwistCommand();
        } else {
            emitScanZeroTeleopTwist();
        }
        event->accept();
        return;
    }

    QWidget::keyReleaseEvent(event);
}

void PlannerScreen::onBackClicked() {
    emit backRequested();
}

void PlannerScreen::onNextClicked() {
    const SessionCache* cache = activeSessionPtr();
    const bool should_complete_from_next =
        current_step_ == PlannerStep::Scan && cache &&
        (cache->scan_run_state == ScanRunState::Completed || scan_manual_override_engaged_once_);
    if (current_step_ == PlannerStep::MapProcessing) {
        navigateToStep(PlannerStep::CoveragePlanning);
    } else if (current_step_ == PlannerStep::CoveragePlanning) {
        if (kBypassPlannerStageGates || (cache && cache->planning_complete)) {
            navigateToStep(PlannerStep::ScanSplitting);
        }
    } else if (current_step_ == PlannerStep::ScanSplitting) {
        if (kBypassPlannerStageGates || (cache && cache->scan_waypoints_published)) {
            enterScanStage();
        }
    } else if (current_step_ == PlannerStep::Scan) {
        if (should_complete_from_next) {
            onCompleteMissionClicked("onNextClicked");
        }
    }
}

QString PlannerScreen::sessionKey() const {
    const QString robot_key =
        robot_id_.isEmpty() ? QStringLiteral("no-robot") : robot_id_.trimmed();
    const QString map_key =
        map_path_.isEmpty() ? QStringLiteral("no-map") : QFileInfo(map_path_).absoluteFilePath();
    return robot_key + QStringLiteral("::") + map_key;
}

QString PlannerScreen::settingsGroupKey() const {
    return QString::fromUtf8(
        QCryptographicHash::hash(sessionKey().toUtf8(), QCryptographicHash::Sha1).toHex());
}

PlannerScreen::SessionCache& PlannerScreen::activeSession() {
    return session_cache_[sessionKey()];
}

const PlannerScreen::SessionCache* PlannerScreen::activeSessionPtr() const {
    const auto it = session_cache_.constFind(sessionKey());
    return it == session_cache_.constEnd() ? nullptr : &it.value();
}

void PlannerScreen::loadPersistedParameters(SessionCache& cache) const {
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    settings.beginGroup(QStringLiteral("planner/mission_planner"));
    settings.beginGroup(settingsGroupKey());
    if (settings.contains(QStringLiteral("voxel_size"))) {
        cache.voxel_size = settings.value(QStringLiteral("voxel_size")).toDouble();
    }
    if (settings.contains(QStringLiteral("z_min"))) {
        cache.z_min = settings.value(QStringLiteral("z_min")).toDouble();
    }
    if (settings.contains(QStringLiteral("z_max"))) {
        cache.z_max = settings.value(QStringLiteral("z_max")).toDouble();
    }
    if (settings.contains(QStringLiteral("alpha"))) {
        cache.alpha = settings.value(QStringLiteral("alpha")).toDouble();
    }
    if (settings.contains(QStringLiteral("coverage_pattern"))) {
        cache.coverage_pattern = settings.value(QStringLiteral("coverage_pattern")).toString();
    }
    if (settings.contains(QStringLiteral("coverage_scan_mode"))) {
        cache.coverage_scan_mode = settings.value(QStringLiteral("coverage_scan_mode")).toString();
    }
    if (settings.contains(QStringLiteral("coverage_path_spacing"))) {
        cache.coverage_path_spacing =
            settings.value(QStringLiteral("coverage_path_spacing")).toDouble();
    }
    if (settings.contains(QStringLiteral("coverage_headland_width"))) {
        cache.coverage_headland_width =
            settings.value(QStringLiteral("coverage_headland_width")).toDouble();
    }
    if (settings.contains(QStringLiteral("coverage_scan_axis"))) {
        cache.coverage_scan_axis = settings.value(QStringLiteral("coverage_scan_axis")).toString();
    }
    if (settings.contains(QStringLiteral("coverage_scan_speed_mps"))) {
        cache.coverage_scan_speed_mps =
            settings.value(QStringLiteral("coverage_scan_speed_mps")).toDouble();
    }
    // Per-robot key kept only for one-time migration (see global block below).
    QString legacy_per_robot_selected;
    if (settings.contains(QStringLiteral("coverage_selected_preset"))) {
        legacy_per_robot_selected =
            settings.value(QStringLiteral("coverage_selected_preset")).toString();
    }
    if (settings.contains(QStringLiteral("coverage_roi_drawing_tool"))) {
        cache.coverage_roi_drawing_tool =
            settings.value(QStringLiteral("coverage_roi_drawing_tool")).toString();
    }
    if (settings.contains(QStringLiteral("coverage_obstacle_mode"))) {
        cache.coverage_obstacle_mode =
            settings.value(QStringLiteral("coverage_obstacle_mode")).toString();
    }
    if (settings.contains(QStringLiteral("coverage_drawing_tool"))) {
        cache.coverage_drawing_tool = settings.value(QStringLiteral("coverage_drawing_tool")).toString();
    }
    if (settings.contains(QStringLiteral("scan_distance_m"))) {
        cache.scan_distance_m =
            settings.value(QStringLiteral("scan_distance_m")).toDouble();
    }
    if (settings.contains(QStringLiteral("scan_progression_mode"))) {
        cache.scan_progression_mode =
            settings.value(QStringLiteral("scan_progression_mode")).toString();
    }
    settings.endGroup();
    settings.endGroup();

    // Selected preset name is global across robots (one preset library shared
    // by every operator/robot). On first launch after the per-robot → global
    // migration we copy any non-default per-robot value forward, then clear
    // the legacy key so subsequent writes don't drift.
    QSettings global_settings(kSettingsOrgName, kSettingsAppName);
    QString global_selected =
        global_settings.value(QLatin1String(kGlobalSelectedPresetKey)).toString();
    if (global_selected.isEmpty()) {
        if (!legacy_per_robot_selected.isEmpty()) {
            global_settings.setValue(QLatin1String(kGlobalSelectedPresetKey),
                                     legacy_per_robot_selected);
            global_selected = legacy_per_robot_selected;
        } else {
            global_selected = QStringLiteral("Standard");
        }
    }
    cache.coverage_selected_preset = global_selected;
    // Drop the legacy per-robot key — the global key is authoritative now.
    if (!legacy_per_robot_selected.isEmpty()) {
        QSettings cleanup(kSettingsOrgName, kSettingsAppName);
        cleanup.beginGroup(QStringLiteral("planner/mission_planner"));
        cleanup.beginGroup(settingsGroupKey());
        cleanup.remove(QStringLiteral("coverage_selected_preset"));
        cleanup.endGroup();
        cleanup.endGroup();
    }

    ensureCoverageDefaults(cache);
}

void PlannerScreen::persistParameters() const {
    const SessionCache* cache = activeSessionPtr();
    if (!cache) {
        return;
    }

    QSettings settings(kSettingsOrgName, kSettingsAppName);
    settings.beginGroup(QStringLiteral("planner/mission_planner"));
    settings.beginGroup(settingsGroupKey());
    settings.setValue(QStringLiteral("robot_id"), robot_id_);
    settings.setValue(QStringLiteral("map_path"), map_path_);
    settings.setValue(QStringLiteral("voxel_size"), cache->voxel_size);
    settings.setValue(QStringLiteral("z_min"), cache->z_min);
    settings.setValue(QStringLiteral("z_max"), cache->z_max);
    settings.setValue(QStringLiteral("alpha"), cache->alpha);
    settings.setValue(QStringLiteral("coverage_scan_mode"), cache->coverage_scan_mode);
    settings.setValue(QStringLiteral("coverage_pattern"), cache->coverage_pattern);
    settings.setValue(QStringLiteral("coverage_path_spacing"), cache->coverage_path_spacing);
    settings.setValue(QStringLiteral("coverage_headland_width"), cache->coverage_headland_width);
    settings.setValue(QStringLiteral("coverage_scan_axis"), cache->coverage_scan_axis);
    settings.setValue(QStringLiteral("coverage_scan_speed_mps"), cache->coverage_scan_speed_mps);
    // Selected preset name is intentionally NOT written here — it's stored
    // globally below so a preset selection survives switching robots.
    settings.setValue(QStringLiteral("coverage_roi_drawing_tool"), cache->coverage_roi_drawing_tool);
    settings.setValue(QStringLiteral("coverage_obstacle_mode"), cache->coverage_obstacle_mode);
    settings.setValue(QStringLiteral("coverage_drawing_tool"), cache->coverage_drawing_tool);
    settings.setValue(QStringLiteral("scan_distance_m"), cache->scan_distance_m);
    settings.setValue(QStringLiteral("scan_progression_mode"), cache->scan_progression_mode);
    settings.endGroup();
    settings.endGroup();

    // Global selected-preset key (shared library across robots).
    QSettings global_settings(kSettingsOrgName, kSettingsAppName);
    global_settings.setValue(QLatin1String(kGlobalSelectedPresetKey),
                             cache->coverage_selected_preset);
}

void PlannerScreen::persistCurrentStep() {
    activeSession().last_step = current_step_;
}

void PlannerScreen::ensureCoverageDefaults(SessionCache& cache) const {
    if (cache.coverage_presets.empty()) {
        // Lazy populate: factories from code + customs from disk via
        // PresetManager. Real refresh on save/delete is driven by the
        // presetsChanged signal connection wired in the ctor.
        reloadCoveragePresetsFromDisk(cache);
    }

    cache.coverage_path_spacing =
        clampValue(cache.coverage_path_spacing, kCoveragePathSpacingMin, kCoveragePathSpacingMax);
    cache.coverage_headland_width =
        clampValue(cache.coverage_headland_width, kCoverageHeadlandMin, kCoverageHeadlandMax);
    if (!std::isfinite(cache.coverage_scan_speed_mps) || cache.coverage_scan_speed_mps <= 0.0) {
        cache.coverage_scan_speed_mps = kCoverageScanSpeedDefault;
    }
    cache.coverage_scan_speed_mps =
        clampValue(cache.coverage_scan_speed_mps, kCoverageScanSpeedMin, kCoverageScanSpeedMax);

    if (cache.coverage_scan_mode.isEmpty()) {
        cache.coverage_scan_mode = QStringLiteral("complete");
    }
    if (cache.coverage_pattern.isEmpty()) {
        cache.coverage_pattern = QStringLiteral("boustro");
    }
    if (cache.coverage_scan_axis.isEmpty()) {
        cache.coverage_scan_axis = QStringLiteral("parallel");
    }
    if (cache.coverage_roi_drawing_tool.isEmpty()) {
        cache.coverage_roi_drawing_tool = QStringLiteral("rectangle");
    }
    if (cache.coverage_obstacle_mode.isEmpty()) {
        cache.coverage_obstacle_mode = QStringLiteral("automatic");
    }
    if (cache.coverage_drawing_tool.isEmpty()) {
        cache.coverage_drawing_tool = QStringLiteral("rectangle");
    }
    if (cache.coverage_selected_preset.isEmpty()) {
        cache.coverage_selected_preset = QStringLiteral("Standard");
    }

    const auto preset_it =
        std::find_if(cache.coverage_presets.begin(),
                     cache.coverage_presets.end(),
                     [&cache](const SessionCache::CoveragePreset& preset) {
                         return preset.name == cache.coverage_selected_preset;
                     });
    if (preset_it == cache.coverage_presets.end()) {
        cache.coverage_selected_preset = QStringLiteral("Standard");
    }
}

// =============================================================================
// Preset persistence (Plan B). Factory presets are immutable code constants;
// customs live one-JSON-per-file under PresetManager::presetsDir(). The
// SessionCache::coverage_presets vector is a derived cache populated by
// reloadCoveragePresetsFromDisk() and refreshed on PresetManager's
// presetsChanged signal.
// =============================================================================

const std::vector<PlannerScreen::SessionCache::CoveragePreset>&
PlannerScreen::factoryPresets() {
    // {name, route_pattern, path_spacing, headland_width, scan_axis,
    //  scan_speed_mps, custom}. Speeds slot into the [0.30, 0.60] m/s slider
    // envelope; pick conservative defaults per factory profile.
    static const std::vector<SessionCache::CoveragePreset> kFactories = {
        {QStringLiteral("Standard"),        QStringLiteral("boustro"), 0.50, 0.30,
         QStringLiteral("parallel"),        0.40, false},
        {QStringLiteral("Fast Scan"),       QStringLiteral("snake"),   0.80, 0.20,
         QStringLiteral("parallel"),        0.60, false},
        {QStringLiteral("Detailed Survey"), QStringLiteral("boustro"), 0.25, 0.40,
         QStringLiteral("perpendicular"),   0.30, false},
        {QStringLiteral("Warehouse"),       QStringLiteral("snake"),   0.60, 0.50,
         QStringLiteral("parallel"),        0.40, false},
    };
    return kFactories;
}

bool PlannerScreen::isFactoryPresetName(const QString& name) {
    for (const auto& f : factoryPresets()) {
        if (f.name == name) return true;
    }
    return false;
}

void PlannerScreen::reloadCoveragePresetsFromDisk(SessionCache& cache) const {
    cache.coverage_presets.clear();
    for (const auto& f : factoryPresets()) {
        cache.coverage_presets.push_back(f);
    }
    if (!preset_manager_) {
        return;
    }
    const QStringList disk_names = preset_manager_->availablePresets();
    for (const QString& name : disk_names) {
        // Defensive: factory names should never appear on disk because the
        // save handler rejects them, but guard anyway in case the user copied
        // a JSON manually.
        if (isFactoryPresetName(name)) {
            continue;
        }
        const PlanningPreset p = preset_manager_->loadPreset(name);
        if (!p.isValid()) {
            continue;
        }
        SessionCache::CoveragePreset cp;
        cp.name           = p.name;
        cp.route_pattern  = canonicalToUiPattern(p.route_pattern);
        cp.path_spacing   = p.swath_width;
        cp.headland_width = p.headland_width;
        cp.scan_axis      = directionToUiAxis(p.direction);
        cp.scan_speed_mps =
            (std::isfinite(p.robot_speed) && p.robot_speed > 0.0)
                ? clampValue(p.robot_speed, kCoverageScanSpeedMin, kCoverageScanSpeedMax)
                : kCoverageScanSpeedDefault;
        cp.custom         = true;
        cache.coverage_presets.push_back(cp);
    }
}

PlanningPreset PlannerScreen::buildPlanningPresetFromSession(
    const SessionCache& cache) const {
    // Seed from PresetManager::defaultPreset() so untouched fields (filtering,
    // hull, turn_radius, decomposition…) get sensible JSON values today, and
    // the on-disk schema stays forward-compatible if the UI later exposes
    // those knobs.
    PlanningPreset p = PresetManager::defaultPreset();
    p.route_pattern  = uiPatternToCanonical(cache.coverage_pattern);
    p.swath_width    = cache.coverage_path_spacing;
    p.headland_width = cache.coverage_headland_width;
    p.direction      = uiAxisToDirection(cache.coverage_scan_axis);
    p.robot_speed    = cache.coverage_scan_speed_mps;
    return p;
}

void PlannerScreen::applyPlanningPresetToSession(const PlanningPreset& p,
                                                 SessionCache& cache) const {
    cache.coverage_pattern         = canonicalToUiPattern(p.route_pattern);
    cache.coverage_path_spacing    = p.swath_width;
    cache.coverage_headland_width  = p.headland_width;
    cache.coverage_scan_axis       = directionToUiAxis(p.direction);
    if (std::isfinite(p.robot_speed) && p.robot_speed > 0.0) {
        cache.coverage_scan_speed_mps =
            clampValue(p.robot_speed, kCoverageScanSpeedMin, kCoverageScanSpeedMax);
    }
}

void PlannerScreen::invalidateProcessingResult(const QString& status_message) {
    SessionCache& cache = activeSession();
    cache.processing_complete = false;
    cache.hull_complete = false;
    cache.processed_cloud.reset();
    cache.processed_projected_points.clear();
    cache.hull_polygon.clear();
    cache.processed_point_count = 0;
    cache.processed_area_estimate_m2 = 0.0;
    cache.hull_area_m2 = 0.0;
    cache.estimated_file_size_mb = 0.0;
    cache.quality_label.clear();
    invalidateCoverageResult();
    cache.coverage_roi_polygon.clear();
    cache.coverage_roi_drawing_active = false;
    cache.coverage_obstacles_detected = false;
    cache.coverage_obstacles.clear();
    cache.coverage_drawing_active = false;

    updatePreview();
    updateOutputCards();
    updateStatsChip();
    updatePlaceholderMessage();
    updateButtonsAndStatus();
    if (!status_message.isEmpty()) {
        setInlineStatus(status_message, QStringLiteral("#71717B"));
    }
}

void PlannerScreen::invalidateHullResult(const QString& status_message) {
    SessionCache& cache = activeSession();
    cache.hull_complete = false;
    cache.hull_polygon.clear();
    cache.hull_area_m2 = 0.0;
    invalidateCoverageResult();
    cache.coverage_roi_polygon.clear();
    cache.coverage_roi_drawing_active = false;
    cache.coverage_obstacles_detected = false;
    cache.coverage_obstacles.clear();
    cache.coverage_drawing_active = false;

    updatePreview();
    updateStatsChip();
    updatePlaceholderMessage();
    updateButtonsAndStatus();
    if (!status_message.isEmpty()) {
        setInlineStatus(status_message, QStringLiteral("#71717B"));
    }
}

void PlannerScreen::invalidateCoverageResult(const QString& status_message) {
    SessionCache& cache = activeSession();
    cache.planning_complete = false;
    cache.planned_swaths.clear();
    cache.planned_route.clear();
    cache.planned_path.clear();
    cache.planned_effective_area_m2 = 0.0;
    cache.scan_segments.clear();
    cache.scan_splits_dirty = true;
    cache.scan_waypoints_published = false;

    if (!status_message.isEmpty()) {
        setInlineStatus(status_message, QStringLiteral("#71717B"));
    }
}

void PlannerScreen::updateValueLabels() {
    const SessionCache& cache = activeSession();
    if (lbl_voxel_value_) {
        lbl_voxel_value_->setText(formatMeters(cache.voxel_size));
    }
    if (lbl_z_min_value_) {
        lbl_z_min_value_->setText(formatMeters(cache.z_min));
    }
    if (lbl_z_max_value_) {
        lbl_z_max_value_->setText(formatMeters(cache.z_max));
    }
    if (lbl_alpha_value_) {
        lbl_alpha_value_->setText(formatParameter(cache.alpha));
    }
    if (lbl_coverage_path_spacing_value_) {
        lbl_coverage_path_spacing_value_->setText(formatMeters(cache.coverage_path_spacing));
    }
    if (lbl_coverage_headland_value_) {
        lbl_coverage_headland_value_->setText(formatMeters(cache.coverage_headland_width));
    }
    if (lbl_coverage_scan_speed_value_) {
        lbl_coverage_scan_speed_value_->setText(
            QString::asprintf("%.2f m/s", cache.coverage_scan_speed_mps));
    }
}

void PlannerScreen::updatePreview() {
    if (!preview_stack_ || !plot_ || !preview_placeholder_) {
        return;
    }

    if (preview_bottom_overlay_stack_) {
        preview_bottom_overlay_stack_->setCurrentWidget(
            current_step_ == PlannerStep::MapProcessing ? stats_chip_ : coverage_legend_chip_);
    }

    const QFileInfo map_info(map_path_);
    const SessionCache* cache = activeSessionPtr();
    const bool map_valid = !map_path_.isEmpty() && map_info.exists();
    if (!map_valid || !cache || !cache->raw_loaded) {
        plot_->clearAll();
        preview_stack_->setCurrentWidget(preview_placeholder_);
        return;
    }

    const bool was_showing_plot = preview_stack_->currentWidget() == plot_;
    plot_->clearAll();

    if (current_step_ == PlannerStep::CoveragePlanning ||
        current_step_ == PlannerStep::ScanSplitting ||
        current_step_ == PlannerStep::Scan) {
        if (cache->processing_complete) {
            plot_->setPoints(cache->processed_projected_points);
        } else {
            plot_->setPoints(cache->raw_projected_points);
        }
        if (cache->hull_complete) {
            plot_->setPolygon(cache->hull_polygon);
            if (cache->coverage_scan_mode == QStringLiteral("roi") &&
                cache->coverage_roi_polygon.size() >= 3) {
                plot_->setROI(cache->coverage_roi_polygon);
            }
            std::vector<Obstacle2D> preview_obstacles;
            preview_obstacles.reserve(cache->coverage_obstacles.size());
            for (const auto& obstacle : cache->coverage_obstacles) {
                if (obstacle.geometry.outer.size() >= 3) {
                    preview_obstacles.push_back(obstacle.geometry);
                }
            }
            if (!preview_obstacles.empty()) {
                plot_->setObstacles(preview_obstacles);
            }
            if (cache->planning_complete) {
                if (current_step_ != PlannerStep::Scan) {
                    plot_->setSwaths(cache->planned_swaths);
                    plot_->setRoute(cache->planned_route);
                }
                plot_->setPath(cache->planned_path);
            }
        }
        if (current_step_ == PlannerStep::ScanSplitting ||
            current_step_ == PlannerStep::Scan) {
            pushScanSegmentsToPlot();
        }
    } else if (cache->processing_complete) {
        plot_->setPoints(cache->processed_projected_points);
        plot_->setPolygon(cache->hull_complete ? cache->hull_polygon : Polygon2D{});
    } else {
        plot_->setPoints(cache->raw_projected_points);
        plot_->setPolygon(Polygon2D{});
    }
    applyLiveOverlayToPlot();
    preview_stack_->setCurrentWidget(plot_);
    if (!was_showing_plot) {
        plot_->resetView();
    }
}

void PlannerScreen::updateOutputCards() {
    const QFileInfo map_info(map_path_);
    const SessionCache* cache = activeSessionPtr();
    const bool map_valid = !map_path_.isEmpty() && map_info.exists();
    if (!map_valid || !cache || !cache->processing_complete) {
        if (lbl_output_points_) {
            lbl_output_points_->setText(QStringLiteral("--"));
        }
        if (lbl_output_reduction_) {
            lbl_output_reduction_->setText(QStringLiteral("--"));
        }
        if (lbl_output_file_size_) {
            lbl_output_file_size_->setText(QStringLiteral("--"));
        }
        if (lbl_output_quality_) {
            lbl_output_quality_->setText(QStringLiteral("--"));
        }
        return;
    }

    if (lbl_output_points_) {
        lbl_output_points_->setText(formatCount(cache->processed_point_count));
    }
    if (lbl_output_reduction_) {
        const double reduction = cache->raw_point_count > 0
                                     ? (1.0 - (static_cast<double>(cache->processed_point_count) /
                                               static_cast<double>(cache->raw_point_count))) *
                                           100.0
                                     : 0.0;
        lbl_output_reduction_->setText(formatPercent(std::max(0.0, reduction)));
    }
    if (lbl_output_file_size_) {
        lbl_output_file_size_->setText(formatFileSize(cache->estimated_file_size_mb));
    }
    if (lbl_output_quality_) {
        lbl_output_quality_->setText(cache->quality_label.isEmpty() ? QStringLiteral("--")
                                                                    : cache->quality_label);
    }
}

void PlannerScreen::updateStatsChip() {
    const QFileInfo map_info(map_path_);
    const SessionCache* cache = activeSessionPtr();
    const bool map_valid = !map_path_.isEmpty() && map_info.exists();
    if (!map_valid || !cache || !cache->raw_loaded) {
        if (lbl_stats_points_) {
            lbl_stats_points_->setText(QStringLiteral("--"));
        }
        if (lbl_stats_area_) {
            lbl_stats_area_->setText(QStringLiteral("--"));
        }
        return;
    }

    qsizetype points = cache->raw_point_count;
    double area_m2 = cache->raw_area_estimate_m2;
    if (cache->processing_complete) {
        points = cache->processed_point_count;
        area_m2 = cache->processed_area_estimate_m2;
    }
    if (cache->hull_complete) {
        area_m2 = cache->hull_area_m2;
    }

    if (lbl_stats_points_) {
        lbl_stats_points_->setText(formatCount(points));
    }
    if (lbl_stats_area_) {
        lbl_stats_area_->setText(area_m2 > 0.0 ? formatArea(area_m2) : QStringLiteral("--"));
    }
}

void PlannerScreen::updatePlaceholderMessage() {
    if (!lbl_stage2_message_) {
        return;
    }

    const QFileInfo map_info(map_path_);
    const SessionCache* cache = activeSessionPtr();
    if (map_path_.isEmpty() || !map_info.exists()) {
        lbl_stage2_message_->setVisible(true);
        lbl_stage2_message_->setText(
            QStringLiteral("A saved exploration map is still required before coverage planning can begin."));
        return;
    }

    QString message;
    if (load_in_flight_) {
        message = QStringLiteral("Loading the saved map from exploration...");
    } else if (!cache || !cache->raw_loaded) {
        message = QStringLiteral("Waiting for the saved map to finish loading.");
    } else if (!cache->processing_complete) {
        message = QStringLiteral("Process the point cloud in Stage 1 to unlock coverage planning.");
    } else if (!cache->hull_complete) {
        message = QStringLiteral("Compute and project the hull in Stage 1 to establish the planning boundary.");
    } else if (planning_in_flight_) {
        message = QStringLiteral("Generating coverage paths from the projected hull...");
    } else if (cache->planning_complete) {
        message = QStringLiteral("Coverage paths are ready. Adjust the scan configuration and regenerate as needed.");
    } else {
        message = QStringLiteral("Boundary ready. Adjust the scan configuration and generate coverage paths.");
    }

    lbl_stage2_message_->setVisible(true);
    lbl_stage2_message_->setText(message);
}

void PlannerScreen::refreshCoveragePresetCombo() {
    if (!combo_coverage_presets_) {
        return;
    }

    const SessionCache& cache = activeSession();
    QSignalBlocker blocker(combo_coverage_presets_);
    combo_coverage_presets_->clear();

    // Factories pinned at top in canonical order.
    for (const auto& f : factoryPresets()) {
        combo_coverage_presets_->addItem(f.name, f.name);
    }

    // Customs sorted A->Z, separated visually from factories.
    QStringList custom_names;
    for (const auto& p : cache.coverage_presets) {
        if (p.custom) {
            custom_names << p.name;
        }
    }
    custom_names.sort(Qt::CaseInsensitive);
    if (!custom_names.isEmpty()) {
        combo_coverage_presets_->insertSeparator(combo_coverage_presets_->count());
        for (const QString& name : custom_names) {
            combo_coverage_presets_->addItem(name + QStringLiteral(" (Custom)"), name);
        }
    }

    const int idx = combo_coverage_presets_->findData(cache.coverage_selected_preset);
    if (idx >= 0) {
        combo_coverage_presets_->setCurrentIndex(idx);
    }
}

void PlannerScreen::rebuildCoveragePresetRows() {
    if (!coverage_custom_presets_card_ || !coverage_custom_presets_layout_) {
        return;
    }

    clearLayout(coverage_custom_presets_layout_);

    SessionCache& cache = activeSession();
    ensureCoverageDefaults(cache);

    const QString row_bg = dark_mode_ ? QStringLiteral("rgba(39,39,42,0.55)")
                                      : QStringLiteral("#F8FAFC");
    const QString row_border = dark_mode_ ? QStringLiteral("#27272A")
                                          : QStringLiteral("#E4E4E7");
    const QString text = dark_mode_ ? QStringLiteral("#D4D4D8")
                                    : QStringLiteral("#374151");
    const QString muted = dark_mode_ ? QStringLiteral("#71717B")
                                     : QStringLiteral("#6B7280");

    bool has_custom = false;
    for (const auto& preset : cache.coverage_presets) {
        if (!preset.custom) {
            continue;
        }

        has_custom = true;
        auto* row = new QWidget(coverage_custom_presets_card_);
        row->setObjectName(QStringLiteral("PresetRow"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        // Selector scoped to #PresetRow so background/border/radius do not
        // cascade onto child SvgIconButtons (which would inherit the 1px
        // border + radius and clip the 24x24 tinted icon to nothing).
        row->setStyleSheet(
            QStringLiteral(
                "QWidget#PresetRow { background: %1; border: 1px solid %2; "
                "border-radius: 8px; }")
                .arg(row_bg, row_border));
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(10, 8, 8, 8);
        row_layout->setSpacing(8);

        auto* text_col = new QVBoxLayout();
        text_col->setContentsMargins(0, 0, 0, 0);
        text_col->setSpacing(0);
        auto* name_label = makeTextLabel(
            row,
            preset.name,
            QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 700; color: %1;")
                .arg(text));
        auto* meta_label = makeTextLabel(
            row,
            QStringLiteral("%1 | %2m spacing")
                .arg(preset.route_pattern == QStringLiteral("snake") ? QStringLiteral("Snake")
                                                                    : QStringLiteral("Zigzag"))
                .arg(preset.path_spacing, 0, 'f', 2),
            QStringLiteral("font-family: 'Arimo'; font-size: 10px; font-weight: 400; color: %1;")
                .arg(muted));
        text_col->addWidget(name_label);
        text_col->addWidget(meta_label);
        row_layout->addLayout(text_col, 1);

        // ---- Rename button ----
        // Lucide square-pen icon, runtime-tinted via SvgIconButton. Opens a
        // themed BdrInputDialog prefilled with the current name. Validator
        // blocks empty / unchanged / factory-collision / custom-collision.
        SvgIconButton::Palette rename_palette;
        rename_palette.dark_resting  = QColor("#71717B");
        rename_palette.dark_hover    = QColor("#F59E0B");
        rename_palette.light_resting = QColor("#6B7280");
        rename_palette.light_hover   = QColor("#F59E0B");
        auto* rename_button = new SvgIconButton(
            QStringLiteral(":/assets/missionplanner/preset_rename.svg"),
            rename_palette,
            24,
            row);
        rename_button->setDarkMode(dark_mode_);
        rename_button->setToolTip(QStringLiteral("Rename preset"));
        connect(rename_button, &QPushButton::clicked, this,
                [this, preset_name = preset.name]() {
            if (isFactoryPresetName(preset_name) || !preset_manager_) {
                return;
            }
            // Snapshot the existing custom names (case-sensitive) so the
            // validator doesn't have to re-walk the cache on every keystroke.
            QStringList existing_custom;
            for (const auto& candidate : activeSession().coverage_presets) {
                if (candidate.custom && candidate.name != preset_name) {
                    existing_custom.append(candidate.name);
                }
            }
            auto validator = [preset_name, existing_custom](const QString& candidate)
                -> QString {
                const QString trimmed = candidate.trimmed();
                if (trimmed.isEmpty()) {
                    return QStringLiteral("Name cannot be empty.");
                }
                if (trimmed == preset_name) {
                    // No-op: same name; treat as cancel so we don't churn disk.
                    // Validator returns empty → dialog accepts → caller compares
                    // and short-circuits before calling renamePreset.
                    return QString();
                }
                if (PlannerScreen::isFactoryPresetName(trimmed)) {
                    return QStringLiteral(
                        "'%1' is a built-in preset name and cannot be reused.")
                        .arg(trimmed);
                }
                if (existing_custom.contains(trimmed)) {
                    return QStringLiteral(
                        "A preset named '%1' already exists.").arg(trimmed);
                }
                return QString();
            };
            bool accepted = false;
            const QString new_name = BdrInputDialog::getText(
                this,
                QStringLiteral("Rename preset"),
                QStringLiteral("New name for preset '%1':").arg(preset_name),
                preset_name,
                dark_mode_,
                validator,
                &accepted);
            if (!accepted || new_name.isEmpty() || new_name == preset_name) {
                return;
            }
            if (!preset_manager_->renamePreset(preset_name, new_name)) {
                BdrMessageBox::critical(
                    this,
                    QStringLiteral("Rename failed"),
                    QStringLiteral("Could not rename preset '%1' to '%2'.")
                        .arg(preset_name, new_name));
                return;
            }
            SessionCache& session = activeSession();
            if (session.coverage_selected_preset == preset_name) {
                session.coverage_selected_preset = new_name;
                persistParameters();
            }
            // PresetManager::renamePreset emitted presetsChanged synchronously,
            // so the row list and combo are already rebuilt. Re-sync the rest
            // of the UI in case the selected preset string moved.
            applySessionToUi();
        });
        row_layout->addWidget(rename_button, 0, Qt::AlignVCenter);

        // ---- Delete button ----
        // Same Lucide trash-2 SVG aliased under preset_delete. Gated behind a
        // BdrMessageBox::question to avoid an instant-destroy on a tiny click
        // target.
        SvgIconButton::Palette delete_palette;
        delete_palette.dark_resting  = QColor("#71717B");
        delete_palette.dark_hover    = QColor("#F87171");
        delete_palette.light_resting = QColor("#6B7280");
        delete_palette.light_hover   = QColor("#DC2626");
        auto* delete_button = new SvgIconButton(
            QStringLiteral(":/assets/missionplanner/preset_delete.svg"),
            delete_palette,
            24,
            row);
        delete_button->setDarkMode(dark_mode_);
        delete_button->setToolTip(QStringLiteral("Delete preset"));
        connect(delete_button, &QPushButton::clicked, this,
                [this, preset_name = preset.name]() {
            if (isFactoryPresetName(preset_name) || !preset_manager_) {
                return;
            }
            const int answer = BdrMessageBox::question(
                this,
                QStringLiteral("Delete preset?"),
                QStringLiteral(
                    "Delete preset '%1'? This cannot be undone.").arg(preset_name),
                BdrMessageBox::No);
            if (answer != BdrMessageBox::Yes) {
                return;
            }
            if (!preset_manager_->deletePreset(preset_name)) {
                BdrMessageBox::critical(
                    this,
                    QStringLiteral("Delete failed"),
                    QStringLiteral("Could not delete preset '%1'.").arg(preset_name));
                return;
            }
            SessionCache& session = activeSession();
            if (session.coverage_selected_preset == preset_name) {
                session.coverage_selected_preset = QStringLiteral("Standard");
                const auto& factories = factoryPresets();
                const auto standard_it = std::find_if(
                    factories.begin(), factories.end(),
                    [](const SessionCache::CoveragePreset& candidate) {
                        return candidate.name == QStringLiteral("Standard");
                    });
                if (standard_it != factories.end()) {
                    session.coverage_pattern        = standard_it->route_pattern;
                    session.coverage_path_spacing   = standard_it->path_spacing;
                    session.coverage_headland_width = standard_it->headland_width;
                    session.coverage_scan_axis      = standard_it->scan_axis;
                    session.coverage_scan_speed_mps = standard_it->scan_speed_mps;
                }
                persistParameters();
                applySessionToUi();
            }
        });
        row_layout->addWidget(delete_button, 0, Qt::AlignVCenter);
        coverage_custom_presets_layout_->addWidget(row);
    }

    coverage_custom_presets_card_->setVisible(has_custom);
}

void PlannerScreen::rebuildCoverageObstacleRows() {
    if (coverage_obstacle_list_section_) {
        coverage_obstacle_list_section_->setVisible(false);
    }
}

void PlannerScreen::updateCoveragePlanningUi() {
    if (!combo_coverage_presets_) {
        return;
    }

    SessionCache& cache = activeSession();
    ensureCoverageDefaults(cache);
    if (plot_ && (cache.coverage_roi_drawing_active || cache.coverage_drawing_active) &&
        !plot_->isSelecting() && !plot_->isDrawingRectangle()) {
        cache.coverage_roi_drawing_active = false;
        cache.coverage_drawing_active = false;
    }

    const bool roi_mode = cache.coverage_scan_mode == QStringLiteral("roi");
    const bool has_roi = cache.coverage_roi_polygon.size() >= 3;
    const bool roi_drawing_active = cache.coverage_roi_drawing_active;
    const bool manual_obstacles = cache.coverage_obstacle_mode == QStringLiteral("manual");
    const bool manual_drawing_active = cache.coverage_drawing_active;

    const QString accent = dark_mode_ ? QStringLiteral("#00D492")
                                      : QStringLiteral("#059669");
    const QString accent_hover = dark_mode_ ? QStringLiteral("#0ACB8B")
                                            : QStringLiteral("#10B981");
    const QString accent_text = dark_mode_ ? QStringLiteral("#34D399")
                                           : QStringLiteral("#047857");
    const QString accent_soft_bg = dark_mode_ ? QStringLiteral("rgba(0,188,125,0.15)")
                                              : QStringLiteral("rgba(5,150,105,0.10)");
    const QString accent_soft_border = dark_mode_ ? QStringLiteral("rgba(0,188,125,0.30)")
                                                  : QStringLiteral("rgba(5,150,105,0.22)");
    const QString amber_text = dark_mode_ ? QStringLiteral("#FBBF24")
                                          : QStringLiteral("#B45309");
    const QString amber_soft_bg = dark_mode_ ? QStringLiteral("rgba(245,158,11,0.14)")
                                             : QStringLiteral("rgba(245,158,11,0.12)");
    const QString amber_soft_border = dark_mode_ ? QStringLiteral("rgba(245,158,11,0.28)")
                                                 : QStringLiteral("rgba(217,119,6,0.22)");
    const QString blue = dark_mode_ ? QStringLiteral("#3B82F6")
                                    : QStringLiteral("#2563EB");
    const QString blue_hover = dark_mode_ ? QStringLiteral("#60A5FA")
                                          : QStringLiteral("#1D4ED8");
    const QString blue_text = dark_mode_ ? QStringLiteral("#93C5FD")
                                         : QStringLiteral("#1D4ED8");
    const QString blue_soft_bg = dark_mode_ ? QStringLiteral("rgba(59,130,246,0.14)")
                                            : QStringLiteral("rgba(37,99,235,0.10)");
    const QString blue_soft_border = dark_mode_ ? QStringLiteral("rgba(59,130,246,0.28)")
                                                : QStringLiteral("rgba(37,99,235,0.22)");
    const QString card_soft = dark_mode_ ? QStringLiteral("rgba(39,39,42,0.45)")
                                         : QStringLiteral("#FFFFFF");
    const QString surface_hover = dark_mode_ ? QStringLiteral("#27272A")
                                             : QStringLiteral("#F3F4F6");
    const QString border = dark_mode_ ? QStringLiteral("#27272A")
                                      : QStringLiteral("#E4E4E7");
    const QString text = dark_mode_ ? QStringLiteral("#E4E4E7")
                                    : QStringLiteral("#18181B");
    const QString muted = dark_mode_ ? QStringLiteral("#71717B")
                                     : QStringLiteral("#6B7280");
    const QString submuted = dark_mode_ ? QStringLiteral("#A1A1AA")
                                        : QStringLiteral("#71717B");
    const QString disabled_bg = dark_mode_ ? QStringLiteral("#1F2937")
                                           : QStringLiteral("#CBD5E1");

    auto setChoiceButtonStyle = [&](QPushButton* button,
                                    bool active,
                                    const QString& active_text,
                                    const QString& active_bg,
                                    const QString& active_border) {
        if (!button) {
            return;
        }
        button->setStyleSheet(
            active
                ? QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: "
                                 "10px; color: %3; }")
                      .arg(active_bg, active_border, active_text)
                : QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: "
                                 "10px; color: %3; } QPushButton:hover { background: %4; }")
                      .arg(card_soft, border, submuted, surface_hover));

        for (QLabel* label : button->findChildren<QLabel*>()) {
            if (label->objectName() == QStringLiteral("coverageTitle")) {
                label->setStyleSheet(
                    QStringLiteral("font-family: 'Arimo'; font-size: 13px; font-weight: 700; color: "
                                   "%1; background: transparent;")
                        .arg(active ? active_text : text));
            } else if (label->objectName() == QStringLiteral("coverageSubtitle")) {
                label->setStyleSheet(
                    QStringLiteral("font-family: 'Arimo'; font-size: 11px; font-weight: 400; color: "
                                   "%1; background: transparent;")
                        .arg(active ? active_text : submuted));
            }
        }
    };

    if (combo_coverage_presets_) {
        combo_coverage_presets_->setStyleSheet(
            QStringLiteral("QComboBox { background: %1; border: 1px solid %2; border-radius: 10px; "
                           "padding: 0 28px 0 12px; min-height: 40px; color: %3; "
                           "font-family: 'Arimo'; font-size: 14px; font-weight: 500; } "
                           "QComboBox::drop-down { border: none; width: 24px; } "
                           "QComboBox QAbstractItemView { background: %1; border: 1px solid %2; "
                           "selection-background-color: %4; selection-color: %3; color: %3; }")
                .arg(card_soft, border, text, accent_soft_bg));
    }
    if (btn_coverage_preset_add_) {
        btn_coverage_preset_add_->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: 10px; "
                           "color: %3; font-family: 'Arimo'; font-size: 18px; font-weight: 700; } "
                           "QPushButton:hover { background: %4; }")
                .arg(card_soft, border, text, surface_hover));
    }
    if (coverage_save_preset_card_) {
        coverage_save_preset_card_->setVisible(cache.coverage_show_save_preset);
        coverage_save_preset_card_->setStyleSheet(
            QStringLiteral("background: %1; border: 1px solid %2; border-radius: 10px;")
                .arg(card_soft, border));
    }
    if (edit_coverage_preset_name_) {
        edit_coverage_preset_name_->setStyleSheet(
            QStringLiteral("QLineEdit { background: %1; border: 1px solid %2; border-radius: 8px; "
                           "padding: 0 10px; min-height: 34px; color: %3; "
                           "font-family: 'Arimo'; font-size: 13px; }")
                .arg(dark_mode_ ? QStringLiteral("#09090B") : QStringLiteral("#F8FAFC"),
                     border,
                     text));
    }
    if (btn_coverage_preset_save_) {
        btn_coverage_preset_save_->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; border: none; border-radius: 8px; color: "
                           "#FFFFFF; font-family: 'Arimo'; font-size: 13px; font-weight: 700; } "
                           "QPushButton:hover { background: %2; } QPushButton:disabled { background: %3; }")
                .arg(accent, accent_hover, disabled_bg));
        btn_coverage_preset_save_->setEnabled(!cache.coverage_new_preset_name.trimmed().isEmpty());
    }
    if (btn_coverage_preset_cancel_) {
        btn_coverage_preset_cancel_->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: 8px; "
                           "color: %3; font-family: 'Arimo'; font-size: 13px; font-weight: 600; } "
                           "QPushButton:hover { background: %4; }")
                .arg(card_soft, border, text, surface_hover));
    }
    if (coverage_custom_presets_card_) {
        coverage_custom_presets_card_->setStyleSheet(
            QStringLiteral("background: %1; border: 1px solid %2; border-radius: 10px;")
                .arg(dark_mode_ ? QStringLiteral("rgba(39,39,42,0.25)") : QStringLiteral("#FFFFFF"),
                     border));
    }

    setChoiceButtonStyle(btn_coverage_scan_complete_,
                         cache.coverage_scan_mode != QStringLiteral("roi"),
                         accent_text,
                         accent_soft_bg,
                         accent_soft_border);
    setChoiceButtonStyle(btn_coverage_scan_roi_,
                         cache.coverage_scan_mode == QStringLiteral("roi"),
                         accent_text,
                         accent_soft_bg,
                         accent_soft_border);
    setChoiceButtonStyle(btn_coverage_roi_draw_rectangle_,
                         cache.coverage_roi_drawing_tool == QStringLiteral("rectangle"),
                         blue_text,
                         blue_soft_bg,
                         blue_soft_border);
    setChoiceButtonStyle(btn_coverage_roi_draw_polygon_,
                         cache.coverage_roi_drawing_tool == QStringLiteral("polygon"),
                         blue_text,
                         blue_soft_bg,
                         blue_soft_border);
    setChoiceButtonStyle(btn_coverage_pattern_boustro_,
                         cache.coverage_pattern == QStringLiteral("boustro"),
                         accent_text,
                         accent_soft_bg,
                         accent_soft_border);
    setChoiceButtonStyle(btn_coverage_pattern_snake_,
                         cache.coverage_pattern == QStringLiteral("snake"),
                         accent_text,
                         accent_soft_bg,
                         accent_soft_border);
    setChoiceButtonStyle(btn_coverage_pattern_spiral_,
                         cache.coverage_pattern == QStringLiteral("spiral"),
                         accent_text,
                         accent_soft_bg,
                         accent_soft_border);
    setChoiceButtonStyle(btn_coverage_axis_parallel_,
                         cache.coverage_scan_axis == QStringLiteral("parallel"),
                         accent_text,
                         accent_soft_bg,
                         accent_soft_border);
    setChoiceButtonStyle(btn_coverage_axis_perpendicular_,
                         cache.coverage_scan_axis == QStringLiteral("perpendicular"),
                         accent_text,
                         accent_soft_bg,
                         accent_soft_border);
    setChoiceButtonStyle(btn_coverage_obstacle_auto_,
                         cache.coverage_obstacle_mode != QStringLiteral("manual"),
                         accent_text,
                         accent_soft_bg,
                         accent_soft_border);
    setChoiceButtonStyle(btn_coverage_obstacle_manual_,
                         cache.coverage_obstacle_mode == QStringLiteral("manual"),
                         accent_text,
                         accent_soft_bg,
                         accent_soft_border);
    setChoiceButtonStyle(btn_coverage_draw_rectangle_,
                         cache.coverage_drawing_tool == QStringLiteral("rectangle"),
                         amber_text,
                         amber_soft_bg,
                         amber_soft_border);
    setChoiceButtonStyle(btn_coverage_draw_polygon_,
                         cache.coverage_drawing_tool == QStringLiteral("polygon"),
                         amber_text,
                         amber_soft_bg,
                         amber_soft_border);
    setChoiceButtonStyle(btn_coverage_draw_circle_,
                         cache.coverage_drawing_tool == QStringLiteral("circle"),
                         amber_text,
                         amber_soft_bg,
                         amber_soft_border);

    if (lbl_coverage_pattern_boustro_icon_) {
        QColor guide(submuted);
        guide.setAlpha(dark_mode_ ? 90 : 110);
        lbl_coverage_pattern_boustro_icon_->setPixmap(makeCoveragePatternPreviewPixmap(
            QStringLiteral("boustro"),
            QColor(cache.coverage_pattern == QStringLiteral("boustro") ? accent_text : submuted),
            guide));
    }
    if (lbl_coverage_pattern_snake_icon_) {
        QColor guide(submuted);
        guide.setAlpha(dark_mode_ ? 90 : 110);
        lbl_coverage_pattern_snake_icon_->setPixmap(makeCoveragePatternPreviewPixmap(
            QStringLiteral("snake"),
            QColor(cache.coverage_pattern == QStringLiteral("snake") ? accent_text : submuted),
            guide));
    }
    if (lbl_coverage_pattern_spiral_icon_) {
        QColor guide(submuted);
        guide.setAlpha(dark_mode_ ? 90 : 110);
        lbl_coverage_pattern_spiral_icon_->setPixmap(makeCoveragePatternPreviewPixmap(
            QStringLiteral("spiral"),
            QColor(cache.coverage_pattern == QStringLiteral("spiral") ? accent_text : submuted),
            guide));
    }

    if (lbl_coverage_scan_complete_icon_) {
        lbl_coverage_scan_complete_icon_->setPixmap(makeCoverageControlIconPixmap(
            QStringLiteral("scan_complete"),
            QColor(roi_mode ? submuted : accent_text),
            16));
    }
    if (lbl_coverage_scan_roi_icon_) {
        lbl_coverage_scan_roi_icon_->setPixmap(makeCoverageControlIconPixmap(
            QStringLiteral("scan_roi"),
            QColor(roi_mode ? accent_text : submuted),
            16));
    }
    if (lbl_coverage_roi_rectangle_icon_) {
        lbl_coverage_roi_rectangle_icon_->setPixmap(makeCoverageControlIconPixmap(
            QStringLiteral("roi_rectangle"),
            QColor(cache.coverage_roi_drawing_tool == QStringLiteral("rectangle") ? blue_text : submuted),
            16));
    }
    if (lbl_coverage_roi_polygon_icon_) {
        lbl_coverage_roi_polygon_icon_->setPixmap(makeCoverageControlIconPixmap(
            QStringLiteral("roi_polygon"),
            QColor(cache.coverage_roi_drawing_tool == QStringLiteral("polygon") ? blue_text : submuted),
            16));
    }
    if (lbl_coverage_obstacle_auto_icon_) {
        lbl_coverage_obstacle_auto_icon_->setPixmap(makeCoverageControlIconPixmap(
            QStringLiteral("obstacle_auto"),
            QColor(manual_obstacles ? submuted : accent_text),
            16));
    }
    if (lbl_coverage_obstacle_manual_icon_) {
        lbl_coverage_obstacle_manual_icon_->setPixmap(makeCoverageControlIconPixmap(
            QStringLiteral("obstacle_manual"),
            QColor(manual_obstacles ? accent_text : submuted),
            16));
    }
    if (lbl_coverage_draw_rectangle_icon_) {
        lbl_coverage_draw_rectangle_icon_->setPixmap(makeCoverageControlIconPixmap(
            QStringLiteral("obstacle_rectangle"),
            QColor(cache.coverage_drawing_tool == QStringLiteral("rectangle") ? amber_text : submuted),
            18));
    }
    if (lbl_coverage_draw_polygon_icon_) {
        lbl_coverage_draw_polygon_icon_->setPixmap(makeCoverageControlIconPixmap(
            QStringLiteral("obstacle_polygon"),
            QColor(cache.coverage_drawing_tool == QStringLiteral("polygon") ? amber_text : submuted),
            18));
    }
    if (lbl_coverage_draw_circle_icon_) {
        lbl_coverage_draw_circle_icon_->setPixmap(makeCoverageControlIconPixmap(
            QStringLiteral("obstacle_circle"),
            QColor(cache.coverage_drawing_tool == QStringLiteral("circle") ? amber_text : submuted),
            18));
    }
    if (lbl_coverage_auto_info_icon_) {
        lbl_coverage_auto_info_icon_->setPixmap(
            makeCoverageControlIconPixmap(QStringLiteral("cpu"), QColor(accent_text), 16));
    }

    if (coverage_roi_section_) {
        coverage_roi_section_->setVisible(roi_mode);
    }
    if (btn_coverage_roi_clear_) {
        btn_coverage_roi_clear_->setVisible(has_roi);
        btn_coverage_roi_clear_->setStyleSheet(
            QStringLiteral("QPushButton { background: transparent; border: none; color: %1; "
                           "font-family: 'Arimo'; font-size: 11px; font-weight: 600; } "
                           "QPushButton:hover { color: %2; }")
                .arg(muted, dark_mode_ ? QStringLiteral("#F87171") : QStringLiteral("#DC2626")));
    }
    if (btn_coverage_roi_start_) {
        const QString roi_tool_name = cache.coverage_roi_drawing_tool == QStringLiteral("polygon")
                                          ? QStringLiteral("Polygon")
                                          : QStringLiteral("Rectangle");
        btn_coverage_roi_start_->setText(
            roi_drawing_active ? QStringLiteral("Drawing %1...").arg(roi_tool_name)
                               : has_roi ? QStringLiteral("Redraw ROI")
                                         : QStringLiteral("Start Drawing"));
        btn_coverage_roi_start_->setEnabled(!roi_drawing_active);
        btn_coverage_roi_start_->setStyleSheet(
            roi_drawing_active
                ? QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: "
                                 "10px; color: %3; font-family: 'Arimo'; font-size: 14px; "
                                 "font-weight: 700; }")
                      .arg(card_soft, border, submuted)
            : has_roi
                ? QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: "
                                 "10px; color: %3; font-family: 'Arimo'; font-size: 14px; "
                                 "font-weight: 700; } QPushButton:hover { background: %4; }")
                      .arg(blue_soft_bg, blue_soft_border, blue_text, surface_hover)
                : QStringLiteral("QPushButton { background: %1; border: none; border-radius: 10px; "
                                 "color: #FFFFFF; font-family: 'Arimo'; font-size: 14px; "
                                 "font-weight: 700; } QPushButton:hover { background: %2; }")
                      .arg(blue, blue_hover));
    }
    if (coverage_roi_status_card_) {
        const bool show_roi_status = roi_mode && (roi_drawing_active || has_roi);
        coverage_roi_status_card_->setVisible(show_roi_status);
        if (show_roi_status) {
            const bool roi_ready = has_roi && !roi_drawing_active;
            coverage_roi_status_card_->setStyleSheet(
                QStringLiteral("background: %1; border: 1px solid %2; border-radius: 10px;")
                    .arg(roi_ready ? accent_soft_bg : blue_soft_bg,
                         roi_ready ? accent_soft_border : blue_soft_border));
            if (lbl_coverage_roi_status_icon_) {
                const QString icon_kind = roi_ready
                                              ? QStringLiteral("check")
                                              : (cache.coverage_roi_drawing_tool == QStringLiteral("polygon")
                                                     ? QStringLiteral("roi_polygon")
                                                     : QStringLiteral("roi_rectangle"));
                lbl_coverage_roi_status_icon_->setPixmap(makeCoverageControlIconPixmap(
                    icon_kind, QColor(roi_ready ? accent_text : blue_text), 16));
            }
            if (lbl_coverage_roi_status_text_) {
                lbl_coverage_roi_status_text_->setText(
                    roi_ready
                        ? QStringLiteral("ROI defined. Coverage generation will be constrained to this region.")
                        : QStringLiteral("Use the map preview to define the selected ROI. Press ESC to cancel."));
                lbl_coverage_roi_status_text_->setStyleSheet(
                    QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: %1; "
                                   "background: transparent;")
                        .arg(roi_ready ? accent_text : blue_text));
            }
        }
    }

    if (coverage_auto_info_card_) {
        coverage_auto_info_card_->setStyleSheet(
            QStringLiteral("background: %1; border: 1px solid %2; border-radius: 10px;")
                .arg(card_soft, border));
        for (QLabel* label : coverage_auto_info_card_->findChildren<QLabel*>()) {
            if (label->objectName() == QStringLiteral("coverageAutoInfoTitle")) {
                label->setStyleSheet(
                    QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 700; color: "
                                   "%1; background: transparent;")
                        .arg(text));
            } else if (label->objectName() == QStringLiteral("coverageAutoInfoBody")) {
                label->setStyleSheet(
                    QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: "
                                   "%1; background: transparent;")
                        .arg(submuted));
            }
        }
    }
    if (coverage_manual_hint_card_) {
        coverage_manual_hint_card_->setVisible(manual_obstacles && manual_drawing_active);
        coverage_manual_hint_card_->setStyleSheet(
            QStringLiteral("background: %1; border: 1px solid %2; border-radius: 10px;")
                .arg(amber_soft_bg, amber_soft_border));
        for (QLabel* label : coverage_manual_hint_card_->findChildren<QLabel*>()) {
            if (label->objectName() == QStringLiteral("coverageManualHint")) {
                label->setText(
                    QStringLiteral("Use the map preview to define the selected obstacle shape. Press ESC to cancel."));
                label->setStyleSheet(
                    QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: "
                                   "%1; background: transparent;")
                        .arg(amber_text));
            }
        }
    }
    if (coverage_obstacle_mode_stack_) {
        coverage_obstacle_mode_stack_->setCurrentIndex(manual_obstacles ? 1 : 0);
    }
    if (btn_coverage_detect_) {
        const bool detecting = detect_in_flight_;
        btn_coverage_detect_->setEnabled(!detecting);
        btn_coverage_detect_->setText(
            detecting ? QStringLiteral("Detecting...")
            : cache.coverage_obstacles_detected ? QStringLiteral("Redetect Obstacles")
                                                : QStringLiteral("Detect Obstacles"));
        btn_coverage_detect_->setStyleSheet(
            detecting
                ? QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: "
                                 "10px; color: %3; font-family: 'Arimo'; font-size: 14px; "
                                 "font-weight: 700; } QPushButton:disabled { background: %1; }")
                      .arg(accent_soft_bg, accent_soft_border, muted)
            : cache.coverage_obstacles_detected
                ? QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: "
                                 "10px; color: %3; font-family: 'Arimo'; font-size: 14px; "
                                 "font-weight: 700; } QPushButton:hover { background: %4; }")
                      .arg(accent_soft_bg, accent_soft_border, accent_text, surface_hover)
                : QStringLiteral("QPushButton { background: %1; border: none; border-radius: 10px; "
                                 "color: #FFFFFF; font-family: 'Arimo'; font-size: 14px; "
                                 "font-weight: 700; } QPushButton:hover { background: %2; }")
                      .arg(accent, accent_hover));
    }
    if (btn_coverage_draw_toggle_) {
        btn_coverage_draw_toggle_->setText(cache.coverage_drawing_active
                                               ? QStringLiteral("Drawing Mode Active")
                                               : QStringLiteral("Start Drawing"));
        btn_coverage_draw_toggle_->setStyleSheet(
            cache.coverage_drawing_active
                ? QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: "
                                 "10px; color: %3; font-family: 'Arimo'; font-size: 14px; "
                                 "font-weight: 700; } QPushButton:hover { background: %4; }")
                      .arg(amber_soft_bg, amber_soft_border, amber_text, surface_hover)
                : QStringLiteral("QPushButton { background: %1; border: 1px solid %2; border-radius: "
                                 "10px; color: %3; font-family: 'Arimo'; font-size: 14px; "
                                 "font-weight: 600; } QPushButton:hover { background: %4; }")
                      .arg(card_soft, border, text, surface_hover));
    }

    const double obstacle_area =
        std::accumulate(cache.coverage_obstacles.begin(),
                        cache.coverage_obstacles.end(),
                        0.0,
                        [](double total, const SessionCache::CoverageObstacle& obstacle) {
                            return total +
                                   (obstacle.geometry.outer.size() >= 3
                                        ? polygonArea(obstacle.geometry.outer)
                                        : obstacle.area_m2);
                        });
    const double total_area =
        roi_mode && has_roi
            ? polygonArea(cache.coverage_roi_polygon)
            : (cache.hull_complete ? cache.hull_area_m2
                                   : (cache.processing_complete ? cache.processed_area_estimate_m2
                                                                : cache.raw_area_estimate_m2));
    const double effective_area = cache.planning_complete
                                      ? cache.planned_effective_area_m2
                                      : std::max(0.0, total_area - obstacle_area);
    const double path_length = cache.planning_complete
                                   ? computePathLength(cache.planned_path)
                                   : (cache.coverage_path_spacing > 0.0
                                          ? (effective_area / cache.coverage_path_spacing) * 1.2
                                          : 0.0);
    const qsizetype waypoint_count = cache.planning_complete
                                         ? static_cast<qsizetype>(cache.planned_path.size())
                                         : static_cast<qsizetype>(
                                               std::llround(path_length / kCoveragePathEstimateSpacingMeters));
    const double eta_speed_mps = effectiveScanSpeedMps();
    const double estimated_minutes =
        eta_speed_mps > 0.0 ? (path_length / eta_speed_mps) / 60.0
                            : 0.0;

    if (lbl_coverage_area_) {
        lbl_coverage_area_->setText(effective_area > 0.0 ? formatArea(effective_area)
                                                         : QStringLiteral("--"));
    }
    if (lbl_coverage_waypoints_) {
        lbl_coverage_waypoints_->setText(waypoint_count > 0 ? formatCount(waypoint_count)
                                                            : QStringLiteral("--"));
    }
    if (lbl_coverage_path_length_) {
        lbl_coverage_path_length_->setText(path_length > 0.0 ? QStringLiteral("%1 m").arg(path_length, 0, 'f', 1)
                                                             : QStringLiteral("--"));
    }
    if (lbl_coverage_est_time_) {
        lbl_coverage_est_time_->setText(estimated_minutes > 0.0
                                            ? QStringLiteral("%1 min").arg(estimated_minutes, 0, 'f', 1)
                                            : QStringLiteral("--"));
    }
    if (coverage_obstacle_area_card_) {
        coverage_obstacle_area_card_->setVisible(obstacle_area > 0.0);
    }
    if (lbl_coverage_obstacles_area_) {
        lbl_coverage_obstacles_area_->setText(
            obstacle_area > 0.0
                ? QStringLiteral("%1 (%2%)")
                      .arg(formatArea(obstacle_area))
                      .arg(total_area > 0.0 ? QString::number((obstacle_area / total_area) * 100.0, 'f', 1)
                                            : QStringLiteral("0.0"))
                : QStringLiteral("--"));
    }

    if (btn_coverage_clear_obstacles_) {
        btn_coverage_clear_obstacles_->setVisible(!cache.coverage_obstacles.empty());
        btn_coverage_clear_obstacles_->setStyleSheet(
            QStringLiteral("QPushButton { background: transparent; border: none; color: %1; "
                           "font-family: 'Arimo'; font-size: 11px; font-weight: 600; } "
                           "QPushButton:hover { color: %2; }")
                .arg(muted, dark_mode_ ? QStringLiteral("#F87171") : QStringLiteral("#DC2626")));
    }

    if (btn_coverage_generate_) {
        if (lbl_coverage_generate_text_) {
            lbl_coverage_generate_text_->setText(
                planning_in_flight_ ? QStringLiteral("Generating Paths...")
                : cache.planning_complete ? QStringLiteral("Regenerate Coverage Paths")
                                          : QStringLiteral("Generate Coverage Paths"));
            lbl_coverage_generate_text_->setStyleSheet(
                QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 700; color: %1; "
                               "background: transparent;")
                    .arg(cache.planning_complete && !planning_in_flight_ ? accent_text
                                                                         : QStringLiteral("#FFFFFF")));
        }
        if (lbl_coverage_generate_icon_) {
            lbl_coverage_generate_icon_->setPixmap(loadSvgPixmap(
                QStringLiteral(":/assets/missionplanner/stage_coverage_planning.svg"),
                16,
                16,
                cache.planning_complete && !planning_in_flight_ ? accent_text : QStringLiteral("#FFFFFF")));
        }
        btn_coverage_generate_->setStyleSheet(
            planning_in_flight_
                ? QStringLiteral("QPushButton { background: %1; border: none; border-radius: 14px; }")
                      .arg(disabled_bg)
            : cache.planning_complete
                ? QStringLiteral("QPushButton { background: %1; border: 2px solid %2; border-radius: "
                                 "14px; } QPushButton:hover { background: %3; } QPushButton:disabled { "
                                 "background: %4; border-color: %2; }")
                      .arg(accent_soft_bg, accent_soft_border, surface_hover, disabled_bg)
                : QStringLiteral("QPushButton { background: %1; border: none; border-radius: 14px; } "
                                 "QPushButton:hover { background: %2; } QPushButton:disabled { "
                                 "background: %3; }")
                      .arg(accent, accent_hover, disabled_bg));
    }

    if (coverage_legend_chip_) {
        coverage_legend_chip_->setStyleSheet(
            QStringLiteral("background: %1; border: 1px solid %2; border-radius: 10px;")
                .arg(card_soft, border));
    }
    if (coverage_legend_boundary_swatch_) {
        coverage_legend_boundary_swatch_->setStyleSheet(
            QStringLiteral("background: %1; border: 2px solid %2; border-radius: 3px;")
                .arg(dark_mode_ ? QStringLiteral("rgba(0,188,125,0.12)")
                                : QStringLiteral("rgba(5,150,105,0.10)"),
                     accent));
    }
    if (coverage_legend_path_swatch_) {
        coverage_legend_path_swatch_->setStyleSheet(
            QStringLiteral("background: %1; border-radius: 1px;")
                .arg(cache.planning_complete ? accent : muted));
    }
    if (lbl_coverage_legend_boundary_) {
        lbl_coverage_legend_boundary_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: %1; "
                           "background: transparent;")
                .arg(submuted));
    }
    if (lbl_coverage_legend_path_) {
        lbl_coverage_legend_path_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: %1; "
                           "background: transparent;")
                .arg(submuted));
    }

    rebuildCoveragePresetRows();
    rebuildCoverageObstacleRows();
}

void PlannerScreen::updateHeaderForCurrentStep() {
    if (!lbl_left_header_icon_ || !lbl_left_header_title_ || !lbl_left_header_subtitle_) {
        return;
    }

    const QString accent = dark_mode_ ? QStringLiteral("#00D492") : QStringLiteral("#059669");
    if (current_step_ == PlannerStep::MapProcessing) {
        lbl_left_header_icon_->setPixmap(
            loadSvgPixmap(QStringLiteral(":/assets/missionplanner/point_cloud_processing.svg"),
                          16,
                          16,
                          accent));
        lbl_left_header_title_->setText(QStringLiteral("Point Cloud Processing"));
        lbl_left_header_subtitle_->setText(QStringLiteral("Optimize and clean scan data"));
    } else if (current_step_ == PlannerStep::CoveragePlanning) {
        lbl_left_header_icon_->setPixmap(
            loadSvgPixmap(QStringLiteral(":/assets/missionplanner/stage_coverage_planning.svg"),
                          16,
                          16,
                          accent));
        lbl_left_header_title_->setText(QStringLiteral("Coverage Planning"));
        lbl_left_header_subtitle_->setText(QStringLiteral("Generate optimal scan paths"));
    } else if (current_step_ == PlannerStep::ScanSplitting) {
        lbl_left_header_icon_->setPixmap(
            loadSvgPixmap(QStringLiteral(":/assets/missionplanner/stage_scan_splitting.svg"),
                          16,
                          16,
                          accent));
        lbl_left_header_title_->setText(QStringLiteral("Scan Splitting"));
        lbl_left_header_subtitle_->setText(QStringLiteral("Divide coverage into segments"));
    } else {
        lbl_left_header_icon_->setPixmap(
            loadSvgPixmap(QStringLiteral(":/assets/missionplanner/stage_scan.svg"),
                          16,
                          16,
                          accent));
        lbl_left_header_title_->setText(QStringLiteral("Scan"));
        lbl_left_header_subtitle_->setText(QStringLiteral("Execute the planned mission"));
    }
}

void PlannerScreen::updateStageSteps() {
    const int current_visual_stage = static_cast<int>(current_step_);

    auto style_step = [&](StageStepUi& step, int stage_index, bool clickable) {
        if (!step.wrapper || !step.button || !step.icon || !step.text) {
            return;
        }

        const bool active = stage_index == current_visual_stage;
        const bool completed = stage_index < current_visual_stage;
        const QString accent = dark_mode_ ? QStringLiteral("#00D492") : QStringLiteral("#059669");
        const QString muted = dark_mode_ ? QStringLiteral("#71717B") : QStringLiteral("#6B7280");
        const QString active_bg = dark_mode_ ? QStringLiteral("rgba(0,188,125,0.15)")
                                             : QStringLiteral("rgba(5,150,105,0.10)");
        const QString active_border = dark_mode_ ? QStringLiteral("rgba(0,188,125,0.30)")
                                                 : QStringLiteral("rgba(5,150,105,0.22)");
        step.wrapper->setFixedHeight(34);
        step.wrapper->setFixedWidth(step.group_width);
        step.button->setFixedSize(step.button_width, active ? 34 : 32);
        step.button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        step.button->setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        step.button->setEnabled(clickable);
        step.button->setStyleSheet(
            active ? QStringLiteral(
                         "QPushButton {"
                         "  background: %1;"
                         "  border: 1px solid %2;"
                         "  border-radius: 10px;"
                         "}")
                         .arg(active_bg, active_border)
                  : QStringLiteral(
                        "QPushButton {"
                        "  background: transparent;"
                        "  border: none;"
                        "  border-radius: 10px;"
                        "}"));
        const QString color = (active || completed) ? accent : muted;
        step.icon->setPixmap(loadSvgPixmap(step.icon_path, 14, 14, color));
        step.text->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; "
                           "color: %1; background: transparent;")
                .arg(color));
    };

    const SessionCache* cache = activeSessionPtr();
    const bool plan_ready =
        kBypassPlannerStageGates || (cache && cache->planning_complete);
    const bool scan_ready =
        kBypassPlannerStageGates || (cache && cache->scan_waypoints_published);
    style_step(step_map_processing_, 0, true);
    style_step(step_coverage_planning_, 1, true);
    style_step(step_scan_splitting_, 2, plan_ready);
    style_step(step_scan_, 3, scan_ready);
}

void PlannerScreen::updateFooter() {
    if (!lbl_stage_footer_ || !btn_next_ || !lbl_next_text_ || !lbl_next_icon_) {
        return;
    }

    const QString muted = dark_mode_ ? QStringLiteral("#71717B") : QStringLiteral("#6B7280");
    const QString next_active = QStringLiteral("#FFFFFF");
    const SessionCache* cache = activeSessionPtr();
    const bool plan_ready =
        kBypassPlannerStageGates || (cache && cache->planning_complete);
    const bool scan_ready =
        kBypassPlannerStageGates || (cache && cache->scan_waypoints_published);
    if (current_step_ == PlannerStep::MapProcessing) {
        lbl_stage_footer_->setText(QStringLiteral("Stage 1 of 4"));
        lbl_next_text_->setText(QStringLiteral("Stage 2 (Coverage Planning)"));
        lbl_next_text_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; "
                           "color: %1; background: transparent;")
                .arg(next_active));
        lbl_next_icon_->setPixmap(loadSvgPixmap(QStringLiteral(":/assets/missionplanner/next_arrow.svg"),
                                                16,
                                                16,
                                                next_active));
        btn_next_->setEnabled(true);
        btn_next_->setToolTip(QStringLiteral("Open the coverage planning step."));
    } else if (current_step_ == PlannerStep::CoveragePlanning) {
        lbl_stage_footer_->setText(QStringLiteral("Stage 2 of 4"));
        lbl_next_text_->setText(QStringLiteral("Stage 3 (Scan Splitting)"));
        const QString tone = plan_ready ? next_active : muted;
        lbl_next_text_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; "
                           "color: %1; background: transparent;")
                .arg(tone));
        lbl_next_icon_->setPixmap(loadSvgPixmap(QStringLiteral(":/assets/missionplanner/next_arrow.svg"),
                                                16,
                                                16,
                                                tone));
        btn_next_->setEnabled(plan_ready);
        btn_next_->setToolTip(plan_ready
                                  ? QStringLiteral("Open the scan splitting step.")
                                  : QStringLiteral("Generate a coverage plan before splitting scans."));
    } else if (current_step_ == PlannerStep::ScanSplitting) {
        lbl_stage_footer_->setText(QStringLiteral("Stage 3 of 4"));
        lbl_next_text_->setText(QStringLiteral("Stage 4 (Scan)"));
        const QString tone = scan_ready ? next_active : muted;
        lbl_next_text_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; "
                           "color: %1; background: transparent;")
                .arg(tone));
        lbl_next_icon_->setPixmap(loadSvgPixmap(QStringLiteral(":/assets/missionplanner/next_arrow.svg"),
                                                16,
                                                16,
                                                tone));
        btn_next_->setEnabled(scan_ready);
        btn_next_->setToolTip(
            scan_ready ? QStringLiteral("Open the scan execution screen.")
                       : QStringLiteral("Publish at least one segment before opening Scan."));
    } else {
        // PlannerStep::Scan — Complete Mission button on the right.
        lbl_stage_footer_->setText(QStringLiteral("Stage 4 of 4"));
        lbl_next_text_->setText(QStringLiteral("Complete Mission"));
        const bool dev_force_complete =
            qEnvironmentVariable("BDR_DEV_START_AT_SCAN").trimmed() == QStringLiteral("1");
        const bool can_complete =
            dev_force_complete ||
            (cache && (cache->scan_run_state == ScanRunState::Completed ||
                       scan_manual_override_engaged_once_));
        const QString tone = can_complete ? next_active : muted;
        lbl_next_text_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; "
                           "color: %1; background: transparent;")
                .arg(tone));
        lbl_next_icon_->setPixmap(loadSvgPixmap(QStringLiteral(":/assets/missionplanner/scan_check.svg"),
                                                16,
                                                16,
                                                tone));
        btn_next_->setEnabled(can_complete);
        btn_next_->setToolTip(can_complete
                                  ? QStringLiteral("Finish the mission and return to dashboard.")
                                  : QStringLiteral("Complete all selected segments or enter Manual Override first."));
    }

    if (footer_left_stack_) {
        footer_left_stack_->setCurrentIndex(
            current_step_ == PlannerStep::Scan ? 1 : 0);
    }

    lbl_stage_footer_->setStyleSheet(
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 400; "
                       "color: %1; background: transparent;")
            .arg(muted));
}

void PlannerScreen::updateButtonsAndStatus() {
    const QFileInfo map_info(map_path_);
    const SessionCache* cache = activeSessionPtr();
    const bool map_valid = !map_path_.isEmpty() && map_info.exists();
    const bool roi_required =
        cache && cache->coverage_scan_mode == QStringLiteral("roi");
    const bool roi_ready =
        !roi_required || cache->coverage_roi_polygon.size() >= 3;
    const bool can_process =
        current_step_ == PlannerStep::MapProcessing && map_valid && cache && cache->raw_loaded &&
        !load_in_flight_ && !process_in_flight_ && !hull_in_flight_;
    const bool can_hull =
        current_step_ == PlannerStep::MapProcessing && map_valid && cache &&
        cache->processing_complete && !process_in_flight_ && !hull_in_flight_;
    const bool can_generate =
        current_step_ == PlannerStep::CoveragePlanning && map_valid && cache &&
        cache->hull_complete && !load_in_flight_ && !process_in_flight_ && !hull_in_flight_ &&
        !planning_in_flight_ && !detect_in_flight_ && roi_ready &&
        !cache->coverage_roi_drawing_active && !cache->coverage_drawing_active;

    if (btn_process_) {
        btn_process_->setEnabled(can_process);
        btn_process_->setToolTip(
            !map_valid ? QStringLiteral("A valid saved map is required.")
            : load_in_flight_ ? QStringLiteral("The saved map is still loading.")
            : process_in_flight_ ? QStringLiteral("Point cloud processing is already running.")
                                 : QStringLiteral("Apply Z filtering and voxel downsampling."));
    }

    if (btn_hull_) {
        btn_hull_->setEnabled(can_hull);
        btn_hull_->setToolTip(
            !map_valid ? QStringLiteral("A valid saved map is required.")
            : !cache || !cache->processing_complete
                ? QStringLiteral("Process Point Cloud first.")
                : hull_in_flight_ ? QStringLiteral("Hull computation is already running.")
                                  : QStringLiteral("Compute a projected hull from the processed cloud."));
    }

    if (btn_coverage_generate_) {
        btn_coverage_generate_->setEnabled(can_generate);
        btn_coverage_generate_->setToolTip(
            !map_valid ? QStringLiteral("A valid saved map is required.")
            : !cache || !cache->processing_complete
                ? QStringLiteral("Process Point Cloud first.")
            : !cache->hull_complete
                ? QStringLiteral("Compute and project the hull first.")
            : cache->coverage_roi_drawing_active || cache->coverage_drawing_active
                ? QStringLiteral("Finish or cancel the current drawing interaction first.")
            : roi_required && !roi_ready
                ? QStringLiteral("Draw an ROI before generating coverage.")
            : planning_in_flight_ ? QStringLiteral("Coverage generation is already running.")
                                  : QStringLiteral("Generate coverage paths from the projected hull."));
    }

    updateCoveragePlanningUi();
    // Stepper and footer share the same gating state (planning_complete,
    // processing_complete, current_step_) as the per-action buttons, so refresh
    // them together to avoid stale "Next" / scan-splitting-step states after
    // the coverage plan completes or is invalidated.
    updateStageSteps();
    updateFooter();
}

void PlannerScreen::applyLiveOverlayToPlot() {
    if (!plot_) {
        return;
    }
    plot_->setRobotMarkerSize(robot_marker_size_m_);
    plot_->setRobotPose(live_robot_pose_);
    plot_->setRobotTrail(live_robot_trail_);
}

void PlannerScreen::applySessionToUi() {
    syncing_widgets_ = true;
    SessionCache& cache = activeSession();
    ensureCoverageDefaults(cache);
    if (slider_voxel_) {
        slider_voxel_->setValue(cache.voxel_size);
    }
    if (slider_z_min_) {
        slider_z_min_->setValue(cache.z_min);
    }
    if (slider_z_max_) {
        slider_z_max_->setValue(cache.z_max);
    }
    if (slider_alpha_) {
        slider_alpha_->setValue(cache.alpha);
    }
    if (slider_coverage_path_spacing_) {
        slider_coverage_path_spacing_->setValue(cache.coverage_path_spacing);
    }
    if (slider_coverage_headland_) {
        slider_coverage_headland_->setValue(cache.coverage_headland_width);
    }
    if (slider_coverage_scan_speed_) {
        slider_coverage_scan_speed_->setValue(cache.coverage_scan_speed_mps);
    }
    if (combo_coverage_presets_) {
        refreshCoveragePresetCombo();
        QSignalBlocker blocker(combo_coverage_presets_);
        int preset_index = combo_coverage_presets_->findData(cache.coverage_selected_preset);
        if (preset_index < 0) {
            preset_index = combo_coverage_presets_->findText(cache.coverage_selected_preset);
        }
        combo_coverage_presets_->setCurrentIndex(preset_index);
    }
    if (edit_coverage_preset_name_) {
        edit_coverage_preset_name_->setText(cache.coverage_new_preset_name);
    }
    if (coverage_obstacle_mode_stack_) {
        coverage_obstacle_mode_stack_->setCurrentIndex(
            cache.coverage_obstacle_mode == QStringLiteral("manual") ? 1 : 0);
    }
    syncing_widgets_ = false;

    if (content_stack_) {
        QWidget* target_page = map_processing_page_;
        if (current_step_ == PlannerStep::CoveragePlanning) {
            target_page = coverage_placeholder_page_;
        } else if (current_step_ == PlannerStep::ScanSplitting && scan_splitting_page_) {
            target_page = scan_splitting_page_;
        } else if (current_step_ == PlannerStep::Scan && scan_page_) {
            target_page = scan_page_;
        }
        content_stack_->setCurrentWidget(target_page);
    }
    // Stage 4 visibility: the Scan page mirrors the Figma 1:1 — every panel
    // is structural, not data-conditional. Single-segment runs (or zero-
    // segment dev-bypass runs) get an empty/single-row state for the
    // segment-aware pieces, but the layout never collapses a column.
    const bool on_scan = current_step_ == PlannerStep::Scan;
    if (!on_scan) {
        setScanManualOverride(false);
        if (scan_camera_stream_requested_) {
            stopScanCameraStream();
        }
    } else if (scan_camera_view_) {
        if (!scan_camera_view_->isPlaying() && !scan_camera_stream_requested_) {
            scan_camera_stream_requested_ = true;
            scan_camera_view_->startStream(5600);
        } else {
            scan_camera_stream_requested_ = true;
        }
    }
    if (left_rail_) {
        left_rail_->setFixedWidth(on_scan ? kPlannerScanLeftRailWidth : kPlannerLeftRailWidth);
    }

    if (scan_right_rail_) {
        scan_right_rail_->setVisible(on_scan);
    }
    if (scan_current_segment_card_) {
        scan_current_segment_card_->setVisible(on_scan);
    }
    if (scan_control_bar_) {
        scan_control_bar_->setVisible(on_scan);
    }
    if (scan_status_pill_) {
        scan_status_pill_->setVisible(on_scan);
    }
    if (scan_legend_chip_) {
        scan_legend_chip_->setVisible(on_scan);
    }
    if (tool_stack_) {
        // Figma 137:201 only shows the status pill in the top-right of the
        // map on the Scan stage \xE2\x80\x94 the zoom/fit/reset tool buttons are
        // hidden there. Keep them on every other stage where they\xE2\x80\x99re
        // genuinely useful.
        tool_stack_->setVisible(!on_scan);
    }

    // Visibility flips on grandchildren of PlannerPreviewHost (e.g. the pill
    // inside top_right_column) do not propagate up to the host's resizeEvent,
    // so its overlay positioning stays at the size measured while the child
    // was hidden. Force a relayout now that the visibility for this stage is
    // settled. center_stage_ is always a PlannerPreviewHost in this TU; the
    // base type is QWidget* only for header opacity (PlannerPreviewHost is a
    // file-scope class). static_cast is safe by construction.
    if (center_stage_) {
        static_cast<PlannerPreviewHost*>(center_stage_)->relayoutOverlays();
    }
    if (preview_bottom_overlay_stack_) {
        // The bottom-left overlay belongs to the planning stages
        // (stats / coverage legend). On Scan we either show the dedicated
        // top-left segment legend (when there are splits) or nothing at all.
        if (on_scan) {
            preview_bottom_overlay_stack_->setVisible(false);
        } else {
            preview_bottom_overlay_stack_->setVisible(true);
            QWidget* overlay = stats_chip_;
            if (current_step_ != PlannerStep::MapProcessing) {
                overlay = coverage_legend_chip_;
            }
            preview_bottom_overlay_stack_->setCurrentWidget(overlay);
        }
    }
    if (preview_container_) {
        // Map card container: only on Scan stage. Other stages keep the
        // original borderless map. Object-name selectors so the rule survives
        // any unqualified `background:` cascade from ancestors. The Scan
        // variant uses card-surface #18181B (matches the rail cards) and the
        // same #27272A border at 1px so the map area reads as a peer card to
        // the rails. Color-matching the inner plot to this surface would be
        // ideal but PlotWidget paints its own dark canvas; instead the 1px
        // margin ring around the plot exposes #18181B + the border, which is
        // enough contrast against the #09090B page background.
        preview_container_->setStyleSheet(
            on_scan
                ? QStringLiteral(
                      "QWidget#plannerPreviewContainer { background: #18181B; "
                      "border: 1px solid #27272A; border-radius: 12px; }")
                : QStringLiteral(
                      "QWidget#plannerPreviewContainer { background: transparent; }"));
    }

    updateValueLabels();
    updateHeaderForCurrentStep();
    updateStageSteps();
    updateFooter();
    updatePreview();
    updateOutputCards();
    updateStatsChip();
    updatePlaceholderMessage();
    updateScanSplittingUi();
    updateScanRunUi();
    updateButtonsAndStatus();
    syncScanManualTeleopTimer();
}

void PlannerScreen::navigateToStep(PlannerStep step) {
    // Self-clicks on the same stage pill should be a no-op for the
    // checklist gate — we don't want to re-prompt the operator just for
    // re-pressing Scan while already on Scan.
    const PlannerStep previous_step = current_step_;
    current_step_ = step;
    persistCurrentStep();
    applySessionToUi();
    // Auto-split on entry to the Scan Splitting step using the cached
    // scan_distance_m, so the operator doesn't have to click Split path
    // every time. Skipped when segments are already populated and not
    // dirty, to preserve in-progress selection / completion state. The
    // splitting stage is also where the preflight ack resets — every
    // round-trip through here forces a new acknowledgment on the next
    // Scan entry.
    if (step == PlannerStep::ScanSplitting) {
        SessionCache& c = activeSession();
        c.scan_preflight_acknowledged = false;
        if (c.planning_complete && c.planned_path.size() >= 2 &&
            (c.scan_segments.empty() || c.scan_splits_dirty)) {
            onSplitPathClicked();
        }
    }
    // Pre-scan operator checklist gate. Defer with a zero-delay timer so
    // the Scan stage paints once before the modal overlays it, otherwise
    // the operator briefly sees an empty/half-built page behind the
    // dialog. previous_step != Scan suppresses the prompt on stage-pill
    // self-clicks while already on Scan.
    if (step == PlannerStep::Scan && previous_step != PlannerStep::Scan) {
        const SessionCache* c = activeSessionPtr();
        if (c && !c->scan_preflight_acknowledged) {
            QTimer::singleShot(0, this, [this]() { showScanPreflightDialog(); });
        }
    }
    maybeRunAutotest();
}

void PlannerScreen::setInlineStatus(const QString& text, const QString& color_hex) {
    if (!lbl_inline_status_) {
        return;
    }

    lbl_inline_status_->setText(text);
    // The Figma frame intentionally hides the status row for info/progress
    // (#71717B grey) and success (#00D492 green) tones to keep the left rail
    // clean. Warnings (#F59E0B amber) and errors (#F87171 red) must remain
    // visible so the user can see why an action failed.
    const QString normalized = color_hex.trimmed().toUpper();
    const bool is_quiet_tone = normalized == QStringLiteral("#71717B") ||
                               normalized == QStringLiteral("#00D492");
    lbl_inline_status_->setVisible(!text.isEmpty() && !is_quiet_tone);
    lbl_inline_status_->setStyleSheet(
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 500; "
                       "color: %1; background: transparent;")
            .arg(color_hex));
}

void PlannerScreen::logAutotestSummary(const QString& phase) {
    if (!autotest_enabled_) {
        return;
    }

    const SessionCache& cache = activeSession();
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    settings.beginGroup(QStringLiteral("planner/mission_planner"));
    settings.beginGroup(settingsGroupKey());
    const double stored_voxel = settings.value(QStringLiteral("voxel_size"), -1.0).toDouble();
    const double stored_z_min = settings.value(QStringLiteral("z_min"), -999.0).toDouble();
    const double stored_z_max = settings.value(QStringLiteral("z_max"), -999.0).toDouble();
    const double stored_alpha = settings.value(QStringLiteral("alpha"), -1.0).toDouble();
    settings.endGroup();
    settings.endGroup();

    std::cout << "PLANNER_AUTOTEST"
              << " phase=" << phase.toStdString()
              << " raw_loaded=" << (cache.raw_loaded ? 1 : 0)
              << " processed=" << (cache.processing_complete ? 1 : 0)
              << " hull=" << (cache.hull_complete ? 1 : 0)
              << " step="
              << (current_step_ == PlannerStep::MapProcessing ? "map_processing"
                  : current_step_ == PlannerStep::CoveragePlanning
                        ? "coverage_planning"
                        : "scan_splitting")
              << " voxel=" << cache.voxel_size
              << " z_min=" << cache.z_min
              << " z_max=" << cache.z_max
              << " alpha=" << cache.alpha
              << " stored_voxel=" << stored_voxel
              << " stored_z_min=" << stored_z_min
              << " stored_z_max=" << stored_z_max
              << " stored_alpha=" << stored_alpha
              << " raw_points=" << cache.raw_point_count
              << " processed_points=" << cache.processed_point_count
              << " hull_vertices=" << cache.hull_polygon.size()
              << " status=\"" << lbl_inline_status_->text().toStdString() << "\""
              << std::endl;
}

void PlannerScreen::maybeRunAutotest() {
    if (!autotest_enabled_ || map_path_.isEmpty() || load_in_flight_) {
        return;
    }

    SessionCache& cache = activeSession();
    if (autotest_mode_ == QStringLiteral("restore")) {
        if (cache.raw_loaded && !autotest_restore_reported_) {
            autotest_restore_reported_ = true;
            std::cout << "PLANNER_AUTOTEST phase=restore_ready" << std::endl;
            logAutotestSummary(QStringLiteral("restore_complete"));
            QTimer::singleShot(0, qApp, []() { qApp->quit(); });
        }
        return;
    }

    if (!cache.raw_loaded) {
        return;
    }

    if (!autotest_started_) {
        autotest_started_ = true;
        std::cout << "PLANNER_AUTOTEST phase=full_start" << std::endl;
        cache.voxel_size = 0.07;
        cache.z_min = -0.12;
        cache.z_max = 0.18;
        cache.alpha = 1.23;
        persistParameters();
        applySessionToUi();
        setInlineStatus(QStringLiteral("Running planner autotest..."), QStringLiteral("#71717B"));
        QTimer::singleShot(0, this, [this]() { startProcessPointCloud(); });
        return;
    }

    if (cache.processing_complete && !cache.hull_complete && !process_in_flight_ && !hull_in_flight_) {
        std::cout << "PLANNER_AUTOTEST phase=run_hull" << std::endl;
        QTimer::singleShot(0, this, [this]() { startComputeHull(); });
        return;
    }

    if (cache.hull_complete && current_step_ == PlannerStep::MapProcessing) {
        std::cout << "PLANNER_AUTOTEST phase=goto_placeholder" << std::endl;
        QTimer::singleShot(0, this, [this]() { navigateToStep(PlannerStep::CoveragePlanning); });
        return;
    }

    if (cache.hull_complete && current_step_ == PlannerStep::CoveragePlanning) {
        logAutotestSummary(QStringLiteral("full_complete"));
        QTimer::singleShot(0, qApp, []() { qApp->quit(); });
    }
}

void PlannerScreen::restoreCurrentSession() {
    const QString new_context_key = sessionKey();
    if (active_context_key_ != new_context_key) {
        active_context_key_ = new_context_key;
        autotest_started_ = false;
        autotest_restore_reported_ = false;
        ++load_generation_;
        ++process_generation_;
        ++hull_generation_;
        ++planning_generation_;
        load_in_flight_ = false;
        process_in_flight_ = false;
        hull_in_flight_ = false;
        planning_in_flight_ = false;
        if (preview_stack_ && preview_placeholder_) {
            preview_stack_->setCurrentWidget(preview_placeholder_);
        }
    }

    SessionCache& cache = activeSession();
    loadPersistedParameters(cache);
    QStringList parameter_warnings;
    const bool parameters_adjusted = sanitizePlannerParameters(
        &cache.voxel_size, &cache.z_min, &cache.z_max, &cache.alpha, &parameter_warnings);
    if (parameters_adjusted) {
        persistParameters();
        std::cout << "PlannerScreen adjusted persisted parameters: "
                  << parameter_warnings.join(' ').toStdString() << std::endl;
    }
    current_step_ = cache.last_step;
    applySessionToUi();
    if (autotest_enabled_) {
        std::cout << "PLANNER_AUTOTEST phase=restore_session map=\""
                  << map_path_.toStdString() << "\" raw_loaded=" << (cache.raw_loaded ? 1 : 0)
                  << std::endl;
    }

    const QFileInfo map_info(map_path_);
    if (map_path_.isEmpty()) {
        setInlineStatus(
            QStringLiteral("No saved map available yet. Processing stays disabled until exploration saves one."),
            QStringLiteral("#F87171"));
        return;
    }
    if (!map_info.exists()) {
        setInlineStatus(
            QStringLiteral("Saved map unavailable: %1").arg(map_path_),
            QStringLiteral("#F87171"));
        return;
    }

    if (cache.raw_loaded) {
        if (cache.hull_complete) {
            setInlineStatus(
                QStringLiteral("Restored cached hull for %1.").arg(map_info.fileName()),
                QStringLiteral("#00D492"));
        } else if (cache.processing_complete) {
            setInlineStatus(
                QStringLiteral("Restored processed point cloud for %1.").arg(map_info.fileName()),
                QStringLiteral("#00D492"));
        } else {
            setInlineStatus(
                QStringLiteral("Saved map ready: %1").arg(map_info.fileName()),
                QStringLiteral("#71717B"));
        }
        maybeRunAutotest();
    } else {
        startMapLoadIfNeeded();
    }

    if (parameters_adjusted) {
        setInlineStatus(
            QStringLiteral("Adjusted saved planner settings: %1").arg(parameter_warnings.join(' ')),
            QStringLiteral("#F59E0B"));
    }
}

void PlannerScreen::startMapLoadIfNeeded() {
    const QFileInfo map_info(map_path_);
    SessionCache& cache = activeSession();
    if (map_path_.isEmpty() || !map_info.exists() || cache.raw_loaded || load_in_flight_) {
        updateButtonsAndStatus();
        return;
    }
    if (autotest_enabled_) {
        std::cout << "PLANNER_AUTOTEST phase=start_map_load path=\""
                  << map_info.absoluteFilePath().toStdString() << "\"" << std::endl;
    }

    const quint64 generation = ++load_generation_;
    load_in_flight_ = true;
    updateButtonsAndStatus();
    setInlineStatus(
        QStringLiteral("Loading saved map: %1").arg(map_info.fileName()),
        QStringLiteral("#71717B"));

    QPointer<PlannerScreen> guard(this);
    const QString path = map_info.absoluteFilePath();
    QtConcurrent::run([guard, generation, path]() mutable {
        MapLoadResult result;
        try {
            result.raw_cloud = makeFinitePointCloud(loadPointCloudFile(path.toStdString()));
            if (!result.raw_cloud || result.raw_cloud->empty()) {
                throw std::runtime_error("The saved map contains no points.");
            }
            result.raw_point_count = static_cast<qsizetype>(result.raw_cloud->size());
            result.raw_projected_points = buildProjectedPointsFromCloud(result.raw_cloud);
            if (result.raw_projected_points.empty()) {
                throw std::runtime_error("The saved map contains no finite XY points.");
            }
            result.area_estimate_m2 = estimateAreaFromPointCloudBounds(result.raw_cloud);
            result.success = true;
        } catch (const std::exception& error) {
            result.error = QString::fromUtf8(error.what());
        }

        if (!guard) {
            return;
        }

        QMetaObject::invokeMethod(
            guard,
            [guard, generation, result = std::move(result)]() mutable {
                if (guard) {
                    guard->applyMapLoadResult(generation, result);
                }
            },
            Qt::QueuedConnection);
    });
}

void PlannerScreen::startProcessPointCloud() {
    const QFileInfo map_info(map_path_);
    SessionCache& cache = activeSession();
    if (map_path_.isEmpty() || !map_info.exists()) {
        setInlineStatus(QStringLiteral("A valid saved map is required before processing."),
                        QStringLiteral("#F87171"));
        updateButtonsAndStatus();
        return;
    }
    if (!cache.raw_loaded) {
        startMapLoadIfNeeded();
        setInlineStatus(QStringLiteral("Waiting for the saved map to finish loading."),
                        QStringLiteral("#F59E0B"));
        return;
    }
    if (process_in_flight_ || hull_in_flight_) {
        return;
    }

    const quint64 generation = ++process_generation_;
    process_in_flight_ = true;
    updateButtonsAndStatus();
    setInlineStatus(QStringLiteral("Processing point cloud with current voxel and height filters..."),
                    QStringLiteral("#71717B"));

    QPointer<PlannerScreen> guard(this);
    const PointCloudPtr raw_cloud = cache.raw_cloud;
    const double z_min = cache.z_min;
    const double z_max = cache.z_max;
    const double voxel_size = cache.voxel_size;
    QtConcurrent::run([guard, generation, raw_cloud, z_min, z_max, voxel_size]() mutable {
        ProcessResult result;
        try {
            PointCloudPtr filtered = filterByZRange(raw_cloud, z_min, z_max);
            if (!filtered || filtered->empty()) {
                throw std::runtime_error("No points remain after applying the height filter.");
            }

            result.processed_cloud = downsampleVoxel(filtered, voxel_size);
            if (!result.processed_cloud || result.processed_cloud->empty()) {
                throw std::runtime_error("No points remain after voxel downsampling.");
            }

            result.raw_point_count = static_cast<qsizetype>(raw_cloud->size());
            result.processed_point_count = static_cast<qsizetype>(result.processed_cloud->size());
            result.processed_projected_points =
                buildProjectedPointsFromCloud(result.processed_cloud);
            if (result.processed_projected_points.empty()) {
                throw std::runtime_error("No finite XY points remain after voxel downsampling.");
            }
            result.area_estimate_m2 = estimateAreaFromPointCloudBounds(result.processed_cloud);
            result.estimated_file_size_mb =
                (static_cast<double>(result.processed_point_count) * sizeof(pcl::PointXYZ)) /
                (1024.0 * 1024.0);
            result.quality_label = deriveQualityLabel(result.raw_point_count,
                                                      result.processed_point_count,
                                                      voxel_size);
            if (result.raw_point_count > 0) {
                result.reduction_percent =
                    (1.0 - (static_cast<double>(result.processed_point_count) /
                            static_cast<double>(result.raw_point_count))) *
                    100.0;
            }
            result.success = true;
        } catch (const std::exception& error) {
            result.error = QString::fromUtf8(error.what());
        }

        if (!guard) {
            return;
        }

        QMetaObject::invokeMethod(
            guard,
            [guard, generation, result = std::move(result)]() mutable {
                if (guard) {
                    guard->applyProcessResult(generation, result);
                }
            },
            Qt::QueuedConnection);
    });
}

void PlannerScreen::startComputeHull() {
    const QFileInfo map_info(map_path_);
    SessionCache& cache = activeSession();
    if (map_path_.isEmpty() || !map_info.exists()) {
        setInlineStatus(QStringLiteral("A valid saved map is required before hull computation."),
                        QStringLiteral("#F87171"));
        return;
    }
    if (!cache.processing_complete || !cache.processed_cloud || cache.processed_cloud->empty()) {
        setInlineStatus(QStringLiteral("Process Point Cloud before computing the hull."),
                        QStringLiteral("#F59E0B"));
        updateButtonsAndStatus();
        return;
    }
    if (process_in_flight_ || hull_in_flight_) {
        return;
    }

    const quint64 generation = ++hull_generation_;
    hull_in_flight_ = true;
    updateButtonsAndStatus();
    setInlineStatus(QStringLiteral("Computing projected hull from the processed cloud..."),
                    QStringLiteral("#71717B"));

    QPointer<PlannerScreen> guard(this);
    const PointCloudPtr processed_cloud = cache.processed_cloud;
    const double alpha = cache.alpha;
    QtConcurrent::run([guard, generation, processed_cloud, alpha]() mutable {
        HullResult result;
        try {
            std::vector<Point2D> xy_points;
            xy_points.reserve(processed_cloud->size());
            for (const auto& point : processed_cloud->points) {
                if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                    continue;
                }
                xy_points.emplace_back(point.x, point.y);
            }
            if (xy_points.size() < 3) {
                throw std::runtime_error("Not enough finite XY points remain for hull computation.");
            }
            result.hull_polygon = computeConcaveHull(xy_points, alpha, "alphashape");
            if (result.hull_polygon.empty()) {
                throw std::runtime_error("Hull computation produced an empty boundary.");
            }
            result.area_m2 = polygonArea(result.hull_polygon);
            result.success = true;
        } catch (const std::exception& error) {
            result.error = QString::fromUtf8(error.what());
        }

        if (!guard) {
            return;
        }

        QMetaObject::invokeMethod(
            guard,
            [guard, generation, result = std::move(result)]() mutable {
                if (guard) {
                    guard->applyHullResult(generation, result);
                }
            },
            Qt::QueuedConnection);
    });
}

void PlannerScreen::startGenerateCoverage() {
    const QFileInfo map_info(map_path_);
    SessionCache& cache = activeSession();
    if (map_path_.isEmpty() || !map_info.exists()) {
        setInlineStatus(QStringLiteral("A valid saved map is required before coverage generation."),
                        QStringLiteral("#F87171"));
        updateButtonsAndStatus();
        return;
    }
    if (!cache.processing_complete) {
        setInlineStatus(QStringLiteral("Process Point Cloud before generating coverage paths."),
                        QStringLiteral("#F59E0B"));
        updateButtonsAndStatus();
        return;
    }
    if (!cache.hull_complete || cache.hull_polygon.empty()) {
        setInlineStatus(QStringLiteral("Compute and project the hull before generating coverage paths."),
                        QStringLiteral("#F59E0B"));
        updateButtonsAndStatus();
        return;
    }
    if (cache.coverage_roi_drawing_active || cache.coverage_drawing_active) {
        setInlineStatus(QStringLiteral("Finish or cancel the current drawing interaction first."),
                        QStringLiteral("#F59E0B"));
        updateButtonsAndStatus();
        return;
    }
    if (cache.coverage_scan_mode == QStringLiteral("roi") && cache.coverage_roi_polygon.size() < 3) {
        setInlineStatus(QStringLiteral("Draw an ROI before generating coverage paths."),
                        QStringLiteral("#F59E0B"));
        updateButtonsAndStatus();
        return;
    }
    if (planning_in_flight_ || process_in_flight_ || hull_in_flight_) {
        return;
    }

    invalidateCoverageResult();
    const quint64 generation = ++planning_generation_;
    planning_in_flight_ = true;
    updatePreview();
    updateButtonsAndStatus();
    setInlineStatus(QStringLiteral("Generating coverage paths from the projected hull..."),
                    QStringLiteral("#71717B"));

    QPointer<PlannerScreen> guard(this);
    const Polygon2D boundary = cache.hull_polygon;
    CoverageConfig cfg;
    cfg.swath_width = cache.coverage_path_spacing;
    cfg.headland_width = cache.coverage_headland_width;
    cfg.auto_align = true;
    cfg.align_mode = cache.coverage_scan_axis == QStringLiteral("parallel") ? "long" : "perp";
    cfg.route_pattern = cache.coverage_pattern.toStdString();
    cfg.path_planner = "dubins";
    // Roofus is skid-steer and can turn in place; axial turns produce
    // straight swath-to-swath transitions instead of Dubins arcs that the
    // platform can't follow without slipping. With axial turns enabled the
    // pipeline ignores `cfg.turn_radius`, so it is left at the struct
    // default. `waypoint_spacing = 0.0` disables the post-planning
    // resampler, keeping the route's exact corner waypoints.
    cfg.use_axial_turns = true;
    cfg.waypoint_spacing = 0.0;
    const Polygon2D roi = cache.coverage_scan_mode == QStringLiteral("roi") ? cache.coverage_roi_polygon
                                                                             : Polygon2D{};
    std::vector<Obstacle2D> obstacles;
    obstacles.reserve(cache.coverage_obstacles.size());
    for (const auto& obstacle : cache.coverage_obstacles) {
        if (obstacle.geometry.outer.size() >= 3) {
            obstacles.push_back(obstacle.geometry);
        }
    }

    QtConcurrent::run([guard, generation, boundary, cfg, roi, obstacles]() mutable {
        PlanningResult result;
        try {
            const Polygon2D* roi_ptr = roi.size() >= 3 ? &roi : nullptr;
            const std::vector<Obstacle2D>* obs_ptr = obstacles.empty() ? nullptr : &obstacles;
            result.coverage = generateCoverage(boundary, cfg, roi_ptr, obs_ptr);
            if (!result.coverage.success) {
                result.error = QString::fromStdString(result.coverage.error_message);
            } else {
                result.success = true;
            }
        } catch (const std::exception& error) {
            result.error = QString::fromUtf8(error.what());
        }

        if (!guard) {
            return;
        }

        QMetaObject::invokeMethod(
            guard,
            [guard, generation, result = std::move(result)]() mutable {
                if (guard) {
                    guard->applyPlanningResult(generation, result);
                }
            },
            Qt::QueuedConnection);
    });
}

void PlannerScreen::applyMapLoadResult(quint64 generation, const MapLoadResult& result) {
    if (generation != load_generation_) {
        return;
    }

    load_in_flight_ = false;
    SessionCache& cache = activeSession();
    if (!result.success) {
        cache.raw_loaded = false;
        cache.raw_cloud.reset();
        cache.raw_projected_points.clear();
        cache.raw_point_count = 0;
        cache.raw_area_estimate_m2 = 0.0;
        cache.processing_complete = false;
        cache.hull_complete = false;
        cache.processed_cloud.reset();
        cache.processed_projected_points.clear();
        cache.hull_polygon.clear();
        cache.processed_point_count = 0;
        cache.processed_area_estimate_m2 = 0.0;
        cache.hull_area_m2 = 0.0;
        cache.estimated_file_size_mb = 0.0;
        cache.quality_label.clear();
        invalidateCoverageResult();
        cache.coverage_roi_polygon.clear();
        cache.coverage_roi_drawing_active = false;
        cache.coverage_obstacles_detected = false;
        cache.coverage_obstacles.clear();
        cache.coverage_drawing_active = false;

        updatePreview();
        updateOutputCards();
        updateStatsChip();
        updatePlaceholderMessage();
        updateButtonsAndStatus();
        setInlineStatus(
            QStringLiteral("Saved map failed to load: %1").arg(result.error),
            QStringLiteral("#F87171"));
        return;
    }

    cache.raw_loaded = true;
    cache.raw_cloud = result.raw_cloud;
    cache.raw_projected_points = result.raw_projected_points;
    cache.raw_point_count = result.raw_point_count;
    cache.raw_area_estimate_m2 = result.area_estimate_m2;
    invalidateCoverageResult();
    cache.coverage_roi_polygon.clear();
    cache.coverage_roi_drawing_active = false;
    cache.coverage_obstacles_detected = false;
    cache.coverage_obstacles.clear();
    cache.coverage_drawing_active = false;

    updatePreview();
    if (plot_ && preview_stack_ && preview_stack_->currentWidget() == plot_) {
        plot_->resetView();
    }
    updateStatsChip();
    updatePlaceholderMessage();
    updateButtonsAndStatus();
    setInlineStatus(
        QStringLiteral("Loaded %1 points from %2.")
            .arg(formatCount(cache.raw_point_count))
            .arg(QFileInfo(map_path_).fileName()),
        QStringLiteral("#00D492"));
    if (autotest_enabled_) {
        std::cout << "PLANNER_AUTOTEST phase=map_loaded points=" << cache.raw_point_count
                  << std::endl;
    }
    maybeRunAutotest();
}

void PlannerScreen::applyProcessResult(quint64 generation, const ProcessResult& result) {
    if (generation != process_generation_) {
        return;
    }

    process_in_flight_ = false;
    SessionCache& cache = activeSession();
    if (!result.success) {
        std::cerr << "[PlannerScreen] point cloud processing failed: "
                  << result.error.toStdString() << std::endl;
        updateButtonsAndStatus();
        setInlineStatus(
            QStringLiteral("Point cloud processing failed: %1").arg(result.error),
            QStringLiteral("#F87171"));
        return;
    }

    cache.processing_complete = true;
    cache.hull_complete = false;
    cache.processed_cloud = result.processed_cloud;
    cache.processed_projected_points = result.processed_projected_points;
    cache.processed_point_count = result.processed_point_count;
    cache.processed_area_estimate_m2 = result.area_estimate_m2;
    cache.hull_polygon.clear();
    cache.hull_area_m2 = 0.0;
    cache.estimated_file_size_mb = result.estimated_file_size_mb;
    cache.quality_label = result.quality_label;
    invalidateCoverageResult();
    cache.coverage_roi_polygon.clear();
    cache.coverage_roi_drawing_active = false;
    cache.coverage_obstacles_detected = false;
    cache.coverage_obstacles.clear();
    cache.coverage_drawing_active = false;

    updatePreview();
    if (plot_ && preview_stack_ && preview_stack_->currentWidget() == plot_) {
        plot_->resetView();
    }
    updateOutputCards();
    updateStatsChip();
    updatePlaceholderMessage();
    updateButtonsAndStatus();
    setInlineStatus(
        QStringLiteral("Processed %1 -> %2 points.")
            .arg(formatCount(result.raw_point_count))
            .arg(formatCount(result.processed_point_count)),
        QStringLiteral("#00D492"));
    if (autotest_enabled_) {
        std::cout << "PLANNER_AUTOTEST phase=process_complete points="
                  << cache.processed_point_count << std::endl;
    }
    maybeRunAutotest();
}

void PlannerScreen::applyHullResult(quint64 generation, const HullResult& result) {
    if (generation != hull_generation_) {
        return;
    }

    hull_in_flight_ = false;
    SessionCache& cache = activeSession();
    if (!result.success) {
        std::cerr << "[PlannerScreen] hull computation failed: "
                  << result.error.toStdString() << std::endl;
        updateButtonsAndStatus();
        setInlineStatus(
            QStringLiteral("Hull computation failed: %1").arg(result.error),
            QStringLiteral("#F87171"));
        return;
    }

    cache.hull_complete = true;
    cache.hull_polygon = result.hull_polygon;
    cache.hull_area_m2 = result.area_m2;
    invalidateCoverageResult();
    cache.coverage_roi_polygon.clear();
    cache.coverage_roi_drawing_active = false;
    cache.coverage_obstacles_detected = false;
    cache.coverage_obstacles.clear();
    cache.coverage_drawing_active = false;

    updatePreview();
    if (plot_ && preview_stack_ && preview_stack_->currentWidget() == plot_) {
        plot_->resetView();
    }
    updateStatsChip();
    updatePlaceholderMessage();
    updateButtonsAndStatus();
    setInlineStatus(
        QStringLiteral("Hull ready with %1 vertices across %2.")
            .arg(cache.hull_polygon.size())
            .arg(formatArea(cache.hull_area_m2)),
        QStringLiteral("#00D492"));
    if (autotest_enabled_) {
        std::cout << "PLANNER_AUTOTEST phase=hull_complete vertices="
                  << cache.hull_polygon.size() << std::endl;
    }
    maybeRunAutotest();
}

void PlannerScreen::applyPlanningResult(quint64 generation, const PlanningResult& result) {
    if (generation != planning_generation_) {
        return;
    }

    planning_in_flight_ = false;
    if (!result.success) {
        std::cerr << "[PlannerScreen] coverage generation failed: "
                  << result.error.toStdString() << std::endl;
        updateButtonsAndStatus();
        const QString message = result.error.trimmed().isEmpty()
                                    ? QStringLiteral("Coverage generation failed: unknown error "
                                                     "(see terminal for details)")
                                    : QStringLiteral("Coverage generation failed: %1").arg(result.error);
        setInlineStatus(message, QStringLiteral("#F87171"));
        return;
    }

    SessionCache& cache = activeSession();
    cache.planning_complete = true;
    cache.planned_swaths = result.coverage.swaths;
    cache.planned_route = result.coverage.route;
    cache.planned_path = result.coverage.path;
    cache.planned_effective_area_m2 = result.coverage.effective_area_m2;

    updatePreview();
    if (plot_ && preview_stack_ && preview_stack_->currentWidget() == plot_) {
        plot_->resetView();
    }
    updatePlaceholderMessage();
    updateButtonsAndStatus();
    setInlineStatus(QStringLiteral("Generated %1 swaths and %2 path states.")
                        .arg(cache.planned_swaths.size())
                        .arg(cache.planned_path.size()),
                    QStringLiteral("#00D492"));
}

void PlannerScreen::startDetectObstacles() {
    SessionCache& cache = activeSession();
    if (!cache.raw_cloud || cache.raw_cloud->empty()) {
        setInlineStatus(QStringLiteral("Load a map before running auto-detect."),
                        QStringLiteral("#F87171"));
        return;
    }
    if (!cache.hull_complete) {
        setInlineStatus(QStringLiteral("Compute the hull before running auto-detect."),
                        QStringLiteral("#F59E0B"));
        return;
    }
    if (detect_in_flight_) {
        return;
    }

    const quint64 generation = ++detect_generation_;
    detect_in_flight_ = true;
    cache.coverage_obstacles_detected = false;
    cache.coverage_obstacles.clear();
    invalidateCoverageResult();
    updateButtonsAndStatus();
    setInlineStatus(QStringLiteral("Auto-detecting obstacles from the point cloud..."),
                    QStringLiteral("#71717B"));

    QPointer<PlannerScreen> guard(this);
    const PointCloudPtr cloud = cache.raw_cloud;

    // Convert Point2D trail to PathState for footprint ground sampling.
    // Headings are computed from consecutive points; last pose reuses predecessor.
    std::vector<PathState> trail;
    trail.reserve(live_robot_trail_.size());
    for (size_t i = 0; i < live_robot_trail_.size(); ++i) {
        double heading = 0.0;
        if (i + 1 < live_robot_trail_.size()) {
            const Point2D& a = live_robot_trail_[i];
            const Point2D& b = live_robot_trail_[i + 1];
            heading = std::atan2(b.y - a.y, b.x - a.x);
        } else if (i > 0) {
            heading = trail.back().heading;
        }
        trail.push_back(PathState{live_robot_trail_[i], heading});
    }

    // ROI filter applied in-thread so callers don't need to wait.
    const Polygon2D roi = (cache.coverage_scan_mode == QStringLiteral("roi") &&
                           cache.coverage_roi_polygon.size() >= 3)
                              ? cache.coverage_roi_polygon
                              : Polygon2D{};

    QtConcurrent::run([guard, generation, cloud, trail, roi]() mutable {
        ObstacleDetectionParams params;
        ObstacleDetectionResult result = detectObstaclesAuto(cloud, trail, nullptr, params);

        if (result.success && roi.size() >= 3) {
            std::vector<Obstacle2D> filtered;
            filtered.reserve(result.obstacles.size());
            for (const auto& obs : result.obstacles) {
                bool keep = false;
                for (const auto& p : obs.outer) {
                    const int n = static_cast<int>(roi.size());
                    bool inside = false;
                    for (int j = 0, k = n - 1; j < n; k = j++) {
                        if (((roi[j].y > p.y) != (roi[k].y > p.y)) &&
                            (p.x < (roi[k].x - roi[j].x) * (p.y - roi[j].y) /
                                           (roi[k].y - roi[j].y) +
                                       roi[j].x)) {
                            inside = !inside;
                        }
                    }
                    if (inside) {
                        keep = true;
                        break;
                    }
                }
                if (keep) {
                    filtered.push_back(obs);
                }
            }
            result.obstacles = std::move(filtered);
            result.stats.obstacle_shapes = static_cast<int>(result.obstacles.size());
        }

        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard,
            [guard, generation, result = std::move(result)]() mutable {
                if (guard) {
                    guard->applyDetectResult(generation, std::move(result));
                }
            },
            Qt::QueuedConnection);
    });
}

void PlannerScreen::applyDetectResult(quint64 generation, ObstacleDetectionResult result) {
    if (generation != detect_generation_) {
        return;
    }

    detect_in_flight_ = false;
    SessionCache& cache = activeSession();

    if (!result.success) {
        updateButtonsAndStatus();
        setInlineStatus(
            QStringLiteral("Obstacle detection failed: %1")
                .arg(QString::fromStdString(result.error_message)),
            QStringLiteral("#F87171"));
        return;
    }

    cache.coverage_obstacles_detected = true;
    cache.coverage_obstacles.clear();
    cache.coverage_obstacles.reserve(result.obstacles.size());
    for (auto& obs : result.obstacles) {
        SessionCache::CoverageObstacle co;
        co.id = cache.coverage_next_obstacle_id++;
        co.type = QStringLiteral("auto");
        co.source = QStringLiteral("auto-detect");
        co.geometry = std::move(obs);
        cache.coverage_obstacles.push_back(std::move(co));
    }

    invalidateCoverageResult(
        QStringLiteral("Auto-detected %1 obstacle(s). Generate coverage paths again to refresh the preview.")
            .arg(cache.coverage_obstacles.size()));
    rebuildCoverageObstacleRows();
    updatePreview();
    updateButtonsAndStatus();
    setInlineStatus(
        QStringLiteral("Auto-detected %1 obstacle(s) from the point cloud.")
            .arg(cache.coverage_obstacles.size()),
        QStringLiteral("#00D492"));
}

// =============================================================================
// Scan Splitting
// =============================================================================

namespace {

constexpr double kScanWaypointDuplicateEpsilon = 1e-3;
constexpr int kScanSegmentRowHeight = 52;
constexpr int kScanSegmentCompletedRole = Qt::UserRole + 1;

// Custom delegate for the scan-segment list. Paints two extras on top of the
// stylesheet-driven row:
//   * a 4px green accent stripe on the left edge of the row that currently
//     has focus (mirrors the plot-side "active segment" highlight)
//   * a green checkmark on the right edge of any row whose segment is
//     marked completed (auto-deselect+lock policy enforces unselected state)
class ScanSegmentItemDelegate : public QStyledItemDelegate {
public:
    ScanSegmentItemDelegate(QListWidget* list, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), list_(list) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (!painter) {
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        if (list_ && list_->currentRow() == index.row()) {
            const QRect r = option.rect;
            const QRect stripe(r.left() + 2, r.top() + 6, 4, r.height() - 12);
            painter->fillRect(stripe, QColor("#10B981"));
        }

        if (index.data(kScanSegmentCompletedRole).toBool()) {
            const int sz = 18;
            const int margin = 14;
            const QRect chk(option.rect.right() - margin - sz,
                            option.rect.center().y() - sz / 2,
                            sz, sz);
            const QColor color = option.state.testFlag(QStyle::State_Selected)
                                     ? QColor("#FFFFFF")
                                     : QColor("#00D492");
            QPen pen(color, 2.5);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter->setPen(pen);
            QPainterPath path;
            path.moveTo(chk.left() + 2, chk.center().y());
            path.lineTo(chk.center().x() - 1, chk.bottom() - 3);
            path.lineTo(chk.right() - 2, chk.top() + 3);
            painter->drawPath(path);
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override {
        QSize sz = QStyledItemDelegate::sizeHint(option, index);
        sz.setHeight(kScanSegmentRowHeight);
        return sz;
    }

private:
    QListWidget* list_;
};

PathStateList dedupeScanPathStates(const PathStateList& path) {
    PathStateList filtered;
    filtered.reserve(path.size());
    for (const auto& state : path) {
        if (!filtered.empty()) {
            const double dx = state.point.x - filtered.back().point.x;
            const double dy = state.point.y - filtered.back().point.y;
            if (std::fabs(dx) <= kScanWaypointDuplicateEpsilon &&
                std::fabs(dy) <= kScanWaypointDuplicateEpsilon) {
                continue;
            }
        }
        filtered.push_back(state);
    }
    return filtered;
}

int estimateScanTurns(const PathStateList& seg) {
    if (seg.size() < 3) return 0;
    int turns = 0;
    constexpr double angle_thresh = 25.0 * M_PI / 180.0;
    for (size_t i = 1; i + 1 < seg.size(); ++i) {
        const auto& p0 = seg[i - 1].point;
        const auto& p1 = seg[i].point;
        const auto& p2 = seg[i + 1].point;
        const double v1x = p1.x - p0.x;
        const double v1y = p1.y - p0.y;
        const double v2x = p2.x - p1.x;
        const double v2y = p2.y - p1.y;
        const double len1 = std::hypot(v1x, v1y);
        const double len2 = std::hypot(v2x, v2y);
        if (len1 < 1e-3 || len2 < 1e-3) continue;
        const double dot = v1x * v2x + v1y * v2y;
        const double det = v1x * v2y - v1y * v2x;
        const double angle = std::fabs(std::atan2(det, dot));
        if (angle >= angle_thresh) {
            ++turns;
        }
    }
    return turns;
}

}  // namespace

void PlannerScreen::invalidateScanSegments(const QString& status_message) {
    SessionCache& cache = activeSession();
    cache.scan_segments.clear();
    cache.scan_splits_dirty = true;
    cache.scan_waypoints_published = false;
    // Splits changed -> any prior preflight ack is stale. Force the
    // operator to re-acknowledge before the next Scan stage entry.
    cache.scan_preflight_acknowledged = false;
    if (lbl_scan_splitting_status_ && !status_message.isEmpty()) {
        lbl_scan_splitting_status_->setText(status_message);
    }
}

void PlannerScreen::rebuildScanSegments() {
    SessionCache& cache = activeSession();
    cache.scan_segments.clear();
    cache.scan_waypoints_published = false;

    PathStateList base = dedupeScanPathStates(cache.planned_path);
    if (base.size() < 2) {
        cache.scan_splits_dirty = true;
        if (lbl_scan_splitting_status_) {
            lbl_scan_splitting_status_->setText(
                QStringLiteral("Generate a coverage plan before splitting scans."));
        }
        return;
    }

    std::vector<double> cum(base.size(), 0.0);
    for (size_t i = 1; i < base.size(); ++i) {
        cum[i] = cum[i - 1] + std::hypot(base[i].point.x - base[i - 1].point.x,
                                         base[i].point.y - base[i - 1].point.y);
    }
    const double total = cum.back();
    const double seg_len = std::max(0.5, cache.scan_distance_m);

    const double min_segment_m =
        std::max(0.05, std::min(0.25, seg_len * 0.10));  // suppress tiny leftovers

    auto addSeg = [&](size_t a, size_t b, const QString& name) -> bool {
        if (b <= a || b >= cum.size()) {
            return false;
        }
        const double length_m = cum[b] - cum[a];
        if (length_m <= min_segment_m) {
            return false;
        }
        SessionCache::ScanSegment s;
        s.name = name;
        s.start_m = cum[a];
        s.end_m = cum[b];
        s.length_m = length_m;
        s.path.assign(base.begin() + a, base.begin() + b + 1);
        s.turns = estimateScanTurns(s.path);
        s.completed = false;
        s.selected = true;
        cache.scan_segments.push_back(std::move(s));
        return true;
    };

    size_t start_idx = 0;
    int idx = 1;
    for (double target = seg_len; target < total && start_idx < base.size() - 1;
         target += seg_len) {
        size_t upper =
            std::lower_bound(cum.begin() + start_idx + 1, cum.end(), target) - cum.begin();
        if (upper >= base.size()) break;
        const size_t lower = upper > 0 ? upper - 1 : upper;
        size_t cut = (target - cum[lower] <= cum[upper] - target) ? lower : upper;
        if (cut <= start_idx) cut = std::min(start_idx + 1, base.size() - 1);
        if (addSeg(start_idx, cut, QString("Segment %1").arg(idx))) {
            ++idx;
        }
        start_idx = cut;
    }
    if (addSeg(start_idx, base.size() - 1, QString("Segment %1").arg(idx))) {
        ++idx;
    }

    // If the path is very short, keep a single segment as long as it is meaningful.
    if (cache.scan_segments.empty() && total > min_segment_m) {
        (void)addSeg(0, base.size() - 1, QString("Segment 1"));
    }

    cache.scan_splits_dirty = false;
    if (lbl_scan_splitting_status_) {
        if (cache.scan_segments.empty()) {
            lbl_scan_splitting_status_->setText(
                QStringLiteral("No usable segments after filtering tiny leftovers."));
        } else {
            lbl_scan_splitting_status_->setText(
                QStringLiteral("Path divided into %1 segment(s) at %2 m each.")
                    .arg(cache.scan_segments.size())
                    .arg(seg_len, 0, 'f', 1));
        }
    }
}

void PlannerScreen::refreshScanSegmentList() {
    if (!list_scan_segments_) {
        return;
    }
    QSignalBlocker blocker(list_scan_segments_);
    list_scan_segments_->clear();
    const SessionCache* cache = activeSessionPtr();
    if (!cache) {
        if (lbl_scan_segments_footer_) {
            lbl_scan_segments_footer_->setText(QStringLiteral("Segments: none"));
        }
        return;
    }
    size_t done = 0;
    for (const auto& seg : cache->scan_segments) {
        auto* item = new QListWidgetItem(
            QString("%1  %2 m, %3 turns")
                .arg(seg.name)
                .arg(seg.length_m, 0, 'f', 1)
                .arg(seg.turns));
        item->setData(kScanSegmentCompletedRole, seg.completed);
        if (seg.completed) {
            item->setForeground(QColor(QStringLiteral("#00D492")));
        }
        list_scan_segments_->addItem(item);
        item->setSelected(seg.selected);
        if (seg.completed) ++done;
    }
    if (lbl_scan_segments_footer_) {
        if (cache->scan_segments.empty()) {
            lbl_scan_segments_footer_->setText(QStringLiteral("Segments: none"));
        } else {
            lbl_scan_segments_footer_->setText(QString("Segments: %1 total | %2 done")
                                                   .arg(cache->scan_segments.size())
                                                   .arg(done));
        }
    }
}

void PlannerScreen::pushScanSegmentsToPlot() {
    if (!plot_) {
        return;
    }
    const SessionCache* cache = activeSessionPtr();
    if (!cache || cache->scan_segments.empty()) {
        plot_->setScanSegments({}, {}, {}, {}, false);
        return;
    }
    std::vector<PathStateList> seg_paths;
    std::vector<QString> labels;
    std::vector<double> lengths;
    std::vector<int> turns;
    std::vector<bool> selected;
    seg_paths.reserve(cache->scan_segments.size());
    labels.reserve(cache->scan_segments.size());
    lengths.reserve(cache->scan_segments.size());
    turns.reserve(cache->scan_segments.size());
    selected.reserve(cache->scan_segments.size());
    for (const auto& seg : cache->scan_segments) {
        seg_paths.push_back(seg.path);
        labels.push_back(seg.name);
        lengths.push_back(seg.length_m);
        turns.push_back(seg.turns);
        selected.push_back(seg.selected);
    }
    plot_->setScanSegments(seg_paths, labels, lengths, turns, true, selected);
    if (current_step_ == PlannerStep::Scan) {
        plot_->setActiveScanSegment(cache->scan_active_segment_index);
    }
}

std::vector<int> PlannerScreen::selectedScanSegmentIndices() const {
    std::vector<int> out;
    const SessionCache* cache = activeSessionPtr();
    if (!cache) return out;
    for (size_t i = 0; i < cache->scan_segments.size(); ++i) {
        if (cache->scan_segments[i].selected) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

PathStateList PlannerScreen::buildPublishPathFromSegments(
    const std::vector<int>& indices) const {
    PathStateList combined;
    const SessionCache* cache = activeSessionPtr();
    if (!cache) return combined;
    for (int idx : indices) {
        if (idx < 0 || idx >= static_cast<int>(cache->scan_segments.size())) continue;
        const auto& seg = cache->scan_segments[idx].path;
        if (seg.empty()) continue;
        size_t start = 0;
        if (!combined.empty()) {
            const auto& prev = combined.back().point;
            if (std::hypot(prev.x - seg.front().point.x,
                           prev.y - seg.front().point.y) < 1e-6) {
                start = 1;
            }
        }
        combined.insert(combined.end(), seg.begin() + start, seg.end());
    }
    return dedupeScanPathStates(combined);
}

void PlannerScreen::onScanDistanceEdited() {
    if (!edit_scan_distance_) return;
    bool ok = false;
    const double value = edit_scan_distance_->text().toDouble(&ok);
    SessionCache& cache = activeSession();
    if (ok && value > 0.0) {
        if (std::fabs(value - cache.scan_distance_m) > 1e-6) {
            cache.scan_distance_m = value;
            // Re-split immediately when a coverage plan exists. Selections
            // are intentionally cleared by rebuildScanSegments — the
            // operator will re-tick segments before publishing.
            if (cache.planning_complete && cache.planned_path.size() >= 2) {
                rebuildScanSegments();
            } else {
                invalidateScanSegments(
                    QStringLiteral("Distance changed. Generate a coverage plan to split."));
            }
        }
    } else {
        edit_scan_distance_->setText(
            QString::number(cache.scan_distance_m, 'f', 2));
    }
    refreshScanSegmentList();
    pushScanSegmentsToPlot();
    updateScanSplittingUi();
}

void PlannerScreen::onProgressionModeChanged(const QString& mode) {
    SessionCache& cache = activeSession();
    cache.scan_progression_mode =
        (mode == QStringLiteral("manual")) ? QStringLiteral("manual")
                                           : QStringLiteral("automatic");
    if (cache.scan_progression_mode == QStringLiteral("manual") &&
        lbl_scan_splitting_status_) {
        lbl_scan_splitting_status_->setText(QStringLiteral(
            "Manual progression enabled: you'll confirm before each next segment."));
    }
    updateScanSplittingUi();
}

void PlannerScreen::onSplitPathClicked() {
    const SessionCache* cache_ptr = activeSessionPtr();
    if (!cache_ptr || !cache_ptr->planning_complete ||
        cache_ptr->planned_path.size() < 2) {
        if (lbl_scan_splitting_status_) {
            lbl_scan_splitting_status_->setText(
                QStringLiteral("Generate a coverage plan before splitting scans."));
        }
        updateScanSplittingUi();
        return;
    }
    rebuildScanSegments();
    refreshScanSegmentList();
    pushScanSegmentsToPlot();
    updateScanSplittingUi();
}

void PlannerScreen::showScanPreflightDialog() {
    // Already acknowledged this round (e.g. another async call raced
    // into here). Bail out quietly.
    if (activeSession().scan_preflight_acknowledged) {
        return;
    }

    auto* dialog = new PreScanChecklistDialog(dark_mode_, this);
    // Forward Wake GPR clicks up to AppShellWindow via the planner-level
    // signal; AppShellWindow's onPlannerWakeGprRequested fires the
    // /gpr_line_stop Trigger.
    connect(dialog, &PreScanChecklistDialog::wakeGprRequested,
            this, &PlannerScreen::wakeGprRequested);

    // Blur the Stage 4 content behind the dialog so the operator's eye
    // is drawn to the modal. Mirrors the exploration_screen pattern at
    // exploration_screen.cpp:1918. Effect attached to `this` because
    // PlannerScreen owns its layout directly (no content_root_ wrapper).
    auto* blur = new QGraphicsBlurEffect(this);
    blur->setBlurRadius(10.0);
    setGraphicsEffect(blur);

    // Center the dialog on the PlannerScreen, deferred until after the
    // dialog has its real size (post-show).
    QPointer<PreScanChecklistDialog> guard(dialog);
    QTimer::singleShot(0, dialog, [this, guard]() {
        if (!guard) return;
        const QPoint top_left = mapToGlobal(QPoint(
            (width() - guard->width()) / 2,
            (height() - guard->height()) / 2));
        guard->move(top_left);
    });

    const int rc = dialog->exec();

    setGraphicsEffect(nullptr);
    dialog->deleteLater();

    if (rc == QDialog::Accepted) {
        activeSession().scan_preflight_acknowledged = true;
    }
}

void PlannerScreen::onPublishSelectedClicked() {
    const auto indices = selectedScanSegmentIndices();
    if (indices.empty()) {
        BdrMessageBox::information(
            this,
            QStringLiteral("No selection"),
            QStringLiteral("Select one or more scan segments before publishing."));
        return;
    }
    const PathStateList publish_path = buildPublishPathFromSegments(indices);
    if (publish_path.size() < 2) {
        BdrMessageBox::warning(this,
                               QStringLiteral("Empty path"),
                               QStringLiteral("Selected segments contain no waypoints."));
        return;
    }
    std::vector<double> xy_pairs;
    xy_pairs.reserve(publish_path.size() * 2);
    for (const auto& st : publish_path) {
        xy_pairs.push_back(st.point.x);
        xy_pairs.push_back(st.point.y);
    }
    SessionCache& cache = activeSession();
    cache.scan_waypoints_published = true;
    if (lbl_scan_splitting_status_) {
        lbl_scan_splitting_status_->setText(
            QStringLiteral("Published %1 segment(s), %2 waypoints to /f2c_waypoints.")
                .arg(indices.size())
                .arg(publish_path.size()));
    }
    emit publishScanSegmentsRequested(xy_pairs);
    updateScanSplittingUi();
}

void PlannerScreen::onStartSelectedClicked() {
    SessionCache& cache = activeSession();
    const auto indices = selectedScanSegmentIndices();
    if (indices.empty()) {
        BdrMessageBox::information(
            this,
            QStringLiteral("No selection"),
            QStringLiteral("Select one or more scan segments before starting."));
        return;
    }
    cache.scan_waypoints_published = true;
    if (lbl_scan_splitting_status_) {
        lbl_scan_splitting_status_->setText(
            QStringLiteral("Ready to execute selected segments. Press Start Scan."));
    }
    updateScanSplittingUi();

    // Advance into Stage 4 (Scan execution).
    enterScanStage();
}

void PlannerScreen::updateScanSplittingUi() {
    const SessionCache* cache = activeSessionPtr();
    if (!cache) return;

    if (edit_scan_distance_) {
        QSignalBlocker blocker(edit_scan_distance_);
        const QString want =
            QString::number(cache->scan_distance_m,
                            cache->scan_distance_m == std::floor(cache->scan_distance_m) ? 'f' : 'f',
                            2);
        if (edit_scan_distance_->text() != want) {
            edit_scan_distance_->setText(want);
        }
    }

    auto styleToggle = [&](QPushButton* btn, bool active) {
        if (!btn) return;
        btn->setChecked(active);
        if (active) {
            btn->setStyleSheet(QStringLiteral(
                "QPushButton { background: rgba(0,188,125,0.15); border: 1px solid rgba(0,188,125,0.30); "
                "  border-radius: 8px; color: #00D492; font-family: 'Arimo'; font-size: 14px; "
                "  font-weight: 700; }"));
        } else {
            btn->setStyleSheet(QStringLiteral(
                "QPushButton { background: transparent; border: none; border-radius: 8px; "
                "  color: #71717B; font-family: 'Arimo'; font-size: 14px; font-weight: 400; }"
                "QPushButton:hover { color: #D4D4D8; }"));
        }
    };
    styleToggle(btn_progression_automatic_,
                cache->scan_progression_mode != QStringLiteral("manual"));
    styleToggle(btn_progression_manual_,
                cache->scan_progression_mode == QStringLiteral("manual"));

    const bool plan_ready = cache->planning_complete && cache->planned_path.size() >= 2;
    if (btn_scan_split_path_) {
        btn_scan_split_path_->setEnabled(plan_ready);
        btn_scan_split_path_->setToolTip(
            plan_ready ? QStringLiteral("Divide the planned path into fixed-distance segments.")
                       : QStringLiteral("Generate a coverage plan first."));
    }
    const bool has_selection = !selectedScanSegmentIndices().empty();
    if (btn_scan_publish_selected_) {
        btn_scan_publish_selected_->setEnabled(plan_ready && has_selection);
        btn_scan_publish_selected_->setToolTip(
            !plan_ready ? QStringLiteral("Generate a coverage plan first.")
            : !has_selection
                ? QStringLiteral("Tick one or more segments to publish.")
                : QStringLiteral("Publish the selected segments to /f2c_waypoints."));
    }
    if (btn_scan_start_selected_) {
        btn_scan_start_selected_->setEnabled(plan_ready && has_selection);
        btn_scan_start_selected_->setToolTip(
            !plan_ready ? QStringLiteral("Generate a coverage plan first.")
            : !has_selection ? QStringLiteral("Tick one or more segments to start.")
                             : QStringLiteral("Start navigating the published segments."));
    }

    if (current_step_ == PlannerStep::ScanSplitting) {
        refreshScanSegmentList();
    }
}

void PlannerScreen::buildUi() {
    setObjectName(QStringLiteral("PlannerScreenRoot"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background: #09090B;"));

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    const QString kInitialLabel12 =
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: #FFFFFF;");
    const QString kInitialLabel10 =
        QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: #71717B;");
    const QString kInitialHeading10 =
        QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 700; color: #9F9FA9; "
                       "letter-spacing: 0.5px;");
    const QString kInitialStatus14 =
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: #9F9FA9;");
    const QString kInitialMono12Green =
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 600; color: #00D492;");
    const QString kInitialMono12White =
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 600; color: #FFFFFF;");
    const QString kInitialMono12Muted =
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 400; color: #71717B;");
    const QString kInitialMono9Muted =
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 11px; font-weight: 400; color: #52525C;");
    auto trackLabel = [](QLabel* label, std::vector<QLabel*>* bucket) -> QLabel* {
        if (label && bucket) {
            bucket->push_back(label);
        }
        return label;
    };
    auto makeTrackedLabel = [&](QWidget* parent,
                                const QString& text,
                                const QString& style,
                                std::vector<QLabel*>* bucket,
                                Qt::Alignment alignment =
                                    Qt::AlignLeft | Qt::AlignVCenter) -> QLabel* {
        return trackLabel(makeTextLabel(parent, text, style, alignment), bucket);
    };

    auto make_stage_step =
        [&](QWidget* parent,
            const QString& icon_path,
            const QString& text,
            int group_width,
            int button_width,
            bool with_separator) {
            StageStepUi step;
            step.icon_path = icon_path;
            step.group_width = group_width;
            step.button_width = button_width;
            step.wrapper = new QWidget(parent);
            step.wrapper->setFixedSize(group_width, 34);
            auto* wrapper_layout = new QHBoxLayout(step.wrapper);
            wrapper_layout->setContentsMargins(0, 0, 0, 0);
            wrapper_layout->setSpacing(4);
            step.button = new QPushButton(step.wrapper);
            step.button->setFlat(true);
            step.button->setCursor(Qt::PointingHandCursor);
            step.button->setFixedSize(button_width, 32);
            step.button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            auto* layout = new QHBoxLayout(step.button);
            layout->setContentsMargins(16, 0, 16, 0);
            layout->setSpacing(8);
            step.icon = makeIconLabel(step.button, icon_path, 14, QStringLiteral("#71717B"));
            step.text = makeTextLabel(
                step.button,
                text,
                QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; "
                               "color: #71717B;"),
                Qt::AlignCenter);
            step.text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            layout->addWidget(step.icon, 0, Qt::AlignVCenter);
            layout->addWidget(step.text, 1, Qt::AlignVCenter);
            wrapper_layout->addWidget(step.button, 0, Qt::AlignVCenter);
            if (with_separator) {
                step.separator = new QWidget(step.wrapper);
                step.separator->setFixedSize(24, 1);
                step.separator->setAttribute(Qt::WA_StyledBackground, true);
                step.separator->setStyleSheet(QStringLiteral("background: #3F3F47;"));
                stage_separator_widgets_.push_back(step.separator);
                wrapper_layout->addWidget(step.separator, 0, Qt::AlignVCenter);
            }
            return step;
        };

    auto make_output_card =
        [&](QWidget* parent, const QString& title, QLabel** value_label) -> QWidget* {
            auto* card = new QWidget(parent);
            card->setFixedHeight(52);
            card->setAttribute(Qt::WA_StyledBackground, true);
            card->setStyleSheet(QStringLiteral(
                "background: rgba(39,39,42,0.5);"
                "border: 1px solid #27272A;"
                "border-radius: 10px;"));
            output_cards_.push_back(card);

            auto* layout = new QVBoxLayout(card);
            layout->setContentsMargins(11, 7, 11, 7);
            layout->setSpacing(2);
            layout->addWidget(makeTrackedLabel(card,
                                               title,
                                               QStringLiteral("font-family: 'Arimo'; font-size: "
                                                              "11px; font-weight: 400; color: "
                                                              "#71717B;"),
                                               &label9_labels_));
            *value_label =
                makeTrackedLabel(card, QStringLiteral("--"), kInitialMono12White, &mono12_white_labels_);
            layout->addWidget(*value_label);
            return card;
        };

    auto make_tool_button =
        [&](QWidget* parent, const QString& icon_path, QLabel** out_icon) -> QPushButton* {
        auto* button = new QPushButton(parent);
        button->setCursor(Qt::PointingHandCursor);
        button->setFlat(true);
        button->setFixedSize(32, 32);
        button->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: rgba(39,39,42,0.9);"
            "  border: 1px solid #3F3F47;"
            "  border-radius: 10px;"
            "}"
            "QPushButton:hover { border-color: #52525C; }"));

        auto* layout = new QHBoxLayout(button);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addStretch(1);
        auto* icon = makeIconLabel(button, icon_path, 14, QStringLiteral("#E4E4E7"));
        if (out_icon) {
            *out_icon = icon;
        }
        layout->addWidget(icon);
        layout->addStretch(1);
        return button;
    };

    top_bar_ = new QWidget(this);
    top_bar_->setFixedHeight(53);
    top_bar_->setAttribute(Qt::WA_StyledBackground, true);
    top_bar_->setStyleSheet(QStringLiteral("background: #18181B;"));
    auto* top_layout = new QHBoxLayout(top_bar_);
    top_layout->setContentsMargins(24, 0, 24, 0);
    top_layout->setSpacing(0);

    auto* title_row = new QWidget(top_bar_);
    auto* title_row_layout = new QHBoxLayout(title_row);
    title_row_layout->setContentsMargins(0, 0, 0, 0);
    title_row_layout->setSpacing(16);

    btn_back_ = new QPushButton(title_row);
    btn_back_->setCursor(Qt::PointingHandCursor);
    btn_back_->setFlat(true);
    btn_back_->setMinimumWidth(135);
    btn_back_->setFixedHeight(36);
    btn_back_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    btn_back_->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; border-radius: 10px; }"
        "QPushButton:hover { background: rgba(39,39,42,0.55); }"));
    auto* back_layout = new QHBoxLayout(btn_back_);
    back_layout->setContentsMargins(12, 0, 12, 0);
    back_layout->setSpacing(8);
    lbl_back_icon_ = makeIconLabel(btn_back_,
                                   QStringLiteral(":/assets/missionplanner/back.svg"),
                                   16,
                                   QStringLiteral("#9F9FA9"));
    back_layout->addWidget(lbl_back_icon_);
    lbl_back_text_ = makeTextLabel(
        btn_back_,
        QStringLiteral("Exploration"),
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 400; color: #9F9FA9;"));
    back_layout->addWidget(lbl_back_text_);
    back_layout->addStretch(1);
    connect(btn_back_, &QPushButton::clicked, this, &PlannerScreen::onBackClicked);
    title_row_layout->addWidget(btn_back_);

    title_divider_ = new QWidget(title_row);
    title_divider_->setFixedSize(1, 24);
    title_divider_->setAttribute(Qt::WA_StyledBackground, true);
    title_divider_->setStyleSheet(QStringLiteral("background: #27272A;"));
    title_row_layout->addWidget(title_divider_, 0, Qt::AlignVCenter);

    lbl_title_ = makeTextLabel(
        title_row,
        QStringLiteral("Mission Planner"),
        QStringLiteral("font-family: 'Arimo'; font-size: 24px; font-weight: 700; color: #FFFFFF;"));
    title_row_layout->addWidget(lbl_title_);

    top_layout->addWidget(title_row, 0, Qt::AlignVCenter);
    top_layout->addStretch(1);

    auto* top_right_host = new QWidget(top_bar_);
    top_right_host->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* top_right_layout = new QHBoxLayout(top_right_host);
    top_right_layout->setContentsMargins(0, 0, 0, 0);
    top_right_layout->setSpacing(24);

    auto* status_bar = new QWidget(top_right_host);
    status_bar->setFixedHeight(kStatusItemHeight);
    status_bar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* status_layout = new QHBoxLayout(status_bar);
    status_layout->setContentsMargins(0, 0, 0, 0);
    status_layout->setSpacing(24);
    status_layout->addWidget(makeStatusItem(status_bar,
                                            QStringLiteral(":/assets/missionplanner/battery.svg"),
                                            16,
                                            QStringLiteral("87%"),
                                            kStatusBatteryMinWidth,
                                            kInitialStatus14,
                                            QString(),
                                            &lbl_top_battery_));
    status_layout->addWidget(makeStatusItem(status_bar,
                                            QStringLiteral(":/assets/missionplanner/status_dot.svg"),
                                            8,
                                            top_signal_text_,
                                            kStatusSignalMinWidth,
                                            kInitialStatus14,
                                            QString(),
                                            &lbl_top_signal_));
    auto* lock_item = new QWidget(status_bar);
    lock_item->setFixedHeight(kStatusItemHeight);
    lock_item->setMinimumWidth(kStatusLockMinWidth);
    lock_item->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* lock_layout = new QHBoxLayout(lock_item);
    lock_layout->setContentsMargins(0, 0, 0, 0);
    lock_layout->setSpacing(8);
    lock_layout->addWidget(
        makeIconLabel(lock_item, QStringLiteral(":/assets/missionplanner/lock.svg"), 16),
        0,
        Qt::AlignVCenter);
    lbl_top_lock_chip_ = makeTextLabel(lock_item, top_lock_text_, kInitialStatus14);
    lock_layout->addWidget(lbl_top_lock_chip_, 0, Qt::AlignVCenter);
    status_layout->addWidget(lock_item);

    top_motors_chip_ = new QWidget(status_bar);
    top_motors_chip_->setFixedSize(kStatusMotorsChipMinWidth, kStatusMotorsChipHeight);
    top_motors_chip_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    top_motors_chip_->setAttribute(Qt::WA_StyledBackground, true);
    auto* motors_layout = new QHBoxLayout(top_motors_chip_);
    motors_layout->setContentsMargins(kStatusMotorsChipHorizontalPadding,
                                      1,
                                      kStatusMotorsChipHorizontalPadding,
                                      1);
    motors_layout->setSpacing(kStatusMotorsChipSpacing);
    lbl_top_motors_dot_ = makeIconLabel(top_motors_chip_,
                                        QStringLiteral(":/assets/missionplanner/motors_armed_dot.svg"),
                                        6,
                                        QStringLiteral("#71717B"));
    motors_layout->addWidget(lbl_top_motors_dot_, 0, Qt::AlignVCenter);
    lbl_top_motors_text_ = makeTextLabel(
        top_motors_chip_,
        top_motors_text_,
        QStringLiteral("font-family: 'Arimo'; font-size: 10px; font-weight: 700; "
                       "color: #71717B; letter-spacing: 0.5px;"));
    motors_layout->addWidget(lbl_top_motors_text_, 0, Qt::AlignVCenter);
    status_layout->addWidget(top_motors_chip_);
    top_right_layout->addWidget(status_bar, 0, Qt::AlignVCenter);

    auto* window_controls_reserve = new QWidget(top_right_host);
    window_controls_reserve->setFixedWidth(kWindowControlsReservedWidth);
    window_controls_reserve->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    top_right_layout->addWidget(window_controls_reserve);

    top_layout->addWidget(top_right_host, 0, Qt::AlignVCenter);
    root_layout->addWidget(top_bar_);

    stage_header_ = new QWidget(this);
    stage_header_->setFixedHeight(65);
    stage_header_->setAttribute(Qt::WA_StyledBackground, true);
    stage_header_->setStyleSheet(QStringLiteral("background: #18181B;"));
    auto* stage_header_layout = new QHBoxLayout(stage_header_);
    stage_header_layout->setContentsMargins(0, 0, 0, 0);
    stage_header_layout->setSpacing(0);

    left_header_ = new QWidget(stage_header_);
    left_header_->setFixedWidth(kPlannerLeftRailWidth);
    left_header_->setAttribute(Qt::WA_StyledBackground, true);
    left_header_->setStyleSheet(
        QStringLiteral(
            "border-top: 1px solid #27272A; border-right: 1px solid #27272A; border-bottom: 1px solid #27272A;"));
    auto* left_header_layout = new QHBoxLayout(left_header_);
    left_header_layout->setContentsMargins(24, 12, 24, 12);
    left_header_layout->setSpacing(12);

    left_header_icon_box_ = new QWidget(left_header_);
    left_header_icon_box_->setFixedSize(36, 36);
    left_header_icon_box_->setAttribute(Qt::WA_StyledBackground, true);
    left_header_icon_box_->setStyleSheet(QStringLiteral(
        "background: rgba(0,188,125,0.1);"
        "border: 1px solid rgba(0,188,125,0.2);"
        "border-radius: 10px;"));
    auto* left_header_icon_layout = new QHBoxLayout(left_header_icon_box_);
    left_header_icon_layout->setContentsMargins(0, 0, 0, 0);
    left_header_icon_layout->addStretch(1);
    lbl_left_header_icon_ = makeIconLabel(left_header_icon_box_,
                                          QStringLiteral(":/assets/missionplanner/point_cloud_processing.svg"),
                                          16,
                                          QStringLiteral("#00D492"));
    left_header_icon_layout->addWidget(lbl_left_header_icon_);
    left_header_icon_layout->addStretch(1);
    left_header_layout->addWidget(left_header_icon_box_, 0, Qt::AlignVCenter);

    auto* left_header_text = new QVBoxLayout();
    left_header_text->setContentsMargins(0, 0, 0, 0);
    left_header_text->setSpacing(0);
    lbl_left_header_title_ = makeTextLabel(
        left_header_,
        QStringLiteral("Point Cloud Processing"),
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; color: #FFFFFF;"));
    lbl_left_header_subtitle_ =
        makeTextLabel(left_header_, QStringLiteral("Optimize and clean scan data"), kInitialLabel10);
    left_header_text->addWidget(lbl_left_header_title_);
    left_header_text->addWidget(lbl_left_header_subtitle_);
    left_header_layout->addLayout(left_header_text, 1);
    stage_header_layout->addWidget(left_header_);

    stage_row_host_ = new QWidget(stage_header_);
    stage_row_host_->setAttribute(Qt::WA_StyledBackground, true);
    stage_row_host_->setStyleSheet(QStringLiteral("background: #18181B; border-top: 1px solid #27272A;"));
    auto* stage_row_host_layout = new QHBoxLayout(stage_row_host_);
    stage_row_host_layout->setContentsMargins(24, 0, 24, 0);
    stage_row_host_layout->setSpacing(0);
    stage_row_host_layout->addStretch(1);

    stage_row_frame_ = new QWidget(stage_row_host_);
    stage_row_frame_->setAttribute(Qt::WA_StyledBackground, true);
    stage_row_frame_->setFixedHeight(34);
    stage_row_frame_->setFixedWidth(kStageRowWidth);
    stage_row_frame_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    stage_row_frame_->setStyleSheet(QStringLiteral("background: #18181B;"));
    auto* stage_row_layout = new QHBoxLayout(stage_row_frame_);
    stage_row_layout->setContentsMargins(0, 0, 0, 0);
    stage_row_layout->setSpacing(4);

    step_map_processing_ = make_stage_step(
        stage_row_frame_,
        QStringLiteral(":/assets/missionplanner/stage_map_processing.svg"),
        QStringLiteral("Map Processing"),
        189,
        158,
        true);
    connect(step_map_processing_.button, &QPushButton::clicked, this, [this]() {
        navigateToStep(PlannerStep::MapProcessing);
    });
    stage_row_layout->addWidget(step_map_processing_.wrapper, 0, Qt::AlignVCenter);
    step_coverage_planning_ = make_stage_step(
        stage_row_frame_,
        QStringLiteral(":/assets/missionplanner/stage_coverage_planning.svg"),
        QStringLiteral("Coverage Planning"),
        208,
        176,
        true);
    connect(step_coverage_planning_.button, &QPushButton::clicked, this, [this]() {
        navigateToStep(PlannerStep::CoveragePlanning);
    });
    stage_row_layout->addWidget(step_coverage_planning_.wrapper, 0, Qt::AlignVCenter);
    step_scan_splitting_ = make_stage_step(
        stage_row_frame_,
        QStringLiteral(":/assets/missionplanner/stage_scan_splitting.svg"),
        QStringLiteral("Scan Splitting"),
        175,
        143,
        true);
    connect(step_scan_splitting_.button, &QPushButton::clicked, this, [this]() {
        const SessionCache* cache = activeSessionPtr();
        if (kBypassPlannerStageGates || (cache && cache->planning_complete)) {
            navigateToStep(PlannerStep::ScanSplitting);
        }
    });
    stage_row_layout->addWidget(step_scan_splitting_.wrapper, 0, Qt::AlignVCenter);
    step_scan_ = make_stage_step(stage_row_frame_,
                                 QStringLiteral(":/assets/missionplanner/stage_scan.svg"),
                                 QStringLiteral("Scan"),
                                 96,
                                 96,
                                 false);
    connect(step_scan_.button, &QPushButton::clicked, this, [this]() {
        const SessionCache* cache = activeSessionPtr();
        if (kBypassPlannerStageGates || (cache && cache->scan_waypoints_published)) {
            navigateToStep(PlannerStep::Scan);
        }
    });
    stage_row_layout->addWidget(step_scan_.wrapper, 0, Qt::AlignVCenter);

    stage_row_host_layout->addWidget(stage_row_frame_, 0, Qt::AlignCenter);
    stage_row_host_layout->addStretch(1);
    stage_header_layout->addWidget(stage_row_host_, 1);
    root_layout->addWidget(stage_header_);

    auto* body = new QWidget(this);
    auto* body_layout = new QHBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);
    root_layout->addWidget(body, 1);

    left_rail_ = new QWidget(body);
    left_rail_->setFixedWidth(kPlannerLeftRailWidth);
    left_rail_->setAttribute(Qt::WA_StyledBackground, true);
    left_rail_->setStyleSheet(QStringLiteral("background: #18181B; border-right: 1px solid #27272A;"));
    auto* left_rail_layout = new QVBoxLayout(left_rail_);
    left_rail_layout->setContentsMargins(0, 0, 0, 0);
    left_rail_layout->setSpacing(0);

    content_stack_ = new QStackedWidget(left_rail_);
    content_stack_->setContentsMargins(0, 0, 0, 0);
    content_stack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    content_stack_->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    left_rail_layout->addWidget(content_stack_, 1);

    map_processing_page_ = new QWidget(content_stack_);
    map_processing_page_->setAttribute(Qt::WA_StyledBackground, true);
    map_processing_page_->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* map_processing_page_layout = new QVBoxLayout(map_processing_page_);
    map_processing_page_layout->setContentsMargins(0, 0, 0, 0);
    map_processing_page_layout->setSpacing(0);

    auto* left_scroll = new QScrollArea(map_processing_page_);
    left_scroll->setFrameShape(QFrame::NoFrame);
    left_scroll->setWidgetResizable(true);
    left_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    left_scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));
    left_scroll->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));
    AutoHideScrollBar::install(left_scroll, dark_mode_);
    map_processing_page_layout->addWidget(left_scroll, 1);

    auto* left_content = new QWidget(left_scroll);
    left_content->setAttribute(Qt::WA_StyledBackground, true);
    left_content->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* left_content_layout = new QVBoxLayout(left_content);
    left_content_layout->setContentsMargins(24, 12, 24, 24);
    left_content_layout->setSpacing(0);

    auto make_stepper_button = [this](QWidget* parent, const QString& icon_path) {
        auto* btn = new QPushButton(parent);
        btn->setObjectName(QStringLiteral("PlannerStepper"));
        btn->setProperty("plannerStepper", true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFlat(true);
        btn->setFixedSize(28, 28);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setAutoRepeat(true);
        btn->setAutoRepeatDelay(400);
        btn->setAutoRepeatInterval(50);
        auto* icon_layout = new QHBoxLayout(btn);
        icon_layout->setContentsMargins(0, 0, 0, 0);
        icon_layout->setSpacing(0);
        auto* icon = makeIconLabel(btn, icon_path, 14,
                                   dark_mode_ ? QStringLiteral("#D4D4D8")
                                              : QStringLiteral("#3F3F47"));
        icon_layout->addWidget(icon, 0, Qt::AlignCenter);
        stepper_buttons_.push_back({btn, icon, icon_path});
        applyStepperButtonStyle(btn);
        return btn;
    };

    auto make_range_block = [&, this](QWidget* parent,
                                      const QString& label_text,
                                      const QString& desc_text,
                                      const QString& min_text,
                                      const QString& max_text,
                                      double minimum,
                                      double maximum,
                                      double step,
                                      int decimals,
                                      PlannerTrackSlider** out_slider,
                                      QLabel** out_value_label) -> QWidget* {
        auto* block = new QWidget(parent);
        block->setFixedHeight(100);
        auto* layout = new QVBoxLayout(block);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        layout->addWidget(makeTrackedLabel(block, desc_text, kInitialLabel10, &label10_labels_));

        auto* header_row = new QHBoxLayout();
        header_row->setContentsMargins(0, 0, 0, 0);
        header_row->setSpacing(12);
        header_row->addWidget(
            makeTrackedLabel(block, label_text, kInitialLabel12, &label12_labels_), 1);
        *out_value_label = makeTrackedLabel(block,
                                            QStringLiteral("--"),
                                            kInitialMono12White,
                                            &mono12_white_labels_,
                                            Qt::AlignRight | Qt::AlignVCenter);
        header_row->addWidget(*out_value_label);
        layout->addLayout(header_row);
        layout->addSpacing(4);

        auto* slider_row = new QHBoxLayout();
        slider_row->setContentsMargins(0, 0, 0, 0);
        slider_row->setSpacing(8);

        auto* btn_minus =
            make_stepper_button(block, QStringLiteral(":/assets/missionplanner/stepper_minus.svg"));
        slider_row->addWidget(btn_minus, 0, Qt::AlignVCenter);

        auto* slider = new PlannerTrackSlider(block);
        slider->setRange(minimum, maximum);
        slider->setStep(step);
        slider->setDecimals(decimals);
        slider_row->addWidget(slider, 1, Qt::AlignVCenter);
        *out_slider = slider;

        auto* btn_plus =
            make_stepper_button(block, QStringLiteral(":/assets/missionplanner/stepper_plus.svg"));
        slider_row->addWidget(btn_plus, 0, Qt::AlignVCenter);

        connect(btn_minus, &QPushButton::clicked, this, [slider]() {
            const double next = slider->value() - slider->step();
            const double clamped = std::max(slider->minimum(), next);
            if (std::abs(clamped - slider->value()) < 1e-9) {
                return;
            }
            slider->setValue(clamped);
            if (slider->on_value_changed) {
                slider->on_value_changed(slider->value());
            }
        });
        connect(btn_plus, &QPushButton::clicked, this, [slider]() {
            const double next = slider->value() + slider->step();
            const double clamped = std::min(slider->maximum(), next);
            if (std::abs(clamped - slider->value()) < 1e-9) {
                return;
            }
            slider->setValue(clamped);
            if (slider->on_value_changed) {
                slider->on_value_changed(slider->value());
            }
        });

        layout->addLayout(slider_row);

        auto* range_row = new QHBoxLayout();
        range_row->setContentsMargins(0, 0, 0, 0);
        range_row->setSpacing(0);
        range_row->addWidget(makeTrackedLabel(block, min_text, kInitialMono9Muted, &mono9_labels_));
        range_row->addStretch(1);
        range_row->addWidget(makeTrackedLabel(block,
                                              max_text,
                                              kInitialMono9Muted,
                                              &mono9_labels_,
                                              Qt::AlignRight | Qt::AlignVCenter));
        layout->addLayout(range_row);
        return block;
    };

    auto* downsampling_section = new QWidget(left_content);
    downsampling_section->setFixedHeight(124);
    auto* downsampling_layout = new QVBoxLayout(downsampling_section);
    downsampling_layout->setContentsMargins(0, 0, 0, 0);
    downsampling_layout->setSpacing(8);
    downsampling_layout->addWidget(makeTrackedLabel(
        downsampling_section, QStringLiteral("DOWNSAMPLING"), kInitialHeading10, &heading10_labels_));
    downsampling_layout->addWidget(make_range_block(downsampling_section,
                                                    QStringLiteral("Voxel Size"),
                                                    QStringLiteral("Grid resolution for downsampling"),
                                                    QStringLiteral("0.01m"),
                                                    QStringLiteral("0.20m"),
                                                    0.01,
                                                    0.20,
                                                    0.01,
                                                    2,
                                                    &slider_voxel_,
                                                    &lbl_voxel_value_));
    left_content_layout->addWidget(downsampling_section);
    left_content_layout->addSpacing(12);

    auto* height_section = new QWidget(left_content);
    height_section->setFixedHeight(238);
    auto* height_layout = new QVBoxLayout(height_section);
    height_layout->setContentsMargins(0, 4, 0, 0);
    height_layout->setSpacing(8);
    height_layout->addWidget(makeTrackedLabel(height_section,
                                              QStringLiteral("HEIGHT FILTRATION"),
                                              kInitialHeading10,
                                              &heading10_labels_));

    auto* height_blocks = new QWidget(height_section);
    auto* height_blocks_layout = new QVBoxLayout(height_blocks);
    height_blocks_layout->setContentsMargins(0, 0, 0, 0);
    height_blocks_layout->setSpacing(10);
    height_blocks_layout->addWidget(make_range_block(height_section,
                                                     QStringLiteral("Z Min"),
                                                     QStringLiteral("Minimum height threshold"),
                                                     QStringLiteral("-0.50m"),
                                                     QStringLiteral("0.50m"),
                                                     -0.50,
                                                     0.50,
                                                     0.01,
                                                     2,
                                                     &slider_z_min_,
                                                     &lbl_z_min_value_));
    height_blocks_layout->addWidget(make_range_block(height_section,
                                                     QStringLiteral("Z Max"),
                                                     QStringLiteral("Maximum height threshold"),
                                                     QStringLiteral("-0.50m"),
                                                     QStringLiteral("0.50m"),
                                                     -0.50,
                                                     0.50,
                                                     0.01,
                                                     2,
                                                     &slider_z_max_,
                                                     &lbl_z_max_value_));
    height_layout->addWidget(height_blocks);
    left_content_layout->addWidget(height_section);

    left_content_layout->addSpacing(20);

    btn_process_ = new QPushButton(left_content);
    btn_process_->setCursor(Qt::PointingHandCursor);
    btn_process_->setFlat(true);
    btn_process_->setFixedHeight(40);
    btn_process_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn_process_->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: #00BC7D;"
        "  border: none;"
        "  border-radius: 14px;"
        "}"
        "QPushButton:hover { background: #0ACB8B; }"
        "QPushButton:disabled { background: #1F2937; }"));
    auto* btn_process_layout = new QHBoxLayout(btn_process_);
    btn_process_layout->setContentsMargins(0, 0, 0, 0);
    btn_process_layout->setSpacing(8);
    btn_process_layout->addStretch(1);
    lbl_process_icon_ = makeIconLabel(btn_process_,
                                      QStringLiteral(":/assets/missionplanner/process_point_cloud.svg"),
                                      16,
                                      QStringLiteral("#FFFFFF"));
    btn_process_layout->addWidget(lbl_process_icon_);
    lbl_process_text_ = makeTextLabel(
        btn_process_,
        QStringLiteral("Process Point Cloud"),
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 700; color: #FFFFFF;"));
    btn_process_layout->addWidget(lbl_process_text_);
    btn_process_layout->addStretch(1);
    applyDropShadow(btn_process_, 18, 4, QColor(0, 188, 125, 64));
    connect(btn_process_, &QPushButton::clicked, this, [this]() { startProcessPointCloud(); });
    left_content_layout->addWidget(btn_process_);
    left_content_layout->addSpacing(12);

    auto* hull_section = new QWidget(left_content);
    hull_section->setFixedHeight(174);
    auto* hull_layout = new QVBoxLayout(hull_section);
    hull_layout->setContentsMargins(0, 4, 0, 0);
    hull_layout->setSpacing(8);
    hull_layout->addWidget(makeTrackedLabel(
        hull_section, QStringLiteral("BOUNDARY HULL"), kInitialHeading10, &heading10_labels_));

    auto* hull_content = new QWidget(hull_section);
    auto* hull_content_layout = new QVBoxLayout(hull_content);
    hull_content_layout->setContentsMargins(0, 0, 0, 0);
    hull_content_layout->setSpacing(8);
    hull_content_layout->addWidget(make_range_block(hull_section,
                                                    QStringLiteral("Parameter"),
                                                    QStringLiteral("Alpha value for hull computation"),
                                                    QStringLiteral("0.10"),
                                                    QStringLiteral("3.00"),
                                                    0.10,
                                                    3.00,
                                                    0.01,
                                                    2,
                                                    &slider_alpha_,
                                                    &lbl_alpha_value_));

    btn_hull_ = new QPushButton(hull_content);
    btn_hull_->setCursor(Qt::PointingHandCursor);
    btn_hull_->setFlat(true);
    btn_hull_->setFixedHeight(38);
    btn_hull_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn_hull_->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: #27272A;"
        "  border: 1px solid #3F3F47;"
        "  border-radius: 10px;"
        "}"
        "QPushButton:hover { border-color: #52525C; }"
        "QPushButton:disabled { background: #1F1F23; border-color: #27272A; }"));
    auto* btn_hull_layout = new QHBoxLayout(btn_hull_);
    btn_hull_layout->setContentsMargins(0, 0, 0, 0);
    btn_hull_layout->setSpacing(10);
    btn_hull_layout->addStretch(1);
    lbl_hull_icon_ = makeIconLabel(btn_hull_,
                                   QStringLiteral(":/assets/missionplanner/compute_hull.svg"),
                                   14,
                                   QStringLiteral("#E4E4E7"));
    btn_hull_layout->addWidget(lbl_hull_icon_);
    lbl_hull_text_ = makeTextLabel(
        btn_hull_,
        QStringLiteral("Compute and Project Hull"),
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: #E4E4E7;"));
    btn_hull_layout->addWidget(lbl_hull_text_);
    btn_hull_layout->addStretch(1);
    connect(btn_hull_, &QPushButton::clicked, this, [this]() { startComputeHull(); });
    hull_content_layout->addWidget(btn_hull_);
    hull_layout->addWidget(hull_content);
    left_content_layout->addWidget(hull_section);
    left_content_layout->addSpacing(12);

    lbl_inline_status_ = makeTextLabel(left_content,
                                       QString(),
                                       QStringLiteral("font-family: 'Arimo'; font-size: 14px; "
                                                      "font-weight: 500; color: #71717B;"));
    lbl_inline_status_->setWordWrap(true);
    lbl_inline_status_->setVisible(false);
    left_content_layout->addWidget(lbl_inline_status_);

    output_section_ = new QWidget(left_content);
    output_section_->setFixedHeight(146);
    output_section_->setAttribute(Qt::WA_StyledBackground, true);
    output_section_->setStyleSheet(
        QStringLiteral("background: transparent; border-top: 1px solid #27272A;"));
    auto* output_layout = new QVBoxLayout(output_section_);
    output_layout->setContentsMargins(0, 9, 0, 0);
    output_layout->setSpacing(8);
    lbl_output_heading_ = makeTrackedLabel(output_section_,
                                           QStringLiteral("Estimated Output"),
                                           kInitialHeading10,
                                           &heading10_labels_);
    output_layout->addWidget(lbl_output_heading_);

    auto* output_grid = new QGridLayout();
    output_grid->setContentsMargins(0, 0, 0, 0);
    output_grid->setHorizontalSpacing(8);
    output_grid->setVerticalSpacing(8);
    output_grid->addWidget(make_output_card(output_section_,
                                            QStringLiteral("Points Retained"),
                                            &lbl_output_points_),
                           0,
                           0);
    output_grid->addWidget(make_output_card(output_section_,
                                            QStringLiteral("Reduction"),
                                            &lbl_output_reduction_),
                           0,
                           1);
    output_grid->addWidget(make_output_card(output_section_,
                                            QStringLiteral("Est. File Size"),
                                            &lbl_output_file_size_),
                           1,
                           0);
    output_grid->addWidget(make_output_card(output_section_,
                                            QStringLiteral("Quality"),
                                            &lbl_output_quality_),
                           1,
                           1);
    output_layout->addLayout(output_grid);
    left_content_layout->addWidget(output_section_);
    left_content_layout->addStretch(1);

    left_scroll->setWidget(left_content);
    body_layout->addWidget(left_rail_);
    content_stack_->addWidget(map_processing_page_);

    auto make_choice_button = [&](QWidget* parent,
                                  const QString& title,
                                  const QString& subtitle,
                                  QLabel** out_icon,
                                  int height) -> QPushButton* {
        auto* button = new QPushButton(parent);
        button->setCursor(Qt::PointingHandCursor);
        button->setFlat(true);
        button->setFixedHeight(height);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* layout = new QVBoxLayout(button);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(4);
        layout->setAlignment(Qt::AlignCenter);
        if (out_icon) {
            auto* icon_label = new QLabel(button);
            icon_label->setFixedSize(32, 24);
            icon_label->setAlignment(Qt::AlignCenter);
            icon_label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            icon_label->setStyleSheet(QStringLiteral("background: transparent;"));
            *out_icon = icon_label;
            layout->addWidget(icon_label, 0, Qt::AlignHCenter);
        }
        auto* title_label = makeTextLabel(
            button,
            title,
            QStringLiteral("font-family: 'Arimo'; font-size: 13px; font-weight: 700; color: #E4E4E7;"),
            Qt::AlignCenter);
        title_label->setObjectName(QStringLiteral("coverageTitle"));
        layout->addWidget(title_label, 0, Qt::AlignHCenter);
        if (!subtitle.isEmpty()) {
            auto* subtitle_label = makeTextLabel(
                button,
                subtitle,
                QStringLiteral("font-family: 'Arimo'; font-size: 11px; font-weight: 400; color: #71717B;"),
                Qt::AlignCenter);
            subtitle_label->setObjectName(QStringLiteral("coverageSubtitle"));
            layout->addWidget(subtitle_label, 0, Qt::AlignHCenter);
        }
        return button;
    };
    auto make_row_choice_button =
        [&](QWidget* parent, const QString& title, QLabel** out_icon, int height) -> QPushButton* {
            auto* button = new QPushButton(parent);
            button->setCursor(Qt::PointingHandCursor);
            button->setFlat(true);
            button->setFixedHeight(height);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            auto* layout = new QHBoxLayout(button);
            layout->setContentsMargins(12, 0, 12, 0);
            layout->setSpacing(8);
            layout->addStretch(1);
            if (out_icon) {
                auto* icon_label = new QLabel(button);
                icon_label->setFixedSize(16, 16);
                icon_label->setAlignment(Qt::AlignCenter);
                icon_label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
                icon_label->setStyleSheet(QStringLiteral("background: transparent;"));
                *out_icon = icon_label;
                layout->addWidget(icon_label, 0, Qt::AlignVCenter);
            }
            auto* title_label = makeTextLabel(
                button,
                title,
                QStringLiteral(
                    "font-family: 'Arimo'; font-size: 13px; font-weight: 700; color: #E4E4E7;"),
                Qt::AlignCenter);
            title_label->setObjectName(QStringLiteral("coverageTitle"));
            layout->addWidget(title_label, 0, Qt::AlignVCenter);
            layout->addStretch(1);
            return button;
        };

    coverage_placeholder_page_ = new QWidget(content_stack_);
    coverage_placeholder_page_->setAttribute(Qt::WA_StyledBackground, true);
    coverage_placeholder_page_->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* coverage_page_layout = new QVBoxLayout(coverage_placeholder_page_);
    coverage_page_layout->setContentsMargins(0, 0, 0, 0);
    coverage_page_layout->setSpacing(0);

    auto* coverage_scroll = new QScrollArea(coverage_placeholder_page_);
    coverage_scroll->setFrameShape(QFrame::NoFrame);
    coverage_scroll->setWidgetResizable(true);
    coverage_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    coverage_scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));
    coverage_scroll->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));
    AutoHideScrollBar::install(coverage_scroll, dark_mode_);
    coverage_page_layout->addWidget(coverage_scroll, 1);

    auto* coverage_content = new QWidget(coverage_scroll);
    coverage_content->setAttribute(Qt::WA_StyledBackground, true);
    coverage_content->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* coverage_content_layout = new QVBoxLayout(coverage_content);
    coverage_content_layout->setContentsMargins(24, 12, 24, 24);
    coverage_content_layout->setSpacing(0);

    auto* scan_mode_section = new QWidget(coverage_content);
    auto* scan_mode_layout = new QVBoxLayout(scan_mode_section);
    scan_mode_layout->setContentsMargins(0, 0, 0, 0);
    scan_mode_layout->setSpacing(8);
    scan_mode_layout->addWidget(
        makeTrackedLabel(scan_mode_section, QStringLiteral("SCAN MODE"), kInitialHeading10, &heading10_labels_));
    auto* scan_mode_grid = new QGridLayout();
    scan_mode_grid->setContentsMargins(0, 0, 0, 0);
    scan_mode_grid->setHorizontalSpacing(8);
    btn_coverage_scan_complete_ =
        make_row_choice_button(scan_mode_section, QStringLiteral("Complete"), &lbl_coverage_scan_complete_icon_, 42);
    btn_coverage_scan_roi_ =
        make_row_choice_button(scan_mode_section, QStringLiteral("ROI Scan"), &lbl_coverage_scan_roi_icon_, 42);
    scan_mode_grid->addWidget(btn_coverage_scan_complete_, 0, 0);
    scan_mode_grid->addWidget(btn_coverage_scan_roi_, 0, 1);
    scan_mode_layout->addLayout(scan_mode_grid);
    coverage_content_layout->addWidget(scan_mode_section);
    coverage_content_layout->addSpacing(12);

    coverage_roi_section_ = new QWidget(coverage_content);
    coverage_roi_section_->setAttribute(Qt::WA_StyledBackground, true);
    coverage_roi_section_->setStyleSheet(
        QStringLiteral("background: transparent; border-top: 1px solid #27272A;"));
    auto* roi_section_layout = new QVBoxLayout(coverage_roi_section_);
    roi_section_layout->setContentsMargins(0, 10, 0, 0);
    roi_section_layout->setSpacing(8);
    auto* roi_header = new QWidget(coverage_roi_section_);
    auto* roi_header_layout = new QHBoxLayout(roi_header);
    roi_header_layout->setContentsMargins(0, 0, 0, 0);
    roi_header_layout->setSpacing(8);
    roi_header_layout->addWidget(
        makeTrackedLabel(roi_header, QStringLiteral("ROI SELECTION"), kInitialHeading10, &heading10_labels_));
    roi_header_layout->addStretch(1);
    btn_coverage_roi_clear_ = new QPushButton(QStringLiteral("Clear ROI"), roi_header);
    btn_coverage_roi_clear_->setCursor(Qt::PointingHandCursor);
    btn_coverage_roi_clear_->setFlat(true);
    roi_header_layout->addWidget(btn_coverage_roi_clear_, 0, Qt::AlignVCenter);
    roi_section_layout->addWidget(roi_header);
    roi_section_layout->addWidget(makeTextLabel(
        coverage_roi_section_,
        QStringLiteral("Drawing Tool"),
        QStringLiteral("font-family: 'Arimo'; font-size: 11px; font-weight: 400; color: #71717B;")));
    auto* roi_tool_grid = new QGridLayout();
    roi_tool_grid->setContentsMargins(0, 0, 0, 0);
    roi_tool_grid->setHorizontalSpacing(8);
    btn_coverage_roi_draw_rectangle_ = make_row_choice_button(
        coverage_roi_section_, QStringLiteral("Rectangle"), &lbl_coverage_roi_rectangle_icon_, 40);
    btn_coverage_roi_draw_polygon_ = make_row_choice_button(
        coverage_roi_section_, QStringLiteral("Polygon"), &lbl_coverage_roi_polygon_icon_, 40);
    roi_tool_grid->addWidget(btn_coverage_roi_draw_rectangle_, 0, 0);
    roi_tool_grid->addWidget(btn_coverage_roi_draw_polygon_, 0, 1);
    roi_section_layout->addLayout(roi_tool_grid);
    btn_coverage_roi_start_ = new QPushButton(coverage_roi_section_);
    btn_coverage_roi_start_->setCursor(Qt::PointingHandCursor);
    btn_coverage_roi_start_->setFlat(true);
    btn_coverage_roi_start_->setFixedHeight(40);
    roi_section_layout->addWidget(btn_coverage_roi_start_);
    coverage_roi_status_card_ = new QWidget(coverage_roi_section_);
    coverage_roi_status_card_->setAttribute(Qt::WA_StyledBackground, true);
    auto* roi_status_layout = new QHBoxLayout(coverage_roi_status_card_);
    roi_status_layout->setContentsMargins(12, 10, 12, 10);
    roi_status_layout->setSpacing(8);
    lbl_coverage_roi_status_icon_ = new QLabel(coverage_roi_status_card_);
    lbl_coverage_roi_status_icon_->setFixedSize(16, 16);
    lbl_coverage_roi_status_icon_->setAlignment(Qt::AlignCenter);
    lbl_coverage_roi_status_icon_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    lbl_coverage_roi_status_icon_->setStyleSheet(QStringLiteral("background: transparent;"));
    roi_status_layout->addWidget(lbl_coverage_roi_status_icon_, 0, Qt::AlignTop);
    lbl_coverage_roi_status_text_ = makeTextLabel(
        coverage_roi_status_card_,
        QString(),
        QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: #2563EB;"));
    lbl_coverage_roi_status_text_->setWordWrap(true);
    lbl_coverage_roi_status_text_->setObjectName(QStringLiteral("coverageRoiStatusText"));
    roi_status_layout->addWidget(lbl_coverage_roi_status_text_, 1);
    roi_section_layout->addWidget(coverage_roi_status_card_);
    coverage_content_layout->addWidget(coverage_roi_section_);
    coverage_content_layout->addSpacing(12);

    auto* preset_section = new QWidget(coverage_content);
    auto* preset_layout = new QVBoxLayout(preset_section);
    preset_layout->setContentsMargins(0, 0, 0, 0);
    preset_layout->setSpacing(8);
    preset_layout->addWidget(makeTrackedLabel(
        preset_section, QStringLiteral("SCAN CONFIGURATION PRESET"), kInitialHeading10, &heading10_labels_));
    auto* preset_row = new QWidget(preset_section);
    auto* preset_row_layout = new QHBoxLayout(preset_row);
    preset_row_layout->setContentsMargins(0, 0, 0, 0);
    preset_row_layout->setSpacing(8);
    combo_coverage_presets_ = new QComboBox(preset_row);
    combo_coverage_presets_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    combo_coverage_presets_->setFixedHeight(40);
    preset_row_layout->addWidget(combo_coverage_presets_, 1);
    btn_coverage_preset_add_ = new QPushButton(QStringLiteral("+"), preset_row);
    btn_coverage_preset_add_->setCursor(Qt::PointingHandCursor);
    btn_coverage_preset_add_->setFlat(true);
    btn_coverage_preset_add_->setFixedSize(40, 40);
    preset_row_layout->addWidget(btn_coverage_preset_add_);
    preset_layout->addWidget(preset_row);
    coverage_content_layout->addWidget(preset_section);
    coverage_content_layout->addSpacing(8);

    coverage_save_preset_card_ = new QWidget(coverage_content);
    coverage_save_preset_card_->setAttribute(Qt::WA_StyledBackground, true);
    auto* save_preset_layout = new QVBoxLayout(coverage_save_preset_card_);
    save_preset_layout->setContentsMargins(12, 12, 12, 12);
    save_preset_layout->setSpacing(8);
    edit_coverage_preset_name_ = new QLineEdit(coverage_save_preset_card_);
    edit_coverage_preset_name_->setPlaceholderText(QStringLiteral("Preset name..."));
    edit_coverage_preset_name_->setFixedHeight(34);
    save_preset_layout->addWidget(edit_coverage_preset_name_);
    auto* save_preset_actions = new QHBoxLayout();
    save_preset_actions->setContentsMargins(0, 0, 0, 0);
    save_preset_actions->setSpacing(6);
    btn_coverage_preset_save_ = new QPushButton(QStringLiteral("Save Preset"), coverage_save_preset_card_);
    btn_coverage_preset_save_->setCursor(Qt::PointingHandCursor);
    btn_coverage_preset_save_->setFlat(true);
    btn_coverage_preset_save_->setFixedHeight(32);
    save_preset_actions->addWidget(btn_coverage_preset_save_, 1);
    btn_coverage_preset_cancel_ = new QPushButton(QStringLiteral("Cancel"), coverage_save_preset_card_);
    btn_coverage_preset_cancel_->setCursor(Qt::PointingHandCursor);
    btn_coverage_preset_cancel_->setFlat(true);
    btn_coverage_preset_cancel_->setFixedHeight(32);
    save_preset_actions->addWidget(btn_coverage_preset_cancel_);
    save_preset_layout->addLayout(save_preset_actions);
    coverage_content_layout->addWidget(coverage_save_preset_card_);
    coverage_content_layout->addSpacing(8);

    coverage_custom_presets_card_ = new QWidget(coverage_content);
    coverage_custom_presets_card_->setAttribute(Qt::WA_StyledBackground, true);
    auto* custom_presets_layout = new QVBoxLayout(coverage_custom_presets_card_);
    custom_presets_layout->setContentsMargins(12, 10, 12, 10);
    custom_presets_layout->setSpacing(8);
    custom_presets_layout->addWidget(makeTextLabel(
        coverage_custom_presets_card_,
        QStringLiteral("Custom Presets"),
        QStringLiteral("font-family: 'Arimo'; font-size: 11px; font-weight: 400; color: #71717B;")));
    auto* custom_presets_body = new QWidget(coverage_custom_presets_card_);
    coverage_custom_presets_layout_ = new QVBoxLayout(custom_presets_body);
    coverage_custom_presets_layout_->setContentsMargins(0, 0, 0, 0);
    coverage_custom_presets_layout_->setSpacing(6);
    custom_presets_layout->addWidget(custom_presets_body);
    coverage_content_layout->addWidget(coverage_custom_presets_card_);
    coverage_content_layout->addSpacing(12);

    auto* pattern_section = new QWidget(coverage_content);
    auto* pattern_layout = new QVBoxLayout(pattern_section);
    pattern_layout->setContentsMargins(0, 0, 0, 0);
    pattern_layout->setSpacing(8);
    pattern_layout->addWidget(makeTrackedLabel(
        pattern_section, QStringLiteral("COVERAGE PATTERN"), kInitialHeading10, &heading10_labels_));
    auto* pattern_grid = new QGridLayout();
    pattern_grid->setContentsMargins(0, 0, 0, 0);
    pattern_grid->setHorizontalSpacing(8);
    pattern_grid->setVerticalSpacing(8);
    btn_coverage_pattern_boustro_ =
        make_choice_button(pattern_section,
                           QStringLiteral("Zigzag"),
                           QString(),
                           &lbl_coverage_pattern_boustro_icon_,
                           84);
    btn_coverage_pattern_snake_ =
        make_choice_button(pattern_section,
                           QStringLiteral("Snake"),
                           QString(),
                           &lbl_coverage_pattern_snake_icon_,
                           84);
    btn_coverage_pattern_spiral_ =
        make_choice_button(pattern_section,
                           QStringLiteral("Spiral"),
                           QString(),
                           &lbl_coverage_pattern_spiral_icon_,
                           84);
    pattern_grid->addWidget(btn_coverage_pattern_boustro_, 0, 0);
    pattern_grid->addWidget(btn_coverage_pattern_snake_, 0, 1);
    pattern_grid->addWidget(btn_coverage_pattern_spiral_, 0, 2);
    pattern_layout->addLayout(pattern_grid);
    coverage_content_layout->addWidget(pattern_section);
    coverage_content_layout->addSpacing(12);

    auto* axis_section = new QWidget(coverage_content);
    auto* axis_layout = new QVBoxLayout(axis_section);
    axis_layout->setContentsMargins(0, 0, 0, 0);
    axis_layout->setSpacing(8);
    axis_layout->addWidget(makeTrackedLabel(
        axis_section, QStringLiteral("SCAN AXIS"), kInitialHeading10, &heading10_labels_));
    auto* axis_grid = new QGridLayout();
    axis_grid->setContentsMargins(0, 0, 0, 0);
    axis_grid->setHorizontalSpacing(8);
    btn_coverage_axis_parallel_ =
        make_choice_button(axis_section, QStringLiteral("Parallel"), QStringLiteral("Long edge"), nullptr, 64);
    btn_coverage_axis_perpendicular_ = make_choice_button(axis_section,
                                                          QStringLiteral("Perpendicular"),
                                                          QStringLiteral("Short edge"),
                                                          nullptr,
                                                          64);
    axis_grid->addWidget(btn_coverage_axis_parallel_, 0, 0);
    axis_grid->addWidget(btn_coverage_axis_perpendicular_, 0, 1);
    axis_layout->addLayout(axis_grid);
    coverage_content_layout->addWidget(axis_section);
    coverage_content_layout->addSpacing(12);

    auto* path_section = new QWidget(coverage_content);
    auto* path_section_layout = new QVBoxLayout(path_section);
    path_section_layout->setContentsMargins(0, 0, 0, 0);
    path_section_layout->setSpacing(8);
    path_section_layout->addWidget(makeTrackedLabel(
        path_section, QStringLiteral("PATH PARAMETERS"), kInitialHeading10, &heading10_labels_));
    auto* path_blocks = new QWidget(path_section);
    auto* path_blocks_layout = new QVBoxLayout(path_blocks);
    path_blocks_layout->setContentsMargins(0, 0, 0, 0);
    path_blocks_layout->setSpacing(10);
    path_blocks_layout->addWidget(make_range_block(path_section,
                                                   QStringLiteral("Path Spacing"),
                                                   QStringLiteral("Distance between paths"),
                                                   QStringLiteral("0.20m"),
                                                   QStringLiteral("1.00m"),
                                                   kCoveragePathSpacingMin,
                                                   kCoveragePathSpacingMax,
                                                   0.05,
                                                   2,
                                                   &slider_coverage_path_spacing_,
                                                   &lbl_coverage_path_spacing_value_));
    path_blocks_layout->addWidget(make_range_block(path_section,
                                                   QStringLiteral("Headland Width"),
                                                   QStringLiteral("Border clearance width"),
                                                   QStringLiteral("0.10m"),
                                                   QStringLiteral("1.00m"),
                                                   kCoverageHeadlandMin,
                                                   kCoverageHeadlandMax,
                                                   0.05,
                                                   2,
                                                   &slider_coverage_headland_,
                                                   &lbl_coverage_headland_value_));
    // Robot cruise speed (m/s). Slider is config-only; the value gets pushed
    // to /mpc_accel_controller's `max_linear_velocity` ROS param at scan-
    // start. Range matches the safe envelope of the current MPC tune.
    path_blocks_layout->addWidget(make_range_block(path_section,
                                                   QStringLiteral("Robot Speed"),
                                                   QStringLiteral("Cruise speed during scan"),
                                                   QStringLiteral("0.30 m/s"),
                                                   QStringLiteral("0.60 m/s"),
                                                   kCoverageScanSpeedMin,
                                                   kCoverageScanSpeedMax,
                                                   kCoverageScanSpeedStep,
                                                   2,
                                                   &slider_coverage_scan_speed_,
                                                   &lbl_coverage_scan_speed_value_));
    path_section_layout->addWidget(path_blocks);
    coverage_content_layout->addWidget(path_section);
    coverage_content_layout->addSpacing(12);

    auto* obstacle_section = new QWidget(coverage_content);
    obstacle_section->setAttribute(Qt::WA_StyledBackground, true);
    obstacle_section->setStyleSheet(QStringLiteral("background: transparent; border-top: 1px solid #27272A;"));
    auto* obstacle_layout = new QVBoxLayout(obstacle_section);
    obstacle_layout->setContentsMargins(0, 10, 0, 0);
    obstacle_layout->setSpacing(8);
    auto* obstacle_header = new QWidget(obstacle_section);
    auto* obstacle_header_layout = new QHBoxLayout(obstacle_header);
    obstacle_header_layout->setContentsMargins(0, 0, 0, 0);
    obstacle_header_layout->setSpacing(8);
    obstacle_header_layout->addWidget(makeTrackedLabel(
        obstacle_header, QStringLiteral("OBSTACLE DETECTION"), kInitialHeading10, &heading10_labels_));
    obstacle_header_layout->addStretch(1);
    btn_coverage_clear_obstacles_ = new QPushButton(QStringLiteral("Clear All"), obstacle_header);
    btn_coverage_clear_obstacles_->setCursor(Qt::PointingHandCursor);
    btn_coverage_clear_obstacles_->setFlat(true);
    obstacle_header_layout->addWidget(btn_coverage_clear_obstacles_, 0, Qt::AlignVCenter);
    obstacle_layout->addWidget(obstacle_header);

    auto* obstacle_mode_grid = new QGridLayout();
    obstacle_mode_grid->setContentsMargins(0, 0, 0, 0);
    obstacle_mode_grid->setHorizontalSpacing(8);
    btn_coverage_obstacle_auto_ =
        make_row_choice_button(
            obstacle_section, QStringLiteral("Auto Detect"), &lbl_coverage_obstacle_auto_icon_, 42);
    btn_coverage_obstacle_manual_ =
        make_row_choice_button(
            obstacle_section, QStringLiteral("Manual Draw"), &lbl_coverage_obstacle_manual_icon_, 42);
    obstacle_mode_grid->addWidget(btn_coverage_obstacle_auto_, 0, 0);
    obstacle_mode_grid->addWidget(btn_coverage_obstacle_manual_, 0, 1);
    obstacle_layout->addLayout(obstacle_mode_grid);

    coverage_obstacle_mode_stack_ = new QStackedWidget(obstacle_section);
    coverage_obstacle_mode_stack_->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    auto* auto_detect_panel = new QWidget(coverage_obstacle_mode_stack_);
    auto* auto_detect_layout = new QVBoxLayout(auto_detect_panel);
    auto_detect_layout->setContentsMargins(0, 0, 0, 0);
    auto_detect_layout->setSpacing(8);
    coverage_auto_info_card_ = new QWidget(auto_detect_panel);
    coverage_auto_info_card_->setAttribute(Qt::WA_StyledBackground, true);
    auto* auto_info_layout = new QHBoxLayout(coverage_auto_info_card_);
    auto_info_layout->setContentsMargins(12, 12, 12, 12);
    auto_info_layout->setSpacing(8);
    lbl_coverage_auto_info_icon_ = new QLabel(coverage_auto_info_card_);
    lbl_coverage_auto_info_icon_->setFixedSize(16, 16);
    lbl_coverage_auto_info_icon_->setAlignment(Qt::AlignCenter);
    lbl_coverage_auto_info_icon_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    lbl_coverage_auto_info_icon_->setStyleSheet(QStringLiteral("background: transparent;"));
    auto_info_layout->addWidget(lbl_coverage_auto_info_icon_, 0, Qt::AlignTop);
    auto* auto_info_text_col = new QVBoxLayout();
    auto_info_text_col->setContentsMargins(0, 0, 0, 0);
    auto_info_text_col->setSpacing(4);
    auto* auto_info_title = makeTextLabel(
        coverage_auto_info_card_,
        QStringLiteral("Algorithm-Based Auto Detection"),
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 700; color: #E4E4E7;"));
    auto_info_title->setObjectName(QStringLiteral("coverageAutoInfoTitle"));
    auto_info_text_col->addWidget(auto_info_title);
    auto* auto_info_body = makeTextLabel(
        coverage_auto_info_card_,
        QStringLiteral("Automatically identifies obstacles from point cloud data using detection "
                       "algorithms. No manual configuration required."),
        QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: #71717B;"));
    auto_info_body->setObjectName(QStringLiteral("coverageAutoInfoBody"));
    auto_info_body->setWordWrap(true);
    auto_info_body->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    auto_info_text_col->addWidget(auto_info_body);
    auto_info_layout->addLayout(auto_info_text_col, 1);
    auto_detect_layout->addWidget(coverage_auto_info_card_);
    btn_coverage_detect_ = new QPushButton(auto_detect_panel);
    btn_coverage_detect_->setCursor(Qt::PointingHandCursor);
    btn_coverage_detect_->setFlat(true);
    btn_coverage_detect_->setFixedHeight(40);
    auto_detect_layout->addWidget(btn_coverage_detect_);
    coverage_obstacle_mode_stack_->addWidget(auto_detect_panel);

    auto* manual_panel = new QWidget(coverage_obstacle_mode_stack_);
    auto* manual_layout = new QVBoxLayout(manual_panel);
    manual_layout->setContentsMargins(0, 0, 0, 0);
    manual_layout->setSpacing(8);
    manual_layout->addWidget(makeTextLabel(
        manual_panel,
        QStringLiteral("Drawing Tool"),
        QStringLiteral("font-family: 'Arimo'; font-size: 11px; font-weight: 400; color: #71717B;")));
    auto* draw_tool_grid = new QGridLayout();
    draw_tool_grid->setContentsMargins(0, 0, 0, 0);
    draw_tool_grid->setHorizontalSpacing(6);
    btn_coverage_draw_rectangle_ =
        make_choice_button(manual_panel,
                           QStringLiteral("Rectangle"),
                           QString(),
                           &lbl_coverage_draw_rectangle_icon_,
                           62);
    btn_coverage_draw_polygon_ =
        make_choice_button(manual_panel,
                           QStringLiteral("Polygon"),
                           QString(),
                           &lbl_coverage_draw_polygon_icon_,
                           62);
    btn_coverage_draw_circle_ =
        make_choice_button(
            manual_panel, QStringLiteral("Circle"), QString(), &lbl_coverage_draw_circle_icon_, 62);
    draw_tool_grid->addWidget(btn_coverage_draw_rectangle_, 0, 0);
    draw_tool_grid->addWidget(btn_coverage_draw_polygon_, 0, 1);
    draw_tool_grid->addWidget(btn_coverage_draw_circle_, 0, 2);
    manual_layout->addLayout(draw_tool_grid);
    btn_coverage_draw_toggle_ = new QPushButton(manual_panel);
    btn_coverage_draw_toggle_->setCursor(Qt::PointingHandCursor);
    btn_coverage_draw_toggle_->setFlat(true);
    btn_coverage_draw_toggle_->setFixedHeight(38);
    manual_layout->addWidget(btn_coverage_draw_toggle_);
    coverage_manual_hint_card_ = new QWidget(manual_panel);
    coverage_manual_hint_card_->setAttribute(Qt::WA_StyledBackground, true);
    auto* manual_hint_layout = new QVBoxLayout(coverage_manual_hint_card_);
    manual_hint_layout->setContentsMargins(12, 10, 12, 10);
    manual_hint_layout->setSpacing(0);
    auto* manual_hint_label = makeTextLabel(
        coverage_manual_hint_card_,
        QStringLiteral("Click and drag on the map to draw the selected obstacle shape. Press ESC to cancel."),
        QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: #D97706;"));
    manual_hint_label->setObjectName(QStringLiteral("coverageManualHint"));
    manual_hint_label->setWordWrap(true);
    manual_hint_label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    manual_hint_layout->addWidget(manual_hint_label);
    manual_layout->addWidget(coverage_manual_hint_card_);
    coverage_obstacle_mode_stack_->addWidget(manual_panel);
    obstacle_layout->addWidget(coverage_obstacle_mode_stack_);
    coverage_content_layout->addWidget(obstacle_section);
    coverage_content_layout->addSpacing(12);

    btn_coverage_generate_ = new QPushButton(coverage_content);
    btn_coverage_generate_->setCursor(Qt::PointingHandCursor);
    btn_coverage_generate_->setFlat(true);
    btn_coverage_generate_->setFixedHeight(40);
    auto* coverage_generate_layout = new QHBoxLayout(btn_coverage_generate_);
    coverage_generate_layout->setContentsMargins(0, 0, 0, 0);
    coverage_generate_layout->setSpacing(8);
    coverage_generate_layout->addStretch(1);
    lbl_coverage_generate_icon_ = makeIconLabel(btn_coverage_generate_,
                                                QStringLiteral(":/assets/missionplanner/stage_coverage_planning.svg"),
                                                16,
                                                QStringLiteral("#FFFFFF"));
    coverage_generate_layout->addWidget(lbl_coverage_generate_icon_);
    lbl_coverage_generate_text_ = makeTextLabel(
        btn_coverage_generate_,
        QStringLiteral("Generate Coverage Paths"),
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 700; color: #FFFFFF;"));
    coverage_generate_layout->addWidget(lbl_coverage_generate_text_);
    coverage_generate_layout->addStretch(1);
    applyDropShadow(btn_coverage_generate_, 16, 4, QColor(0, 188, 125, 52));
    coverage_content_layout->addWidget(btn_coverage_generate_);
    coverage_content_layout->addSpacing(12);

    auto* coverage_stats_section = new QWidget(coverage_content);
    coverage_stats_section->setAttribute(Qt::WA_StyledBackground, true);
    coverage_stats_section->setStyleSheet(
        QStringLiteral("background: transparent; border-top: 1px solid #27272A;"));
    auto* coverage_stats_layout = new QVBoxLayout(coverage_stats_section);
    coverage_stats_layout->setContentsMargins(0, 10, 0, 0);
    coverage_stats_layout->setSpacing(8);
    coverage_stats_layout->addWidget(makeTrackedLabel(coverage_stats_section,
                                                      QStringLiteral("Coverage Statistics"),
                                                      kInitialHeading10,
                                                      &heading10_labels_));
    auto* coverage_stats_grid = new QGridLayout();
    coverage_stats_grid->setContentsMargins(0, 0, 0, 0);
    coverage_stats_grid->setHorizontalSpacing(8);
    coverage_stats_grid->setVerticalSpacing(8);
    coverage_stats_grid->addWidget(
        make_output_card(coverage_stats_section, QStringLiteral("Coverage Area"), &lbl_coverage_area_), 0, 0);
    coverage_stats_grid->addWidget(
        make_output_card(coverage_stats_section, QStringLiteral("Waypoints"), &lbl_coverage_waypoints_), 0, 1);
    coverage_stats_grid->addWidget(
        make_output_card(coverage_stats_section, QStringLiteral("Path Length"), &lbl_coverage_path_length_), 1, 0);
    coverage_stats_grid->addWidget(
        make_output_card(coverage_stats_section, QStringLiteral("Est. Time"), &lbl_coverage_est_time_), 1, 1);
    coverage_stats_layout->addLayout(coverage_stats_grid);
    coverage_obstacle_area_card_ = make_output_card(
        coverage_stats_section, QStringLiteral("Obstacles Area"), &lbl_coverage_obstacles_area_);
    coverage_stats_layout->addWidget(coverage_obstacle_area_card_);
    coverage_content_layout->addWidget(coverage_stats_section);
    coverage_content_layout->addStretch(1);

    coverage_scroll->setWidget(coverage_content);
    content_stack_->addWidget(coverage_placeholder_page_);

    // ---------------- Scan Splitting page (Stage 3) ----------------
    scan_splitting_page_ = new QWidget(content_stack_);
    scan_splitting_page_->setAttribute(Qt::WA_StyledBackground, true);
    scan_splitting_page_->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* scan_page_layout = new QVBoxLayout(scan_splitting_page_);
    scan_page_layout->setContentsMargins(0, 0, 0, 0);
    scan_page_layout->setSpacing(0);

    auto* scan_scroll = new QScrollArea(scan_splitting_page_);
    scan_scroll->setFrameShape(QFrame::NoFrame);
    scan_scroll->setWidgetResizable(true);
    scan_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scan_scroll->setStyleSheet(QStringLiteral("background: transparent;"));
    AutoHideScrollBar::install(scan_scroll, dark_mode_);
    scan_page_layout->addWidget(scan_scroll, 1);

    auto* scan_content = new QWidget(scan_scroll);
    scan_content->setAttribute(Qt::WA_StyledBackground, true);
    scan_content->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* scan_content_layout = new QVBoxLayout(scan_content);
    scan_content_layout->setContentsMargins(24, 16, 24, 16);
    scan_content_layout->setSpacing(20);

    scan_content_layout->addWidget(makeTrackedLabel(scan_content,
                                                    QStringLiteral("SCAN PLANNER"),
                                                    kInitialHeading10,
                                                    &heading10_labels_));

    auto* scan_distance_row = new QWidget(scan_content);
    auto* scan_distance_layout = new QHBoxLayout(scan_distance_row);
    scan_distance_layout->setContentsMargins(0, 0, 0, 0);
    scan_distance_layout->setSpacing(8);
    auto* lbl_scan_distance = makeTextLabel(
        scan_distance_row,
        QStringLiteral("Distance per scan (m):"),
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: #D4D4D8;"));
    scan_distance_layout->addWidget(lbl_scan_distance, 1);
    edit_scan_distance_ = new QLineEdit(scan_distance_row);
    edit_scan_distance_->setFixedWidth(96);
    edit_scan_distance_->setFixedHeight(34);
    edit_scan_distance_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    edit_scan_distance_->setText(QStringLiteral("500"));
    edit_scan_distance_->setStyleSheet(QStringLiteral(
        "QLineEdit { background: #27272A; border: 1px solid #3F3F47; border-radius: 4px; "
        "padding: 6px 8px; color: #E4E4E7; font-family: 'Arimo'; font-size: 14px; }"
        "QLineEdit:focus { border: 1px solid #00BC7D; }"));
    {
        auto* validator = new QDoubleValidator(0.5, 100000.0, 2, edit_scan_distance_);
        validator->setNotation(QDoubleValidator::StandardNotation);
        edit_scan_distance_->setValidator(validator);
    }
    scan_distance_layout->addWidget(edit_scan_distance_);
    scan_content_layout->addWidget(scan_distance_row);

    auto* scan_progression_section = new QWidget(scan_content);
    auto* scan_progression_layout = new QVBoxLayout(scan_progression_section);
    scan_progression_layout->setContentsMargins(0, 0, 0, 0);
    scan_progression_layout->setSpacing(6);
    auto* lbl_scan_progression = makeTextLabel(
        scan_progression_section,
        QStringLiteral("Progression Mode:"),
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: #D4D4D8;"));
    scan_progression_layout->addWidget(lbl_scan_progression);

    scan_progression_toggle_ = new QWidget(scan_progression_section);
    scan_progression_toggle_->setAttribute(Qt::WA_StyledBackground, true);
    scan_progression_toggle_->setFixedHeight(42);
    scan_progression_toggle_->setStyleSheet(
        QStringLiteral("background: #09090B; border: 1px solid #27272A; border-radius: 10px;"));
    auto* scan_progression_toggle_layout = new QHBoxLayout(scan_progression_toggle_);
    scan_progression_toggle_layout->setContentsMargins(5, 5, 5, 5);
    scan_progression_toggle_layout->setSpacing(0);

    auto make_toggle_button = [&](const QString& text) {
        auto* btn = new QPushButton(text, scan_progression_toggle_);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFlat(true);
        btn->setFixedHeight(32);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setCheckable(true);
        return btn;
    };
    btn_progression_automatic_ = make_toggle_button(QStringLiteral("Automatic"));
    btn_progression_manual_ = make_toggle_button(QStringLiteral("Manual"));
    scan_progression_toggle_layout->addWidget(btn_progression_automatic_);
    scan_progression_toggle_layout->addWidget(btn_progression_manual_);
    scan_progression_layout->addWidget(scan_progression_toggle_);
    scan_content_layout->addWidget(scan_progression_section);

    btn_scan_split_path_ = new QPushButton(scan_content);
    btn_scan_split_path_->setCursor(Qt::PointingHandCursor);
    btn_scan_split_path_->setFlat(true);
    btn_scan_split_path_->setFixedHeight(42);
    btn_scan_split_path_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn_scan_split_path_->setStyleSheet(
        QStringLiteral("QPushButton { background: #27272A; border: 1px solid #3F3F47; border-radius: 10px; }"
                       "QPushButton:hover { background: #2D2D31; }"
                       "QPushButton:disabled { background: rgba(39,39,42,0.5); color: #52525B; }"));
    {
        auto* split_layout = new QHBoxLayout(btn_scan_split_path_);
        split_layout->setContentsMargins(12, 8, 12, 8);
        split_layout->setSpacing(8);
        split_layout->addStretch(1);
        lbl_scan_split_path_icon_ = new QLabel(btn_scan_split_path_);
        lbl_scan_split_path_icon_->setFixedSize(14, 14);
        lbl_scan_split_path_icon_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        lbl_scan_split_path_icon_->setStyleSheet(QStringLiteral("background: transparent;"));
        lbl_scan_split_path_icon_->setPixmap(loadSvgPixmap(
            QStringLiteral(":/assets/missionplanner/split_path.svg"), 14, 14, QStringLiteral("#FFFFFF")));
        split_layout->addWidget(lbl_scan_split_path_icon_);
        auto* split_text = makeTextLabel(
            btn_scan_split_path_,
            QStringLiteral("Split path"),
            QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: #FFFFFF;"));
        split_layout->addWidget(split_text);
        split_layout->addStretch(1);
    }
    scan_content_layout->addWidget(btn_scan_split_path_);

    auto* scan_divider = new QFrame(scan_content);
    scan_divider->setFrameShape(QFrame::HLine);
    scan_divider->setStyleSheet(QStringLiteral("color: #27272A; background: #27272A; border: none;"));
    scan_divider->setFixedHeight(1);
    scan_content_layout->addWidget(scan_divider);

    btn_scan_publish_selected_ = new QPushButton(scan_content);
    btn_scan_publish_selected_->setCursor(Qt::PointingHandCursor);
    btn_scan_publish_selected_->setFlat(true);
    btn_scan_publish_selected_->setFixedHeight(40);
    btn_scan_publish_selected_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn_scan_publish_selected_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #008236; border: none; border-radius: 10px; }"
        "QPushButton:hover { background: #009644; }"
        "QPushButton:disabled { background: rgba(0,130,54,0.5); color: rgba(255,255,255,0.7); }"));
    {
        auto* publish_layout = new QHBoxLayout(btn_scan_publish_selected_);
        publish_layout->setContentsMargins(12, 8, 12, 8);
        publish_layout->setSpacing(8);
        publish_layout->addStretch(1);
        lbl_scan_publish_icon_ = new QLabel(btn_scan_publish_selected_);
        lbl_scan_publish_icon_->setFixedSize(16, 16);
        lbl_scan_publish_icon_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        lbl_scan_publish_icon_->setStyleSheet(QStringLiteral("background: transparent;"));
        lbl_scan_publish_icon_->setPixmap(
            loadSvgPixmap(QStringLiteral(":/assets/missionplanner/publish_selected.svg"),
                          16,
                          16,
                          QStringLiteral("#FFFFFF")));
        publish_layout->addWidget(lbl_scan_publish_icon_);
        auto* publish_text = makeTextLabel(
            btn_scan_publish_selected_,
            QStringLiteral("Publish selected"),
            QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: #FFFFFF;"));
        publish_layout->addWidget(publish_text);
        publish_layout->addStretch(1);
    }
    applyDropShadow(btn_scan_publish_selected_, 16, 4, QColor(0, 130, 54, 52));
    scan_content_layout->addWidget(btn_scan_publish_selected_);

    btn_scan_start_selected_ = new QPushButton(scan_content);
    btn_scan_start_selected_->setCursor(Qt::PointingHandCursor);
    btn_scan_start_selected_->setFlat(true);
    btn_scan_start_selected_->setFixedHeight(40);
    btn_scan_start_selected_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn_scan_start_selected_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #155DFC; border: none; border-radius: 10px; }"
        "QPushButton:hover { background: #1E6EFF; }"
        "QPushButton:disabled { background: rgba(21,93,252,0.5); color: rgba(255,255,255,0.7); }"));
    {
        auto* start_layout = new QHBoxLayout(btn_scan_start_selected_);
        start_layout->setContentsMargins(12, 8, 12, 8);
        start_layout->setSpacing(8);
        start_layout->addStretch(1);
        lbl_scan_start_icon_ = new QLabel(btn_scan_start_selected_);
        lbl_scan_start_icon_->setFixedSize(16, 16);
        lbl_scan_start_icon_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        lbl_scan_start_icon_->setStyleSheet(QStringLiteral("background: transparent;"));
        lbl_scan_start_icon_->setPixmap(
            loadSvgPixmap(QStringLiteral(":/assets/missionplanner/start_selected.svg"),
                          16,
                          16,
                          QStringLiteral("#FFFFFF")));
        start_layout->addWidget(lbl_scan_start_icon_);
        auto* start_text = makeTextLabel(
            btn_scan_start_selected_,
            QStringLiteral("Start selected"),
            QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: #FFFFFF;"));
        start_layout->addWidget(start_text);
        start_layout->addStretch(1);
    }
    applyDropShadow(btn_scan_start_selected_, 16, 4, QColor(21, 93, 252, 52));
    scan_content_layout->addWidget(btn_scan_start_selected_);

    auto* segment_actions_row = new QWidget(scan_content);
    segment_actions_row->setAttribute(Qt::WA_StyledBackground, true);
    segment_actions_row->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* segment_actions_layout = new QHBoxLayout(segment_actions_row);
    segment_actions_layout->setContentsMargins(2, 6, 2, 2);
    segment_actions_layout->setSpacing(14);
    segment_actions_layout->addStretch(1);

    const QString segment_action_qss = QStringLiteral(
        "QPushButton { background: transparent; border: none; "
        "  font-family: 'Arimo'; font-size: 13px; font-weight: 600; "
        "  padding: 4px 6px; color: %1; }"
        "QPushButton:hover { color: %2; }"
        "QPushButton:disabled { color: #3F3F47; }");

    btn_segments_select_all_ = new QPushButton(QStringLiteral("Select all"), segment_actions_row);
    btn_segments_select_all_->setFlat(true);
    btn_segments_select_all_->setCursor(Qt::PointingHandCursor);
    btn_segments_select_all_->setStyleSheet(segment_action_qss.arg("#00D492", "#34D399"));
    segment_actions_layout->addWidget(btn_segments_select_all_);

    btn_segments_clear_all_ = new QPushButton(QStringLiteral("Clear"), segment_actions_row);
    btn_segments_clear_all_->setFlat(true);
    btn_segments_clear_all_->setCursor(Qt::PointingHandCursor);
    btn_segments_clear_all_->setStyleSheet(segment_action_qss.arg("#A1A1AA", "#FFFFFF"));
    segment_actions_layout->addWidget(btn_segments_clear_all_);

    scan_content_layout->addWidget(segment_actions_row);

    list_scan_segments_ = new QListWidget(scan_content);
    list_scan_segments_->setSelectionMode(QAbstractItemView::MultiSelection);
    list_scan_segments_->setFocusPolicy(Qt::StrongFocus);
    list_scan_segments_->setAttribute(Qt::WA_StyledBackground, true);
    list_scan_segments_->setStyleSheet(QStringLiteral(
        "QListWidget { background: rgba(39,39,42,0.3); border: 1px solid #3F3F47; "
        "  border-radius: 10px; padding: 8px; color: #E4E4E7; "
        "  font-family: 'Arimo'; font-size: 17px; font-weight: 500; "
        "  outline: 0; }"
        "QListWidget::item { padding: 12px 44px 12px 18px; border-radius: 8px; "
        "  margin: 3px 0; }"
        "QListWidget::item:hover { background: rgba(63,63,71,0.4); }"
        "QListWidget::item:selected { background: #00BC7D; color: #FFFFFF; }"
        "QListWidget::item:selected:hover { background: #0ACB8B; }"));
    list_scan_segments_->setMinimumHeight(360);
    list_scan_segments_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    list_scan_segments_->setItemDelegate(
        new ScanSegmentItemDelegate(list_scan_segments_, list_scan_segments_));
    list_scan_segments_->installEventFilter(this);
    AutoHideScrollBar::install(list_scan_segments_, dark_mode_);
    scan_content_layout->addWidget(list_scan_segments_, 1);

    lbl_scan_segments_footer_ = makeTextLabel(
        scan_content,
        QStringLiteral("Segments: none"),
        QStringLiteral("font-family: 'Arimo'; font-size: 13px; font-weight: 400; color: #71717B;"));
    scan_content_layout->addWidget(lbl_scan_segments_footer_);

    lbl_scan_splitting_status_ = makeTextLabel(
        scan_content,
        QString(),
        QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: #71717B;"));
    lbl_scan_splitting_status_->setWordWrap(true);
    scan_content_layout->addWidget(lbl_scan_splitting_status_);

    scan_scroll->setWidget(scan_content);
    content_stack_->addWidget(scan_splitting_page_);

    // ---------------- Scan execution page (Stage 4) ----------------
    scan_page_ = buildScanPage(content_stack_);
    content_stack_->addWidget(scan_page_);

    auto* center_stage_host = new PlannerPreviewHost(body);
    center_stage_host->setAttribute(Qt::WA_StyledBackground, true);
    center_stage_host->setStyleSheet(QStringLiteral("background: #09090B;"));
    center_stage_ = center_stage_host;

    auto* center_base = new QWidget(center_stage_host);
    center_base->setAttribute(Qt::WA_StyledBackground, true);
    center_base->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* center_base_layout = new QVBoxLayout(center_base);
    center_base_layout->setContentsMargins(0, 0, 0, 0);
    center_base_layout->setSpacing(0);

    preview_container_ = new QWidget(center_base);
    preview_container_->setObjectName(QStringLiteral("plannerPreviewContainer"));
    preview_container_->setAttribute(Qt::WA_StyledBackground, true);
    preview_container_->setStyleSheet(QStringLiteral(
        "QWidget#plannerPreviewContainer { background: transparent; }"));
    preview_container_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    preview_container_->setMinimumSize(520, 360);
    QWidget* preview_container = preview_container_;
    auto* preview_layout = new QVBoxLayout(preview_container);
    // 1px contents margin so the QSS border on preview_container is not
    // painted over by preview_stack_/plot_ (which otherwise fill the entire
    // rectangle and erase the border edge). The 1px inset is imperceptible
    // on non-Scan stages where no border is set.
    preview_layout->setContentsMargins(1, 1, 1, 1);
    preview_layout->setSpacing(0);

    preview_stack_ = new QStackedWidget(preview_container);
    preview_stack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    preview_stack_->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    preview_placeholder_ = new QWidget(preview_stack_);
    preview_placeholder_->setAttribute(Qt::WA_StyledBackground, true);
    preview_placeholder_->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* preview_placeholder_layout = new QVBoxLayout(preview_placeholder_);
    preview_placeholder_layout->setContentsMargins(0, 0, 0, 0);
    preview_placeholder_layout->addStretch(1);
    auto* preview_canvas = new QWidget(preview_placeholder_);
    preview_canvas->setFixedSize(600, 400);
    preview_canvas->setAttribute(Qt::WA_TranslucentBackground, true);
    preview_canvas->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* preview_base = new QLabel(preview_canvas);
    preview_base->setGeometry(0, 0, 600, 400);
    preview_base->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    preview_base->setStyleSheet(QStringLiteral("background: transparent;"));
    preview_base->setScaledContents(true);
    preview_base->setPixmap(loadSvgPixmap(
        QStringLiteral(":/assets/missionplanner/point_cloud_preview_base.svg"), 600, 400));
    auto* preview_points = new QLabel(preview_canvas);
    preview_points->setGeometry(0, 0, 600, 400);
    preview_points->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    preview_points->setStyleSheet(QStringLiteral("background: transparent;"));
    preview_points->setScaledContents(true);
    preview_points->setPixmap(loadSvgPixmap(
        QStringLiteral(":/assets/missionplanner/point_cloud_preview_points.svg"), 600, 400));
    preview_placeholder_layout->addWidget(preview_canvas, 0, Qt::AlignCenter);
    preview_placeholder_layout->addStretch(1);
    preview_stack_->addWidget(preview_placeholder_);

    plot_ = new PlotWidget(preview_stack_);
    plot_->setMinimumSize(520, 360);
    plot_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    plot_->setDarkMode(dark_mode_);
    plot_->setPlannerPreviewMode(true);
    plot_->setRobotMarkerSize(robot_marker_size_m_);
    preview_stack_->addWidget(plot_);
    auto* coverage_selection_state_timer = new QTimer(this);
    coverage_selection_state_timer->setInterval(120);
    connect(coverage_selection_state_timer, &QTimer::timeout, this, [this]() {
        if (!plot_ || current_step_ != PlannerStep::CoveragePlanning) {
            return;
        }
        SessionCache& cache = activeSession();
        if ((cache.coverage_roi_drawing_active || cache.coverage_drawing_active) &&
            !plot_->isSelecting() && !plot_->isDrawingRectangle()) {
            cache.coverage_roi_drawing_active = false;
            cache.coverage_drawing_active = false;
            updateButtonsAndStatus();
        }
    });
    coverage_selection_state_timer->start();
    preview_stack_->setCurrentWidget(preview_placeholder_);
    preview_layout->addWidget(preview_stack_, 1);

    center_base_layout->addWidget(preview_container, 1);

    tool_stack_ = new QWidget(center_stage_host);
    QWidget* tool_stack = tool_stack_;
    auto* tool_stack_layout = new QVBoxLayout(tool_stack);
    tool_stack_layout->setContentsMargins(0, 0, 0, 0);
    tool_stack_layout->setSpacing(6);
    tool_zoom_in_ =
        make_tool_button(tool_stack, QStringLiteral(":/assets/missionplanner/tool_zoom_in.svg"),
                         &lbl_tool_zoom_in_icon_);
    tool_fit_ =
        make_tool_button(tool_stack, QStringLiteral(":/assets/missionplanner/tool_fit.svg"),
                         &lbl_tool_fit_icon_);
    tool_reset_ =
        make_tool_button(tool_stack, QStringLiteral(":/assets/missionplanner/tool_reset.svg"),
                         &lbl_tool_reset_icon_);
    connect(tool_zoom_in_, &QPushButton::clicked, this, [this]() {
        if (plot_ && preview_stack_ && preview_stack_->currentWidget() == plot_) {
            plot_->zoomIn();
        }
    });
    connect(tool_fit_, &QPushButton::clicked, this, [this]() {
        if (plot_ && preview_stack_ && preview_stack_->currentWidget() == plot_) {
            plot_->resetView();
        }
    });
    connect(tool_reset_, &QPushButton::clicked, this, [this]() {
        if (plot_ && preview_stack_ && preview_stack_->currentWidget() == plot_) {
            plot_->resetView();
        }
    });
    tool_stack_layout->addWidget(tool_zoom_in_);
    tool_stack_layout->addWidget(tool_fit_);
    tool_stack_layout->addWidget(tool_reset_);

    stats_chip_ = new QWidget(center_stage_host);
    stats_chip_->setFixedHeight(66);
    stats_chip_->setMinimumWidth(201);
    stats_chip_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    stats_chip_->setAttribute(Qt::WA_StyledBackground, true);
    stats_chip_->setStyleSheet(QStringLiteral(
        "background: rgba(24,24,27,0.9);"
        "border: 1px solid #27272A;"
        "border-radius: 10px;"));
    auto* stats_layout = new QHBoxLayout(stats_chip_);
    stats_layout->setContentsMargins(17, 13, 17, 13);
    stats_layout->setSpacing(24);

    auto* points_host = new QWidget(stats_chip_);
    points_host->setMinimumWidth(76);
    points_host->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    auto* points_col = new QVBoxLayout(points_host);
    points_col->setContentsMargins(0, 0, 0, 0);
    points_col->setSpacing(0);
    points_col->addWidget(
        makeTrackedLabel(points_host, QStringLiteral("Points"), kInitialMono12Muted, &mono12_muted_labels_));
    lbl_stats_points_ = makeTextLabel(points_host,
                                      QStringLiteral("--"),
                                      QStringLiteral("font-family: 'Liberation Mono'; "
                                                     "font-size: 14px; font-weight: 400; "
                                                     "color: #E4E4E7;"));
    points_col->addWidget(lbl_stats_points_);
    stats_layout->addWidget(points_host);

    auto* area_host = new QWidget(stats_chip_);
    area_host->setMinimumWidth(68);
    area_host->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    auto* area_col = new QVBoxLayout(area_host);
    area_col->setContentsMargins(0, 0, 0, 0);
    area_col->setSpacing(0);
    area_col->addWidget(
        makeTrackedLabel(area_host, QStringLiteral("Area"), kInitialMono12Muted, &mono12_muted_labels_));
    lbl_stats_area_ = makeTextLabel(area_host,
                                    QStringLiteral("--"),
                                    QStringLiteral("font-family: 'Liberation Mono'; "
                                                   "font-size: 14px; font-weight: 400; "
                                                   "color: #E4E4E7;"));
    area_col->addWidget(lbl_stats_area_);
    stats_layout->addWidget(area_host);

    preview_bottom_overlay_stack_ = new QStackedWidget(center_stage_host);
    preview_bottom_overlay_stack_->setStyleSheet(
        QStringLiteral("background: transparent; border: none;"));
    preview_bottom_overlay_stack_->addWidget(stats_chip_);

    coverage_legend_chip_ = new QWidget(preview_bottom_overlay_stack_);
    coverage_legend_chip_->setFixedHeight(50);
    coverage_legend_chip_->setMinimumWidth(228);
    coverage_legend_chip_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    coverage_legend_chip_->setAttribute(Qt::WA_StyledBackground, true);
    auto* legend_layout = new QHBoxLayout(coverage_legend_chip_);
    legend_layout->setContentsMargins(16, 12, 16, 12);
    legend_layout->setSpacing(18);

    auto* boundary_legend = new QWidget(coverage_legend_chip_);
    auto* boundary_legend_layout = new QHBoxLayout(boundary_legend);
    boundary_legend_layout->setContentsMargins(0, 0, 0, 0);
    boundary_legend_layout->setSpacing(8);
    coverage_legend_boundary_swatch_ = new QWidget(boundary_legend);
    coverage_legend_boundary_swatch_->setFixedSize(12, 12);
    coverage_legend_boundary_swatch_->setAttribute(Qt::WA_StyledBackground, true);
    boundary_legend_layout->addWidget(coverage_legend_boundary_swatch_, 0, Qt::AlignVCenter);
    lbl_coverage_legend_boundary_ = makeTextLabel(
        boundary_legend,
        QStringLiteral("Boundary Hull"),
        QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: #A1A1AA;"));
    boundary_legend_layout->addWidget(lbl_coverage_legend_boundary_, 0, Qt::AlignVCenter);
    legend_layout->addWidget(boundary_legend);

    auto* path_legend = new QWidget(coverage_legend_chip_);
    auto* path_legend_layout = new QHBoxLayout(path_legend);
    path_legend_layout->setContentsMargins(0, 0, 0, 0);
    path_legend_layout->setSpacing(8);
    coverage_legend_path_swatch_ = new QWidget(path_legend);
    coverage_legend_path_swatch_->setFixedSize(24, 2);
    coverage_legend_path_swatch_->setAttribute(Qt::WA_StyledBackground, true);
    path_legend_layout->addWidget(coverage_legend_path_swatch_, 0, Qt::AlignVCenter);
    lbl_coverage_legend_path_ = makeTextLabel(
        path_legend,
        QStringLiteral("Coverage Path"),
        QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: #A1A1AA;"));
    path_legend_layout->addWidget(lbl_coverage_legend_path_, 0, Qt::AlignVCenter);
    legend_layout->addWidget(path_legend);

    preview_bottom_overlay_stack_->addWidget(coverage_legend_chip_);
    preview_bottom_overlay_stack_->setCurrentWidget(stats_chip_);

    // ---------------- Scan execution: top-left segment legend ----------------
    // Lives in the new top-left overlay slot (matches Figma) instead of being
    // wedged into the bottom-left stack.
    scan_legend_chip_ = buildScanLegendChip(center_stage_host);
    scan_legend_chip_->setVisible(false);
    center_stage_host->setTopLeftOverlay(scan_legend_chip_);

    center_stage_host->setBaseWidget(center_base);

    // Wrap the existing tool_stack and the new scan status pill in one column
    // so they can share the top-right overlay slot. Pill on top so it gets the
    // most prominent corner; tool stack remains visible on Scan per UX call.
    //
    // No stylesheet on the wrapper itself: an unqualified `background:` rule
    // here would cascade to the pill (which has its own background) and erase
    // it. The wrapper is a pure layout container — it must not paint.
    auto* top_right_column = new QWidget(center_stage_host);
    auto* top_right_column_layout = new QVBoxLayout(top_right_column);
    top_right_column_layout->setContentsMargins(0, 0, 0, 0);
    top_right_column_layout->setSpacing(8);
    top_right_column_layout->setAlignment(Qt::AlignRight | Qt::AlignTop);
    scan_status_pill_ = buildScanStatusPill(top_right_column);
    scan_status_pill_->setVisible(false);
    top_right_column_layout->addWidget(scan_status_pill_, 0, Qt::AlignRight);
    top_right_column_layout->addWidget(tool_stack, 0, Qt::AlignRight);
    center_stage_host->setTopRightOverlay(top_right_column);
    center_stage_host->setBottomLeftOverlay(preview_bottom_overlay_stack_);

    // The Scan-stage segment legend lives at the TOP-LEFT of the map (matches
    // the Figma frames). It is built later (after preview_bottom_overlay_stack_
    // is finalized) and installed into the new top-left overlay slot.

    // Center column: stack the map host vertically with the Scan control bar.
    // Lifting the bar out of the PreviewHost's base widget means the bar no
    // longer sits under the bottom-left overlay zone, and its left/right edges
    // align with the map card itself (not the page edges).
    auto* center_column = new QWidget(body);
    center_column->setAttribute(Qt::WA_StyledBackground, true);
    center_column->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* center_column_layout = new QVBoxLayout(center_column);
    center_column_layout->setContentsMargins(0, 0, 0, 0);
    center_column_layout->setSpacing(0);
    center_column_layout->addWidget(center_stage_host, 1);

    scan_control_bar_ = buildScanCenterControlBar(center_column);
    scan_control_bar_->setVisible(false);
    center_column_layout->addWidget(scan_control_bar_);

    body_layout->addWidget(center_column, 1);

    // ---------------- Scan execution: right rail ----------------
    scan_right_rail_ = buildScanRightRail(body);
    scan_right_rail_->setVisible(false);
    body_layout->addWidget(scan_right_rail_);

    footer_ = new QWidget(this);
    footer_->setFixedHeight(69);
    footer_->setAttribute(Qt::WA_StyledBackground, true);
    footer_->setStyleSheet(QStringLiteral("background: #18181B;"));
    auto* footer_layout = new QHBoxLayout(footer_);
    footer_layout->setContentsMargins(24, 0, 24, 0);
    footer_layout->setSpacing(0);

    footer_left_stack_ = new QStackedWidget(footer_);
    footer_left_stack_->setFixedWidth(kFooterCallToActionWidth);
    footer_left_stack_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    footer_left_stack_->setStyleSheet(QStringLiteral("background: transparent; border: none;"));

    auto* footer_left_balance = new QWidget(footer_left_stack_);
    footer_left_balance->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    footer_left_stack_->addWidget(footer_left_balance);

    // Footer back-to-Stage-3 button, only shown on Stage 4 (Scan).
    footer_back_stage3_ = new QPushButton(footer_left_stack_);
    footer_back_stage3_->setCursor(Qt::PointingHandCursor);
    footer_back_stage3_->setFlat(true);
    footer_back_stage3_->setFixedHeight(44);
    footer_back_stage3_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    footer_back_stage3_->setMinimumWidth(kFooterCallToActionWidth);
    // Borderless to match the top-bar "← Exploration" back button. Matches
    // Figma — footer stage-back is a text-only button, not a chip.
    footer_back_stage3_->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; border-radius: 10px; }"
        "QPushButton:hover { background: rgba(39,39,42,0.55); }"));
    auto* footer_back_layout = new QHBoxLayout(footer_back_stage3_);
    footer_back_layout->setContentsMargins(16, 0, 16, 0);
    footer_back_layout->setSpacing(10);
    auto* lbl_footer_back_icon = makeIconLabel(footer_back_stage3_,
                                               QStringLiteral(":/assets/missionplanner/back.svg"),
                                               16,
                                               QStringLiteral("#9F9FA9"));
    footer_back_layout->addWidget(lbl_footer_back_icon);
    auto* lbl_footer_back_text = makeTextLabel(
        footer_back_stage3_,
        QStringLiteral("Stage 3 (Scan Splitting)"),
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 500; color: #D4D4D8;"));
    footer_back_layout->addWidget(lbl_footer_back_text);
    footer_back_layout->addStretch(1);
    connect(footer_back_stage3_, &QPushButton::clicked,
            this, &PlannerScreen::onScanFooterBackClicked);
    footer_left_stack_->addWidget(footer_back_stage3_);

    footer_layout->addWidget(footer_left_stack_);

    auto* footer_stage_host = new QWidget(footer_);
    footer_stage_host->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* footer_stage_layout = new QHBoxLayout(footer_stage_host);
    footer_stage_layout->setContentsMargins(0, 0, 0, 0);
    footer_stage_layout->setSpacing(0);
    footer_stage_layout->addStretch(1);

    lbl_stage_footer_ = makeTextLabel(
        footer_stage_host,
        QStringLiteral("Stage 1 of 4"),
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 400; "
                       "color: #71717B;"));
    footer_stage_layout->addWidget(lbl_stage_footer_);
    footer_stage_layout->addStretch(1);
    footer_layout->addWidget(footer_stage_host, 1);

    btn_next_ = new QPushButton(footer_);
    btn_next_->setObjectName(QStringLiteral("PlannerFooterNextButton"));
    btn_next_->setCursor(Qt::PointingHandCursor);
    btn_next_->setFlat(true);
    btn_next_->setAutoDefault(false);
    btn_next_->setDefault(false);
    btn_next_->installEventFilter(this);
    btn_next_->setFixedHeight(44);
    btn_next_->setMinimumWidth(kFooterCallToActionWidth);
    btn_next_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    btn_next_->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: #00BC7D;"
        "  border: none;"
        "  border-radius: 10px;"
        "}"
        "QPushButton:hover { background: #0ACB8B; }"
        "QPushButton:disabled { background: #1F2937; }"));
    auto* next_layout = new QHBoxLayout(btn_next_);
    next_layout->setContentsMargins(16, 0, 16, 0);
    next_layout->setSpacing(10);
    next_layout->addStretch(1);
    lbl_next_text_ = makeTextLabel(
        btn_next_,
        QStringLiteral("Stage 2 (Coverage Planning)"),
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; color: #FFFFFF;"));
    next_layout->addWidget(lbl_next_text_);
    lbl_next_icon_ =
        makeIconLabel(btn_next_, QStringLiteral(":/assets/missionplanner/next_arrow.svg"), 16,
                      QStringLiteral("#FFFFFF"));
    next_layout->addWidget(lbl_next_icon_);
    connect(btn_next_, &QPushButton::clicked, this, &PlannerScreen::onNextClicked);
    footer_layout->addWidget(btn_next_, 0, Qt::AlignVCenter);
    root_layout->addWidget(footer_);

    slider_voxel_->on_value_changed = [this](double value) {
        if (syncing_widgets_) {
            return;
        }
        SessionCache& cache = activeSession();
        cache.voxel_size = value;
        persistParameters();
        updateValueLabels();
        invalidateProcessingResult(
            QStringLiteral("Voxel size changed. Re-run point cloud processing to refresh the result."));
    };

    slider_z_min_->on_value_changed = [this](double value) {
        if (syncing_widgets_) {
            return;
        }
        SessionCache& cache = activeSession();
        cache.z_min = value;
        if (cache.z_min > cache.z_max) {
            cache.z_max = cache.z_min;
            syncing_widgets_ = true;
            slider_z_max_->setValue(cache.z_max);
            syncing_widgets_ = false;
        }
        persistParameters();
        updateValueLabels();
        invalidateProcessingResult(
            QStringLiteral("Height filter changed. Re-run point cloud processing to refresh the result."));
    };

    slider_z_max_->on_value_changed = [this](double value) {
        if (syncing_widgets_) {
            return;
        }
        SessionCache& cache = activeSession();
        cache.z_max = value;
        if (cache.z_max < cache.z_min) {
            cache.z_min = cache.z_max;
            syncing_widgets_ = true;
            slider_z_min_->setValue(cache.z_min);
            syncing_widgets_ = false;
        }
        persistParameters();
        updateValueLabels();
        invalidateProcessingResult(
            QStringLiteral("Height filter changed. Re-run point cloud processing to refresh the result."));
    };

    slider_alpha_->on_value_changed = [this](double value) {
        if (syncing_widgets_) {
            return;
        }
        SessionCache& cache = activeSession();
        cache.alpha = value;
        persistParameters();
        updateValueLabels();
        invalidateHullResult(
            QStringLiteral("Hull parameter changed. Recompute the hull to refresh the boundary."));
    };

    auto apply_coverage_preset = [this](const QString& preset_name) {
        SessionCache& cache = activeSession();
        ensureCoverageDefaults(cache);
        const auto preset_it =
            std::find_if(cache.coverage_presets.begin(),
                         cache.coverage_presets.end(),
                         [&preset_name](const SessionCache::CoveragePreset& preset) {
                             return preset.name == preset_name;
                         });
        if (preset_it == cache.coverage_presets.end()) {
            return;
        }

        cache.coverage_selected_preset = preset_it->name;
        cache.coverage_pattern = preset_it->route_pattern;
        cache.coverage_path_spacing = preset_it->path_spacing;
        cache.coverage_headland_width = preset_it->headland_width;
        cache.coverage_scan_axis = preset_it->scan_axis;
        cache.coverage_scan_speed_mps =
            clampValue(preset_it->scan_speed_mps,
                       kCoverageScanSpeedMin,
                       kCoverageScanSpeedMax);
        cache.coverage_show_save_preset = false;
        cache.coverage_new_preset_name.clear();
        persistParameters();
        invalidateCoverageResult(
            QStringLiteral("Coverage preset changed. Generate coverage paths again to refresh the preview."));
        applySessionToUi();
    };
    auto cancel_coverage_selection = [this]() {
        if (!plot_) {
            return;
        }
        if (plot_->isSelecting()) {
            plot_->cancelSelection();
        }
        if (plot_->isDrawingRectangle()) {
            plot_->cancelRectangleMode();
        }
    };
    auto add_manual_obstacle = [this](const Polygon2D& shape) {
        SessionCache& cache = activeSession();
        SessionCache::CoverageObstacle obstacle;
        obstacle.id = cache.coverage_next_obstacle_id++;
        obstacle.type = cache.coverage_drawing_tool == QStringLiteral("circle")
                            ? QStringLiteral("Circle")
                            : cache.coverage_drawing_tool == QStringLiteral("polygon")
                                ? QStringLiteral("Polygon")
                                : QStringLiteral("Rectangle");
        obstacle.area_m2 = std::abs(polygonArea(shape));
        obstacle.source = QStringLiteral("manual");
        obstacle.geometry = Obstacle2D{shape, {}};
        cache.coverage_obstacles.push_back(obstacle);
    };

    connect(plot_, &PlotWidget::roiSelected, this, [this](const Polygon2D& roi) {
        SessionCache& cache = activeSession();
        cache.coverage_roi_polygon = roi;
        cache.coverage_roi_drawing_active = false;
        invalidateCoverageResult(
            QStringLiteral("ROI updated. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });
    connect(plot_, &PlotWidget::obstacleSelected, this, [this, add_manual_obstacle](const Polygon2D& obstacle) {
        SessionCache& cache = activeSession();
        add_manual_obstacle(obstacle);
        cache.coverage_drawing_active = false;
        invalidateCoverageResult(
            QStringLiteral("Manual obstacle added. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });
    connect(plot_, &PlotWidget::rectangleCompleted, this, [this, add_manual_obstacle](const Polygon2D& rect) {
        SessionCache& cache = activeSession();
        if (cache.coverage_roi_drawing_active) {
            cache.coverage_roi_polygon = rect;
            cache.coverage_roi_drawing_active = false;
            invalidateCoverageResult(
                QStringLiteral("ROI updated. Generate coverage paths again to refresh the preview."));
            updatePreview();
            updateButtonsAndStatus();
            return;
        }
        if (cache.coverage_drawing_active) {
            const Polygon2D shape =
                cache.coverage_drawing_tool == QStringLiteral("circle")
                    ? makeEllipsePolygonFromRectangle(rect)
                    : rect;
            add_manual_obstacle(shape);
            cache.coverage_drawing_active = false;
            invalidateCoverageResult(
                QStringLiteral("Manual obstacle added. Generate coverage paths again to refresh the preview."));
            updatePreview();
            updateButtonsAndStatus();
        }
    });
    connect(plot_, &PlotWidget::selectionCancelled, this, [this]() {
        SessionCache& cache = activeSession();
        if (!cache.coverage_roi_drawing_active && !cache.coverage_drawing_active) {
            return;
        }
        cache.coverage_roi_drawing_active = false;
        cache.coverage_drawing_active = false;
        setInlineStatus(QStringLiteral("Selection cancelled."), QStringLiteral("#71717B"));
        updateButtonsAndStatus();
    });
    connect(plot_, &PlotWidget::obstacleDeleteRequested, this, [this](int index) {
        SessionCache& cache = activeSession();
        if (index < 0 || index >= static_cast<int>(cache.coverage_obstacles.size())) {
            return;
        }
        cache.coverage_obstacles.erase(cache.coverage_obstacles.begin() + index);
        if (cache.coverage_obstacles.empty()) {
            cache.coverage_obstacles_detected = false;
        }
        invalidateCoverageResult(
            QStringLiteral("Obstacle removed. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });

    connect(btn_coverage_scan_complete_, &QPushButton::clicked, this, [this, cancel_coverage_selection]() {
        SessionCache& cache = activeSession();
        cancel_coverage_selection();
        cache.coverage_scan_mode = QStringLiteral("complete");
        cache.coverage_roi_drawing_active = false;
        cache.coverage_drawing_active = false;
        persistParameters();
        invalidateCoverageResult(
            QStringLiteral("Scan mode changed. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });
    connect(btn_coverage_scan_roi_, &QPushButton::clicked, this, [this, cancel_coverage_selection]() {
        SessionCache& cache = activeSession();
        cancel_coverage_selection();
        cache.coverage_scan_mode = QStringLiteral("roi");
        cache.coverage_roi_drawing_active = false;
        cache.coverage_drawing_active = false;
        persistParameters();
        invalidateCoverageResult(
            QStringLiteral("Scan mode changed. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });
    connect(btn_coverage_roi_draw_rectangle_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_roi_drawing_tool = QStringLiteral("rectangle");
        persistParameters();
        updateCoveragePlanningUi();
    });
    connect(btn_coverage_roi_draw_polygon_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_roi_drawing_tool = QStringLiteral("polygon");
        persistParameters();
        updateCoveragePlanningUi();
    });
    connect(btn_coverage_roi_start_, &QPushButton::clicked, this, [this, cancel_coverage_selection]() {
        SessionCache& cache = activeSession();
        if (!cache.hull_complete || cache.hull_polygon.empty()) {
            setInlineStatus(QStringLiteral("Compute and project the hull before drawing an ROI."),
                            QStringLiteral("#F59E0B"));
            updateButtonsAndStatus();
            return;
        }

        cancel_coverage_selection();
        cache.coverage_drawing_active = false;
        cache.coverage_roi_drawing_active = true;
        if (plot_) {
            if (cache.coverage_roi_drawing_tool == QStringLiteral("polygon")) {
                plot_->startROISelection();
            } else {
                plot_->startRectangleMode();
            }
            plot_->setFocus();
        }
        setInlineStatus(QStringLiteral("Define the ROI on the map preview."),
                        QStringLiteral("#71717B"));
        updateButtonsAndStatus();
    });
    connect(btn_coverage_roi_clear_, &QPushButton::clicked, this, [this, cancel_coverage_selection]() {
        SessionCache& cache = activeSession();
        cancel_coverage_selection();
        cache.coverage_roi_polygon.clear();
        cache.coverage_roi_drawing_active = false;
        invalidateCoverageResult(
            QStringLiteral("ROI cleared. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });

    connect(combo_coverage_presets_,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this, apply_coverage_preset](int index) {
                if (syncing_widgets_ || index < 0 || !combo_coverage_presets_) {
                    return;
                }
                apply_coverage_preset(combo_coverage_presets_->itemData(index).toString());
            });
    connect(btn_coverage_preset_add_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_show_save_preset = !cache.coverage_show_save_preset;
        if (!cache.coverage_show_save_preset) {
            cache.coverage_new_preset_name.clear();
        }
        updateCoveragePlanningUi();
        applySessionToUi();
    });
    connect(edit_coverage_preset_name_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (syncing_widgets_) {
            return;
        }
        activeSession().coverage_new_preset_name = text;
        updateCoveragePlanningUi();
    });
    connect(btn_coverage_preset_cancel_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_show_save_preset = false;
        cache.coverage_new_preset_name.clear();
        applySessionToUi();
    });
    connect(btn_coverage_preset_save_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        const QString preset_name = cache.coverage_new_preset_name.trimmed();
        if (preset_name.isEmpty()) {
            return;
        }

        // Reject collisions with reserved factory preset names. Factories are
        // immutable code constants — letting a custom shadow them would make
        // the in-memory derived cache ambiguous and break `isFactoryPresetName`.
        if (isFactoryPresetName(preset_name)) {
            BdrMessageBox::warning(
                this,
                QStringLiteral("Reserved name"),
                QStringLiteral("'%1' is reserved for factory presets. "
                               "Choose a different name.").arg(preset_name));
            return;
        }

        if (!preset_manager_) {
            BdrMessageBox::critical(
                this,
                QStringLiteral("Save failed"),
                QStringLiteral("Preset manager unavailable. Please restart the app."));
            return;
        }

        if (preset_manager_->presetExists(preset_name)) {
            const int choice = BdrMessageBox::question(
                this,
                QStringLiteral("Overwrite preset"),
                QStringLiteral("A preset named '%1' already exists. Overwrite it?")
                    .arg(preset_name),
                BdrMessageBox::No);
            if (choice != BdrMessageBox::Yes) {
                return;
            }
        }

        PlanningPreset to_save = buildPlanningPresetFromSession(cache);
        to_save.name = preset_name;
        if (!preset_manager_->savePreset(to_save)) {
            BdrMessageBox::critical(
                this,
                QStringLiteral("Save failed"),
                QStringLiteral("Could not save preset '%1'. Check filesystem permissions "
                               "for ~/.config/PilotControl/presets/.").arg(preset_name));
            return;
        }

        // savePreset emitted presetsChanged synchronously — the connection
        // already reloaded coverage_presets and refreshed the UI but used
        // the OLD selected name. Commit the new selection now and re-apply.
        cache.coverage_selected_preset = preset_name;
        cache.coverage_show_save_preset = false;
        cache.coverage_new_preset_name.clear();
        persistParameters();
        applySessionToUi();
    });

    connect(btn_coverage_pattern_boustro_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_pattern = QStringLiteral("boustro");
        persistParameters();
        invalidateCoverageResult(
            QStringLiteral("Coverage pattern changed. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });
    connect(btn_coverage_pattern_snake_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_pattern = QStringLiteral("snake");
        persistParameters();
        invalidateCoverageResult(
            QStringLiteral("Coverage pattern changed. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });
    connect(btn_coverage_pattern_spiral_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_pattern = QStringLiteral("spiral");
        persistParameters();
        invalidateCoverageResult(
            QStringLiteral("Coverage pattern changed. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });

    if (edit_scan_distance_) {
        connect(edit_scan_distance_, &QLineEdit::editingFinished, this, [this]() {
            onScanDistanceEdited();
        });
    }
    if (btn_progression_automatic_) {
        connect(btn_progression_automatic_, &QPushButton::clicked, this, [this]() {
            onProgressionModeChanged(QStringLiteral("automatic"));
        });
    }
    if (btn_progression_manual_) {
        connect(btn_progression_manual_, &QPushButton::clicked, this, [this]() {
            onProgressionModeChanged(QStringLiteral("manual"));
        });
    }
    if (btn_scan_split_path_) {
        connect(btn_scan_split_path_, &QPushButton::clicked, this, [this]() {
            onSplitPathClicked();
        });
    }
    if (btn_scan_publish_selected_) {
        connect(btn_scan_publish_selected_, &QPushButton::clicked, this, [this]() {
            onPublishSelectedClicked();
        });
    }
    if (btn_scan_start_selected_) {
        connect(btn_scan_start_selected_, &QPushButton::clicked, this, [this]() {
            onStartSelectedClicked();
        });
    }
    if (list_scan_segments_) {
        connect(list_scan_segments_,
                &QListWidget::itemSelectionChanged,
                this,
                [this]() {
                    if (!list_scan_segments_) {
                        return;
                    }
                    SessionCache& cache = activeSession();
                    QSignalBlocker blocker(list_scan_segments_);
                    for (int i = 0; i < list_scan_segments_->count(); ++i) {
                        if (i >= static_cast<int>(cache.scan_segments.size())) {
                            break;
                        }
                        auto* item = list_scan_segments_->item(i);
                        if (!item) continue;
                        auto& seg = cache.scan_segments[static_cast<size_t>(i)];
                        if (seg.completed) {
                            // Locked: completed segments stay deselected.
                            if (item->isSelected()) {
                                item->setSelected(false);
                            }
                            seg.selected = false;
                        } else {
                            seg.selected = item->isSelected();
                        }
                    }
                    updateScanSplittingUi();
                    pushScanSegmentsToPlot();
                });
        connect(list_scan_segments_,
                &QListWidget::currentRowChanged,
                this,
                [this](int row) {
                    if (plot_) {
                        plot_->setActiveScanSegment(row);
                    }
                    if (list_scan_segments_ && list_scan_segments_->viewport()) {
                        list_scan_segments_->viewport()->update();
                    }
                });
    }
    if (btn_segments_select_all_) {
        connect(btn_segments_select_all_, &QPushButton::clicked, this, [this]() {
            SessionCache& cache = activeSession();
            for (auto& seg : cache.scan_segments) {
                if (!seg.completed) {
                    seg.selected = true;
                }
            }
            refreshScanSegmentList();
            pushScanSegmentsToPlot();
            updateScanSplittingUi();
        });
    }
    if (btn_segments_clear_all_) {
        connect(btn_segments_clear_all_, &QPushButton::clicked, this, [this]() {
            SessionCache& cache = activeSession();
            for (auto& seg : cache.scan_segments) {
                seg.selected = false;
            }
            refreshScanSegmentList();
            pushScanSegmentsToPlot();
            updateScanSplittingUi();
        });
    }
    connect(btn_coverage_axis_parallel_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_scan_axis = QStringLiteral("parallel");
        persistParameters();
        invalidateCoverageResult(
            QStringLiteral("Scan axis changed. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });
    connect(btn_coverage_axis_perpendicular_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_scan_axis = QStringLiteral("perpendicular");
        persistParameters();
        invalidateCoverageResult(
            QStringLiteral("Scan axis changed. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });

    slider_coverage_path_spacing_->on_value_changed = [this](double value) {
        if (syncing_widgets_) {
            return;
        }
        SessionCache& cache = activeSession();
        cache.coverage_path_spacing = value;
        persistParameters();
        updateValueLabels();
        invalidateCoverageResult(
            QStringLiteral("Path spacing changed. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    };
    slider_coverage_headland_->on_value_changed = [this](double value) {
        if (syncing_widgets_) {
            return;
        }
        SessionCache& cache = activeSession();
        cache.coverage_headland_width = value;
        persistParameters();
        updateValueLabels();
        invalidateCoverageResult(
            QStringLiteral("Headland width changed. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    };
    slider_coverage_scan_speed_->on_value_changed = [this](double value) {
        if (syncing_widgets_) {
            return;
        }
        SessionCache& cache = activeSession();
        // Snap to the configured 0.1 m/s grid before persist. The slider
        // emits raw `double`s on every track tick, so explicit snap keeps
        // QSettings, the JSON preset payload, and the controller param in
        // lockstep with what the operator actually selected.
        const double snapped = clampValue(
            std::round(value / kCoverageScanSpeedStep) * kCoverageScanSpeedStep,
            kCoverageScanSpeedMin,
            kCoverageScanSpeedMax);
        cache.coverage_scan_speed_mps = snapped;
        persistParameters();
        updateValueLabels();
        // No invalidateCoverageResult / updatePreview — speed is config-only;
        // the boustrophedon path is unchanged, the controller just cruises
        // it faster/slower. ETA card refreshes via updateValueLabels for
        // free (effectiveScanSpeedMps reads cache.coverage_scan_speed_mps).
        updateCoveragePlanningUi();
    };
    connect(btn_coverage_obstacle_auto_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        if (plot_) {
            if (plot_->isSelecting()) {
                plot_->cancelSelection();
            }
            if (plot_->isDrawingRectangle()) {
                plot_->cancelRectangleMode();
            }
        }
        cache.coverage_obstacle_mode = QStringLiteral("automatic");
        cache.coverage_roi_drawing_active = false;
        cache.coverage_drawing_active = false;
        persistParameters();
        updateButtonsAndStatus();
    });
    connect(btn_coverage_obstacle_manual_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        if (plot_) {
            if (plot_->isSelecting()) {
                plot_->cancelSelection();
            }
            if (plot_->isDrawingRectangle()) {
                plot_->cancelRectangleMode();
            }
        }
        cache.coverage_obstacle_mode = QStringLiteral("manual");
        cache.coverage_roi_drawing_active = false;
        cache.coverage_drawing_active = false;
        persistParameters();
        updateButtonsAndStatus();
    });
    connect(btn_coverage_detect_, &QPushButton::clicked, this, [this, cancel_coverage_selection]() {
        SessionCache& cache = activeSession();
        cancel_coverage_selection();
        cache.coverage_drawing_active = false;
        persistParameters();
        startDetectObstacles();
    });
    connect(btn_coverage_clear_obstacles_, &QPushButton::clicked, this, [this, cancel_coverage_selection]() {
        SessionCache& cache = activeSession();
        cancel_coverage_selection();
        cache.coverage_drawing_active = false;
        cache.coverage_obstacles.clear();
        cache.coverage_obstacles_detected = false;
        invalidateCoverageResult(
            QStringLiteral("Obstacle overlays cleared. Generate coverage paths again to refresh the preview."));
        updatePreview();
        updateButtonsAndStatus();
    });
    connect(btn_coverage_draw_rectangle_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_drawing_tool = QStringLiteral("rectangle");
        persistParameters();
        updateCoveragePlanningUi();
    });
    connect(btn_coverage_draw_polygon_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_drawing_tool = QStringLiteral("polygon");
        persistParameters();
        updateCoveragePlanningUi();
    });
    connect(btn_coverage_draw_circle_, &QPushButton::clicked, this, [this]() {
        SessionCache& cache = activeSession();
        cache.coverage_drawing_tool = QStringLiteral("circle");
        persistParameters();
        updateCoveragePlanningUi();
    });
    connect(btn_coverage_draw_toggle_, &QPushButton::clicked, this, [this, cancel_coverage_selection]() {
        SessionCache& cache = activeSession();
        if (!cache.hull_complete || cache.hull_polygon.empty()) {
            setInlineStatus(QStringLiteral("Compute and project the hull before drawing obstacles."),
                            QStringLiteral("#F59E0B"));
            updateButtonsAndStatus();
            return;
        }

        if (cache.coverage_drawing_active) {
            cancel_coverage_selection();
            cache.coverage_drawing_active = false;
            setInlineStatus(QStringLiteral("Manual obstacle drawing cancelled."),
                            QStringLiteral("#71717B"));
            updateButtonsAndStatus();
            return;
        }

        cancel_coverage_selection();
        cache.coverage_roi_drawing_active = false;
        cache.coverage_drawing_active = true;
        if (plot_) {
            if (cache.coverage_drawing_tool == QStringLiteral("polygon")) {
                plot_->startObstacleSelection();
            } else {
                plot_->startRectangleMode();
            }
            plot_->setFocus();
        }
        setInlineStatus(QStringLiteral("Define the obstacle shape on the map preview."),
                        QStringLiteral("#71717B"));
        updateButtonsAndStatus();
    });
    connect(btn_coverage_generate_, &QPushButton::clicked, this, [this]() { startGenerateCoverage(); });

    // Scan-stage 1 Hz tick timer for elapsed time / ETA recalculation.
    scan_tick_timer_ = new QTimer(this);
    scan_tick_timer_->setInterval(1000);
    connect(scan_tick_timer_, &QTimer::timeout, this, &PlannerScreen::onScanTick);
    scan_manual_teleop_timer_ = new QTimer(this);
    scan_manual_teleop_timer_->setInterval(100);
    connect(scan_manual_teleop_timer_, &QTimer::timeout,
            this, &PlannerScreen::onScanManualTeleopTick);
    scan_camera_restart_timer_ = new QTimer(this);
    scan_camera_restart_timer_->setSingleShot(true);
    scan_camera_restart_timer_->setInterval(400);
    connect(scan_camera_restart_timer_, &QTimer::timeout, this, [this]() {
        if (current_step_ != PlannerStep::Scan || !scan_camera_view_ ||
            !scan_camera_stream_requested_ || scan_camera_view_->isPlaying()) {
            return;
        }
        scan_camera_view_->startStream(5600);
    });
    scan_quality_watcher_ = new QFutureWatcher<double>(this);
    connect(scan_quality_watcher_, &QFutureWatcher<double>::finished, this, [this]() {
        if (!scan_quality_watcher_) {
            return;
        }
        SessionCache& cache = activeSession();
        const int idx = scan_quality_segment_index_;
        if (idx < 0 || idx >= static_cast<int>(cache.scan_segments.size())) {
            return;
        }
        const double sample_quality =
            std::clamp(scan_quality_watcher_->result(), 0.0, 100.0);
        auto& seg = cache.scan_segments[static_cast<size_t>(idx)];
        if (seg.completed || sample_quality <= 0.0) {
            return;
        }
        // Keep this as a running EWMA so 5s samples accumulate smoothly.
        seg.quality_pct = seg.quality_pct <= 0.0
                              ? sample_quality
                              : (0.7 * seg.quality_pct + 0.3 * sample_quality);
        recomputeScanAggregateStats();
        updateScanRunUi();
    });

    installEventFilter(this);
    if (plot_) {
        plot_->installEventFilter(this);
    }
    if (scan_camera_view_) {
        scan_camera_view_->installEventFilter(this);
    }

    applyStyle();
    applySessionToUi();
}

// ============================================================================
// Stage 4 (Scan execution) — UI builders
// ============================================================================

QWidget* PlannerScreen::buildScanPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    page->setAttribute(Qt::WA_StyledBackground, true);
    page->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* page_layout = new QVBoxLayout(page);
    page_layout->setContentsMargins(0, 0, 0, 0);
    page_layout->setSpacing(0);

    auto* scroll = new QScrollArea(page);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("background: transparent;"));
    AutoHideScrollBar::install(scroll, dark_mode_);
    page_layout->addWidget(scroll, 1);

    auto* content = new QWidget(scroll);
    content->setAttribute(Qt::WA_StyledBackground, true);
    content->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* content_layout = new QVBoxLayout(content);
    // Figma 137:201 rail paddings: left 16, right 17, top/bottom 16.
    content_layout->setContentsMargins(16, 16, 17, 16);
    content_layout->setSpacing(16);

    scan_current_segment_card_ = buildScanCurrentSegmentCard(content);
    content_layout->addWidget(scan_current_segment_card_);
    content_layout->addWidget(buildScanOverallProgressCard(content));
    content_layout->addWidget(buildScanTelemetryCard(content));
    content_layout->addStretch(1);

    scroll->setWidget(content);
    return page;
}

namespace {

// Header row for one of the scan-stage cards: small accent icon + title.
QWidget* makeScanCardHeader(QWidget* parent, const QString& icon_path, const QString& title) {
    auto* row = new QWidget(parent);
    row->setFixedHeight(24);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* icon = makeIconLabel(row, icon_path, 16, QStringLiteral("#00D492"));
    layout->addWidget(icon, 0, Qt::AlignVCenter);
    auto* title_label = makeTextLabel(
        row,
        title,
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; color: #FFFFFF;"));
    layout->addWidget(title_label, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    return row;
}

QWidget* makeScanCardShell(QWidget* parent) {
    auto* card = new QWidget(parent);
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setStyleSheet(QStringLiteral(
        "background: #27272A; border: none; border-radius: 10px;"));
    return card;
}

QProgressBar* makeScanProgressBar(QWidget* parent, const QString& chunk_color) {
    auto* bar = new QProgressBar(parent);
    bar->setRange(0, 1000);  // tenths of a percent for smooth animation
    bar->setValue(0);
    bar->setTextVisible(false);
    bar->setFixedHeight(8);
    bar->setStyleSheet(
        QStringLiteral(
            "QProgressBar { background: #3F3F47; border: none; border-radius: 999px; }"
            "QProgressBar::chunk { background: %1; border-radius: 999px; }")
            .arg(chunk_color));
    return bar;
}

QWidget* makeKeyValueRow(QWidget* parent,
                         const QString& key,
                         const QString& initial_value,
                         QLabel** out_value) {
    auto* row = new QWidget(parent);
    row->setFixedHeight(24);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* key_label = makeTextLabel(
        row,
        key,
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: #9F9FA9;"));
    layout->addWidget(key_label, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    auto* value_label = makeTextLabel(
        row,
        initial_value,
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 400; "
                       "color: #D4D4D8;"));
    value_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(value_label, 0, Qt::AlignVCenter);
    if (out_value) {
        *out_value = value_label;
    }
    return row;
}

}  // namespace

QWidget* PlannerScreen::buildScanCurrentSegmentCard(QWidget* parent) {
    auto* card = makeScanCardShell(parent);
    card->setFixedHeight(236);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    layout->addWidget(makeScanCardHeader(card,
                                         QStringLiteral(":/assets/missionplanner/scan_card_current.svg"),
                                         QStringLiteral("Current Segment")));

    auto* content = new QWidget(card);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(12);

    auto make_stack_metric = [&](const QString& label, const QString& initial, QLabel*& out_value) {
        auto* section = new QWidget(content);
        section->setFixedHeight(48);
        auto* section_layout = new QVBoxLayout(section);
        section_layout->setContentsMargins(0, 0, 0, 0);
        section_layout->setSpacing(0);
        section_layout->addWidget(makeTextLabel(
            section,
            label,
            QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 400; "
                           "color: #9F9FA9;")));
        out_value = makeTextLabel(
            section,
            initial,
            QStringLiteral("font-family: 'Liberation Mono'; font-size: 16px; font-weight: 400; "
                           "color: #FFFFFF;"));
        section_layout->addWidget(out_value);
        return section;
    };
    content_layout->addWidget(make_stack_metric(QStringLiteral("Active"),
                                                QStringLiteral("--"),
                                                lbl_scan_active_segment_));

    auto* progress_section = new QWidget(content);
    progress_section->setFixedHeight(48);
    auto* progress_section_layout = new QVBoxLayout(progress_section);
    progress_section_layout->setContentsMargins(0, 0, 0, 0);
    progress_section_layout->setSpacing(4);
    auto* progress_top = new QWidget(progress_section);
    progress_top->setFixedHeight(24);
    auto* progress_top_layout = new QHBoxLayout(progress_top);
    progress_top_layout->setContentsMargins(0, 0, 0, 0);
    progress_top_layout->setSpacing(0);
    progress_top_layout->addWidget(makeTextLabel(
        progress_top,
        QStringLiteral("Progress"),
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 400; color: #9F9FA9;")));
    progress_section_layout->addWidget(progress_top);

    auto* progress_row = new QWidget(progress_section);
    progress_row->setFixedHeight(20);
    auto* progress_row_layout = new QHBoxLayout(progress_row);
    progress_row_layout->setContentsMargins(0, 0, 0, 0);
    progress_row_layout->setSpacing(8);
    bar_scan_active_progress_ =
        makeScanProgressBar(progress_row, QStringLiteral("#00BC7D"));
    progress_row_layout->addWidget(bar_scan_active_progress_, 1);
    lbl_scan_active_progress_value_ = makeTextLabel(
        progress_row,
        QStringLiteral("0%"),
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 400; "
                       "color: #FFFFFF;"));
    progress_row_layout->addWidget(lbl_scan_active_progress_value_, 0, Qt::AlignVCenter);
    progress_section_layout->addWidget(progress_row);
    content_layout->addWidget(progress_section);

    content_layout->addWidget(make_stack_metric(QStringLiteral("Quality Score"),
                                                QStringLiteral("--%"),
                                                lbl_scan_active_quality_));
    layout->addWidget(content);
    return card;
}

QWidget* PlannerScreen::buildScanOverallProgressCard(QWidget* parent) {
    auto* card = makeScanCardShell(parent);
    card->setFixedHeight(236);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    layout->addWidget(makeScanCardHeader(card,
                                         QStringLiteral(":/assets/missionplanner/scan_card_progress.svg"),
                                         QStringLiteral("Overall Progress")));

    auto* content = new QWidget(card);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(12);

    auto add_progress_row = [&](const QString& key,
                                QProgressBar*& out_bar,
                                QLabel*& out_value,
                                const QString& chunk_color) {
        auto* section = new QWidget(content);
        section->setFixedHeight(48);
        auto* section_layout = new QVBoxLayout(section);
        section_layout->setContentsMargins(0, 0, 0, 0);
        section_layout->setSpacing(4);
        section_layout->addWidget(makeTextLabel(
            section,
            key,
            QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 400; "
                           "color: #9F9FA9;")));

        auto* row = new QWidget(section);
        row->setFixedHeight(20);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(8);
        out_bar = makeScanProgressBar(row, chunk_color);
        row_layout->addWidget(out_bar, 1);
        out_value = makeTextLabel(
            row,
            QStringLiteral("0.0%"),
            QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 400; "
                           "color: #FFFFFF;"));
        row_layout->addWidget(out_value, 0, Qt::AlignVCenter);
        section_layout->addWidget(row);
        content_layout->addWidget(section);
    };

    add_progress_row(QStringLiteral("Total Coverage"),
                     bar_scan_total_coverage_,
                     lbl_scan_total_coverage_value_,
                     QStringLiteral("#00BC7D"));
    add_progress_row(QStringLiteral("Scan Quality"),
                     bar_scan_total_quality_,
                     lbl_scan_total_quality_value_,
                     QStringLiteral("#3B82F6"));

    auto* time_section = new QWidget(content);
    time_section->setFixedHeight(48);
    auto* time_layout = new QVBoxLayout(time_section);
    time_layout->setContentsMargins(0, 0, 0, 0);
    time_layout->setSpacing(0);
    time_layout->addWidget(makeTextLabel(
        time_section,
        QStringLiteral("Scan Time"),
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 400; color: #9F9FA9;")));
    lbl_scan_time_value_ = makeTextLabel(
        time_section,
        QStringLiteral("00:00"),
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 16px; font-weight: 400; color: #FFFFFF;"));
    time_layout->addWidget(lbl_scan_time_value_);
    content_layout->addWidget(time_section);
    layout->addWidget(content);
    return card;
}

QWidget* PlannerScreen::buildScanTelemetryCard(QWidget* parent) {
    auto* card = makeScanCardShell(parent);
    card->setFixedHeight(252);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    layout->addWidget(makeScanCardHeader(card,
                                         QStringLiteral(":/assets/missionplanner/scan_card_telemetry.svg"),
                                         QStringLiteral("Telemetry")));

    auto* content = new QWidget(card);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(12);

    auto* speed_section = new QWidget(content);
    speed_section->setFixedHeight(48);
    auto* speed_layout = new QVBoxLayout(speed_section);
    speed_layout->setContentsMargins(0, 0, 0, 0);
    speed_layout->setSpacing(0);
    speed_layout->addWidget(makeTextLabel(
        speed_section,
        QStringLiteral("Speed"),
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 400; color: #9F9FA9;")));
    lbl_scan_telemetry_speed_ = makeTextLabel(
        speed_section,
        QStringLiteral("0.00 m/s"),
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 16px; font-weight: 400; color: #FFFFFF;"));
    speed_layout->addWidget(lbl_scan_telemetry_speed_);
    content_layout->addWidget(speed_section);

    auto* pos_section = new QWidget(content);
    pos_section->setFixedHeight(64);
    auto* pos_layout = new QVBoxLayout(pos_section);
    pos_layout->setContentsMargins(0, 0, 0, 0);
    pos_layout->setSpacing(0);
    pos_layout->addWidget(makeTextLabel(
        pos_section,
        QStringLiteral("Position"),
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 400; color: #9F9FA9;")));
    auto* pos_values = new QWidget(pos_section);
    auto* pos_values_layout = new QVBoxLayout(pos_values);
    pos_values_layout->setContentsMargins(0, 0, 0, 0);
    pos_values_layout->setSpacing(0);
    auto* row_x = new QWidget(pos_values);
    auto* row_x_layout = new QHBoxLayout(row_x);
    row_x_layout->setContentsMargins(0, 0, 0, 0);
    row_x_layout->setSpacing(6);
    row_x_layout->addWidget(makeTextLabel(
        row_x,
        QStringLiteral("X:"),
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 16px; font-weight: 400; color: #A1A1AA;")));
    lbl_scan_telemetry_pos_x_ = makeTextLabel(
        row_x,
        QStringLiteral("0.00 m"),
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 16px; font-weight: 400; color: #FFFFFF;"));
    row_x_layout->addWidget(lbl_scan_telemetry_pos_x_, 1, Qt::AlignLeft | Qt::AlignVCenter);
    pos_values_layout->addWidget(row_x);
    auto* row_y = new QWidget(pos_values);
    auto* row_y_layout = new QHBoxLayout(row_y);
    row_y_layout->setContentsMargins(0, 0, 0, 0);
    row_y_layout->setSpacing(6);
    row_y_layout->addWidget(makeTextLabel(
        row_y,
        QStringLiteral("Y:"),
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 16px; font-weight: 400; color: #A1A1AA;")));
    lbl_scan_telemetry_pos_y_ = makeTextLabel(
        row_y,
        QStringLiteral("0.00 m"),
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 16px; font-weight: 400; color: #FFFFFF;"));
    row_y_layout->addWidget(lbl_scan_telemetry_pos_y_, 1, Qt::AlignLeft | Qt::AlignVCenter);
    pos_values_layout->addWidget(row_y);
    pos_layout->addWidget(pos_values);
    content_layout->addWidget(pos_section);

    auto* heading_section = new QWidget(content);
    heading_section->setFixedHeight(48);
    auto* heading_layout = new QVBoxLayout(heading_section);
    heading_layout->setContentsMargins(0, 0, 0, 0);
    heading_layout->setSpacing(0);
    heading_layout->addWidget(makeTextLabel(
        heading_section,
        QStringLiteral("Heading"),
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 400; color: #9F9FA9;")));
    lbl_scan_telemetry_heading_ = makeTextLabel(
        heading_section,
        QStringLiteral("0.0°"),
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 16px; font-weight: 400; color: #FFFFFF;"));
    heading_layout->addWidget(lbl_scan_telemetry_heading_);
    content_layout->addWidget(heading_section);
    layout->addWidget(content);
    return card;
}

QWidget* PlannerScreen::buildScanManualOverrideCard(QWidget* parent) {
    auto* card = makeScanCardShell(parent);
    card->setFixedHeight(320);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    layout->addWidget(makeScanCardHeader(card,
                                         QStringLiteral(":/assets/missionplanner/scan_card_telemetry.svg"),
                                         QStringLiteral("Manual Override")));

    auto* content = new QWidget(card);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(10);

    // FPVCameraView mirrors Exploration's known-good FPV layout (no HUD overlay).
    // It owns its own placeholder/video stack, so the surrounding card chrome no
    // longer wraps multiple styled-background QWidgets that triggered repaint
    // storms in the Planner Scan stage.
    scan_camera_view_ = new FPVCameraView(content);
    scan_camera_view_->setObjectName("PlannerScanFpvCamera");
    scan_camera_view_->setCursor(Qt::PointingHandCursor);
    scan_camera_view_->setFixedHeight(196);
    scan_camera_view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    scan_camera_view_->setPlaceholderText(QStringLiteral("Camera View"),
                                          QStringLiteral("Click to enter manual teleop"));
    auto* stream_widget = scan_camera_view_->streamWidget();
    if (stream_widget) {
        stream_widget->setMinimumSize(280, 157);
    }
    connect(scan_camera_view_, &FPVCameraView::firstFrameReady, this, [this]() {
        if (scan_camera_restart_timer_ && scan_camera_restart_timer_->isActive()) {
            scan_camera_restart_timer_->stop();
        }
    });
    connect(scan_camera_view_, &FPVCameraView::streamStopped, this, [this]() {
        scheduleScanCameraRestart(QStringLiteral("streamStopped"));
    });
    connect(scan_camera_view_, &FPVCameraView::streamError, this,
            [this](const QString&) {
                scheduleScanCameraRestart(QStringLiteral("streamError"));
            });
    content_layout->addWidget(scan_camera_view_);

    lbl_scan_manual_override_state_ = makeTextLabel(
        content,
        QStringLiteral("Manual Override: Inactive"),
        QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 600; color: #9F9FA9;"));
    content_layout->addWidget(lbl_scan_manual_override_state_);

    auto* hint = makeTextLabel(
        content,
        QStringLiteral("Click camera for manual teleop (W/A/S/D, E/Q, O, 0/9). "
                       "Click map to return to autonomy."),
        QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: #71717B;"));
    hint->setWordWrap(true);
    content_layout->addWidget(hint);

    layout->addWidget(content);
    return card;
}

QWidget* PlannerScreen::buildScanRightRail(QWidget* parent) {
    auto* rail = new QWidget(parent);
    rail->setFixedWidth(kPlannerRightRailWidth);
    rail->setAttribute(Qt::WA_StyledBackground, true);
    rail->setStyleSheet(QStringLiteral("background: #18181B; border-left: 1px solid #27272A;"));
    auto* layout = new QVBoxLayout(rail);
    layout->setContentsMargins(17, 16, 16, 16);
    layout->setSpacing(16);
    // Both cards stack at the top of the rail; trailing stretch absorbs any
    // remaining vertical space. Stretch factor 1 on Segment Status was wrong:
    // it dragged Scan Statistics to the bottom of the rail when the segment
    // list was empty, leaving a giant void in the middle.
    layout->addWidget(buildScanManualOverrideCard(rail));
    scan_segment_status_card_ = buildScanSegmentStatusCard(rail);
    layout->addWidget(scan_segment_status_card_);
    layout->addWidget(buildScanStatisticsCard(rail));
    layout->addStretch(1);
    return rail;
}

QWidget* PlannerScreen::buildScanSegmentStatusCard(QWidget* parent) {
    auto* card = makeScanCardShell(parent);
    card->setFixedHeight(240);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(card);
    header->setObjectName(QStringLiteral("segmentStatusHeader"));
    header->setFixedHeight(40);
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setStyleSheet(QStringLiteral(
        "QWidget#segmentStatusHeader { background: #3F3F47; border-radius: 10px 10px 0 0; }"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(12, 8, 12, 8);
    header_layout->setSpacing(8);
    auto* title = makeTextLabel(
        header,
        QStringLiteral("Segment Status"),
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; color: #FFFFFF;"));
    header_layout->addWidget(title, 1, Qt::AlignVCenter);
    auto* close_hint = makeTextLabel(
        header,
        QStringLiteral("×"),
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; color: #00D492;"));
    close_hint->setFixedSize(16, 16);
    close_hint->setAlignment(Qt::AlignCenter);
    header_layout->addWidget(close_hint, 0, Qt::AlignVCenter);
    layout->addWidget(header);

    list_scan_segment_status_ = new QListWidget(card);
    list_scan_segment_status_->setSelectionMode(QAbstractItemView::NoSelection);
    list_scan_segment_status_->setFocusPolicy(Qt::NoFocus);
    // QAbstractScrollArea draws its own QFrame border around the viewport
    // even when the QListWidget QSS sets `border: none;`. That frame was the
    // thin "residual line" visible at the bottom of the Segment Status card.
    list_scan_segment_status_->setFrameShape(QFrame::NoFrame);
    list_scan_segment_status_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    list_scan_segment_status_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_scan_segment_status_->setAttribute(Qt::WA_StyledBackground, true);
    list_scan_segment_status_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_scan_segment_status_->setSpacing(8);
    list_scan_segment_status_->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; padding: 12px 12px 12px 12px; "
        "  color: #E4E4E7; outline: none; }"
        "QListWidget::item { border: none; background: transparent; margin: 0; padding: 0; }"));
    AutoHideScrollBar::install(list_scan_segment_status_, dark_mode_);
    list_scan_segment_status_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(list_scan_segment_status_, 1);
    return card;
}

QWidget* PlannerScreen::buildScanStatisticsCard(QWidget* parent) {
    auto* card = makeScanCardShell(parent);
    card->setFixedHeight(170);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    layout->addWidget(makeScanCardHeader(card,
                                         QStringLiteral(":/assets/missionplanner/scan_card_stats.svg"),
                                         QStringLiteral("Scan Statistics")));

    layout->addWidget(makeKeyValueRow(card,
                                      QStringLiteral("Distance"),
                                      QStringLiteral("0.0 m"),
                                      &lbl_scan_stats_distance_));
    layout->addWidget(makeKeyValueRow(card,
                                      QStringLiteral("Points"),
                                      QStringLiteral("0.00M"),
                                      &lbl_scan_stats_points_));
    layout->addWidget(makeKeyValueRow(card,
                                      QStringLiteral("Avg Quality"),
                                      QStringLiteral("0.0%"),
                                      &lbl_scan_stats_avg_quality_));
    layout->addWidget(makeKeyValueRow(card,
                                      QStringLiteral("Est. Time Left"),
                                      QStringLiteral("--:--"),
                                      &lbl_scan_stats_eta_));
    return card;
}

QWidget* PlannerScreen::buildScanCenterControlBar(QWidget* parent) {
    auto* bar = new QWidget(parent);
    bar->setFixedHeight(81);
    bar->setAttribute(Qt::WA_StyledBackground, true);
    // Transparent — the bar should look like a continuation of the page
    // background, with the buttons floating below the map card. Matches Figma.
    bar->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 17, 0, 16);
    layout->setSpacing(12);

    btn_scan_start_pause_ = new QPushButton(bar);
    btn_scan_start_pause_->setCursor(Qt::PointingHandCursor);
    btn_scan_start_pause_->setFlat(true);
    btn_scan_start_pause_->setFixedHeight(48);
    btn_scan_start_pause_->setMinimumWidth(154);
    btn_scan_start_pause_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    btn_scan_start_pause_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #00BC7D; border: none; border-radius: 10px; }"
        "QPushButton:hover { background: #0ACB8B; }"
        "QPushButton:disabled { background: rgba(0,188,125,0.4); color: rgba(255,255,255,0.7); }"));
    {
        auto* btn_layout = new QHBoxLayout(btn_scan_start_pause_);
        btn_layout->setContentsMargins(16, 0, 16, 0);
        btn_layout->setSpacing(8);
        btn_layout->addStretch(1);
        lbl_scan_start_pause_icon_ = makeIconLabel(
            btn_scan_start_pause_,
            QStringLiteral(":/assets/missionplanner/scan_play.svg"),
            20,
            QStringLiteral("#FFFFFF"));
        btn_layout->addWidget(lbl_scan_start_pause_icon_);
        lbl_scan_start_pause_text_ = makeTextLabel(
            btn_scan_start_pause_,
            QStringLiteral("Start Scan"),
            QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; color: #FFFFFF;"));
        btn_layout->addWidget(lbl_scan_start_pause_text_);
        btn_layout->addStretch(1);
    }
    applyDropShadow(btn_scan_start_pause_, 16, 4, QColor(0, 188, 125, 64));
    connect(btn_scan_start_pause_, &QPushButton::clicked,
            this, &PlannerScreen::onScanStartPauseClicked);
    layout->addWidget(btn_scan_start_pause_);

    lbl_scan_run_summary_ = makeTextLabel(
        bar,
        QStringLiteral("00:00 \u2022 0/0 segments"),
        QStringLiteral("font-family: 'Liberation Mono'; font-size: 14px; font-weight: 400; "
                       "color: #9F9FA9;"));
    // Hugs the start button on the left; a single stretch pushes Emergency
    // Stop to the right edge of the map card. Matches Figma.
    layout->addWidget(lbl_scan_run_summary_, 0, Qt::AlignVCenter);

    layout->addStretch(1);

    // Cancel Scan — Figma node 162:233. Amber pill (166x48, 10px radius) that
    // sits 12px to the left of Emergency Stop. Lives next to E-Stop because
    // both are "abort"-flavoured controls; the gating is intentionally
    // narrower (estop latched OR manual override engaged) so the operator
    // can't fat-finger a destructive delete during a normal autonomous run.
    btn_scan_cancel_ = new QPushButton(bar);
    btn_scan_cancel_->setCursor(Qt::PointingHandCursor);
    btn_scan_cancel_->setFlat(true);
    btn_scan_cancel_->setFixedHeight(48);
    btn_scan_cancel_->setMinimumWidth(166);
    btn_scan_cancel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    btn_scan_cancel_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #FE9A00; border: none; border-radius: 10px; }"
        "QPushButton:hover { background: #FFAA22; }"
        "QPushButton:disabled { background: rgba(82,82,91,0.4); color: rgba(228,228,231,0.5); }"));
    {
        auto* btn_layout = new QHBoxLayout(btn_scan_cancel_);
        btn_layout->setContentsMargins(16, 0, 16, 0);
        btn_layout->setSpacing(8);
        btn_layout->addStretch(1);
        lbl_scan_cancel_icon_ = makeIconLabel(
            btn_scan_cancel_,
            QStringLiteral(":/assets/missionplanner/scan_cancel.svg"),
            20,
            QStringLiteral("#FFFFFF"));
        btn_layout->addWidget(lbl_scan_cancel_icon_);
        lbl_scan_cancel_text_ = makeTextLabel(
            btn_scan_cancel_,
            QStringLiteral("Cancel Scan"),
            QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; color: #FFFFFF;"));
        btn_layout->addWidget(lbl_scan_cancel_text_);
        btn_layout->addStretch(1);
    }
    btn_scan_cancel_->setEnabled(false);
    // Dual-mode dispatch: same button hosts Cancel Scan (mid-run abort,
    // amber) and Discard Scan (post-Completed delete, dark red). The slot
    // branches on run_state == Completed → Discard, else → Cancel.
    connect(btn_scan_cancel_, &QPushButton::clicked, this, [this]() {
        const SessionCache* cache = activeSessionPtr();
        const bool completed =
            cache && cache->scan_run_state == ScanRunState::Completed;
        if (completed) {
            onScanDiscardClicked();
        } else {
            onScanCancelClicked();
        }
    });
    layout->addWidget(btn_scan_cancel_);

    btn_scan_emergency_stop_ = new QPushButton(bar);
    btn_scan_emergency_stop_->setCursor(Qt::PointingHandCursor);
    btn_scan_emergency_stop_->setFlat(true);
    btn_scan_emergency_stop_->setFixedHeight(48);
    btn_scan_emergency_stop_->setMinimumWidth(200);
    btn_scan_emergency_stop_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    btn_scan_emergency_stop_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #DC2626; border: none; border-radius: 10px; }"
        "QPushButton:hover { background: #EF4444; }"
        "QPushButton:disabled { background: rgba(82,82,91,0.4); color: rgba(228,228,231,0.5); }"));
    {
        auto* btn_layout = new QHBoxLayout(btn_scan_emergency_stop_);
        btn_layout->setContentsMargins(16, 0, 16, 0);
        btn_layout->setSpacing(8);
        btn_layout->addStretch(1);
        lbl_scan_emergency_stop_icon_ = makeIconLabel(
            btn_scan_emergency_stop_,
            QStringLiteral(":/assets/missionplanner/scan_emergency_stop.svg"),
            20,
            QStringLiteral("#FFFFFF"));
        btn_layout->addWidget(lbl_scan_emergency_stop_icon_);
        lbl_scan_emergency_stop_text_ = makeTextLabel(
            btn_scan_emergency_stop_,
            QStringLiteral("Emergency Stop"),
            QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; color: #FFFFFF;"));
        btn_layout->addWidget(lbl_scan_emergency_stop_text_);
        btn_layout->addStretch(1);
    }
    btn_scan_emergency_stop_->setEnabled(false);
    connect(btn_scan_emergency_stop_, &QPushButton::clicked,
            this, &PlannerScreen::onScanEmergencyStopClicked);
    layout->addWidget(btn_scan_emergency_stop_);

    return bar;
}

QWidget* PlannerScreen::buildScanStatusPill(QWidget* parent) {
    // Custom-painted rounded fill (see RoundedFillWidget). QSS-based
    // border-radius left visible corner artifacts on the dark map because
    // Qt 5 clips the background with a 1-bit mask; QPainter::Antialiasing
    // gives 8-bit alpha edges that fade cleanly into the parent colour.
    // Figma node 139:823 spec: zinc-900 fill, 10px radius. We render the
    // fill fully opaque (Figma uses 0.9 alpha, but transparency makes the
    // contrast issue worse, not better, on this background).
    auto* pill = new RoundedFillWidget(QColor(0x27, 0x27, 0x2A), 10.0, parent);
    pill->setObjectName(QStringLiteral("scanStatusPill"));
    pill->setFixedHeight(40);
    // Preferred + Fixed so the pill keeps its sizeHint width when its parent
    // (top_right_column / overlay) calls adjustSize. Maximum was collapsing the
    // pill down to its dot icon's natural width.
    pill->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(pill);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(8);
    lbl_scan_status_pill_dot_ = makeIconLabel(
        pill,
        QStringLiteral(":/assets/missionplanner/status_dot.svg"),
        8,
        QStringLiteral("#71717B"));
    layout->addWidget(lbl_scan_status_pill_dot_, 0, Qt::AlignVCenter);
    lbl_scan_status_pill_text_ = makeTextLabel(
        pill,
        QStringLiteral("Ready"),
        QStringLiteral("font-family: 'Arimo'; font-size: 16px; font-weight: 700; color: #D4D4D8;"));
    layout->addWidget(lbl_scan_status_pill_text_, 0, Qt::AlignVCenter);
    // Figma applies a soft drop shadow, but on this dark map background the
    // shadow's blur extends past the rounded clip path and renders as faint
    // dark "smudges" at each corner instead of as elevation. Skip it — the
    // pill is plenty legible from the fill contrast alone.
    pill->adjustSize();
    return pill;
}

QWidget* PlannerScreen::buildScanLegendChip(QWidget* parent) {
    auto* chip = new QWidget(parent);
    chip->setObjectName(QStringLiteral("scanLegendChip"));
    chip->setFixedHeight(84);
    chip->setMinimumWidth(128);
    chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    chip->setAttribute(Qt::WA_StyledBackground, true);
    chip->setStyleSheet(QStringLiteral(
        "QWidget#scanLegendChip { background: rgba(24,24,27,0.9); "
        "border: 1px solid #27272A; border-radius: 10px; }"));
    auto* layout = new QVBoxLayout(chip);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(4);

    auto add_row = [&](const QString& dot_color, const QString& initial_text, QLabel*& out_label) {
        auto* row = new QWidget(chip);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(8);
        auto* dot = makeIconLabel(row,
                                  QStringLiteral(":/assets/missionplanner/status_dot.svg"),
                                  8,
                                  dot_color);
        row_layout->addWidget(dot, 0, Qt::AlignVCenter);
        out_label = makeTextLabel(
            row,
            initial_text,
            QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; "
                           "color: #D4D4D8;"));
        row_layout->addWidget(out_label, 0, Qt::AlignVCenter);
        row_layout->addStretch(1);
        layout->addWidget(row);
    };
    add_row(QStringLiteral("#00D492"),
            QStringLiteral("Completed: 0"),
            lbl_scan_legend_completed_);
    add_row(QStringLiteral("#3B82F6"),
            QStringLiteral("Active: 0"),
            lbl_scan_legend_active_);
    add_row(QStringLiteral("#71717B"),
            QStringLiteral("Pending: 0"),
            lbl_scan_legend_pending_);
    return chip;
}

// ============================================================================
// Stage 4 (Scan execution) — state updates
// ============================================================================

void PlannerScreen::enterScanStage() {
    // INVARIANT: every entry into the Scan substep MUST go through this
    // function. The three discard flags
    // (scan_discard_in_flight_ / scan_discarded_ / scan_discard_failed_)
    // and the cancel/estop/manual-override latches below are PlannerScreen
    // members rather than SessionCache fields, so they are reset only
    // here and at the bottom of notifyScanCancelled / notifyScanDiscarded.
    // If you add a new navigation path into the Scan substep that does NOT
    // go through this function, call it explicitly or you will latch a
    // stale "Discarded" button (or a stale E-Stop pill) into the next run.
    SessionCache& cache = activeSession();
    setScanManualOverride(false);
    scan_manual_override_engaged_once_ = false;
    scan_manual_resume_after_override_ = false;
    scan_pause_explicit_ = false;
    scan_camera_stream_requested_ = false;
    cache.scan_run_state = ScanRunState::Idle;
    cache.scan_elapsed_ms = 0;
    cache.scan_distance_traveled_m = 0.0;
    cache.scan_total_coverage_pct = 0.0;
    cache.scan_avg_quality_pct = 0.0;
    cache.scan_total_points_m = 0.0;
    cache.scan_estimated_ms_left = -1;
    // Auto-split when the operator skipped the Splitting stage entirely.
    // Treats the whole planned path as a single "complete scan" segment so
    // the Scan stage has something to execute. Segment-aware UI then
    // collapses to a single-mission view (see updateScanRunUi).
    if (cache.scan_segments.empty() && cache.planned_path.size() >= 2) {
        rebuildScanSegments();
        for (auto& seg : cache.scan_segments) {
            seg.selected = true;
        }
    }
    cache.scan_active_segment_index = firstPendingSelectedSegmentIndex();
    scan_active_segment_path_hint_ = 0;
    scan_estop_latched_ = false;
    scan_dc_save_in_flight_ = false;
    scan_cancel_in_flight_ = false;
    scan_discard_in_flight_ = false;
    scan_discarded_ = false;
    scan_discard_failed_ = false;
    scan_pending_next_segment_idx_ = -1;
    scan_last_quality_sample_ms_ = 0;
    scan_quality_segment_index_ = -1;
    if (scan_tick_timer_) {
        scan_tick_timer_->stop();
    }
    last_telemetry_valid_ = false;
    recomputeScanAggregateStats();
    navigateToStep(PlannerStep::Scan);
    setFocus();
}

int PlannerScreen::firstPendingSelectedSegmentIndex(int start_index) const {
    const SessionCache* cache = activeSessionPtr();
    if (!cache) {
        return -1;
    }
    const int begin = std::max(0, start_index);
    for (int i = begin; i < static_cast<int>(cache->scan_segments.size()); ++i) {
        const auto& seg = cache->scan_segments[static_cast<size_t>(i)];
        if (seg.selected && !seg.completed) {
            return i;
        }
    }
    return -1;
}

bool PlannerScreen::publishSegmentForExecution(int segment_index) {
    SessionCache& cache = activeSession();
    if (segment_index < 0 || segment_index >= static_cast<int>(cache.scan_segments.size())) {
        return false;
    }
    PathStateList path =
        dedupeScanPathStates(cache.scan_segments[static_cast<size_t>(segment_index)].path);
    if (path.size() < 2) {
        return false;
    }

    // Publish as [x, y, dc] triples so controller-side AUTO-DC stays explicit.
    // Per-segment flow is handled by this UI: one segment path at a time.
    std::vector<double> payload;
    payload.reserve(path.size() * 3);
    for (const auto& st : path) {
        payload.push_back(st.point.x);
        payload.push_back(st.point.y);
        payload.push_back(1.0);
    }
    emit publishScanSegmentsRequested(payload);
    return true;
}

void PlannerScreen::startSegmentExecution(int segment_index, bool resume_action) {
    SessionCache& cache = activeSession();
    setScanManualOverride(false);
    scan_manual_resume_after_override_ = false;
    scan_pause_explicit_ = false;
    if (!publishSegmentForExecution(segment_index)) {
        BdrMessageBox::warning(this,
                               QStringLiteral("Segment publish failed"),
                               QStringLiteral("Could not publish waypoints for the active segment."));
        return;
    }

    cache.scan_waypoints_published = true;
    cache.scan_active_segment_index = segment_index;
    cache.scan_run_state = ScanRunState::Running;
    scan_active_segment_path_hint_ = 0;
    scan_last_quality_sample_ms_ = QDateTime::currentMSecsSinceEpoch();
    emit startScanSegmentsRequested(cache.scan_progression_mode);
    if (resume_action) {
        emit scanResumeRequested();
    } else {
        // Pass the configured cruise speed so AppShellWindow can push it to
        // /mpc_accel_controller's `max_linear_velocity` ROS param BEFORE
        // arming autonomy. Resume reuses the param already set on the
        // initial start.
        emit scanStartRequested(cache.coverage_scan_speed_mps);
    }
    if (scan_tick_timer_ && !scan_tick_timer_->isActive()) {
        scan_tick_timer_->start();
    }
}

void PlannerScreen::notifyScanSegmentCompleted() {
    // Phase A of the per-segment hand-off. Fires when the controller
    // publishes "segment_complete" (final waypoint reached, just before
    // _dc_end_sequence kicks off). The GP8800 actuator is still retracting
    // and /dc/end_and_save has not returned yet — we MUST NOT publish the
    // next segment here. We just mark the segment done, freeze the timer,
    // park the run-state in "Saving…", and stash the next index for Phase B
    // (notifyScanSegmentSaved) to consume once the controller confirms the
    // save.
    SessionCache& cache = activeSession();
    if (cache.scan_run_state != ScanRunState::Running) {
        return;
    }
    const int completed_idx = cache.scan_active_segment_index;
    if (completed_idx < 0 || completed_idx >= static_cast<int>(cache.scan_segments.size())) {
        return;
    }
    auto& completed_seg = cache.scan_segments[static_cast<size_t>(completed_idx)];
    if (!completed_seg.selected || completed_seg.completed) {
        return;
    }
    completed_seg.completed = true;
    completed_seg.selected = false;  // Auto-deselect + lock on completion.
    completed_seg.progress_pct = 100.0;
    completed_seg.quality_pct = std::max(completed_seg.quality_pct, 1.0);

    if (scan_tick_timer_) {
        scan_tick_timer_->stop();
    }
    cache.scan_run_state = ScanRunState::Paused;
    scan_pause_explicit_ = false;
    scan_manual_resume_after_override_ = false;
    scan_dc_save_in_flight_ = true;
    scan_pending_next_segment_idx_ =
        firstPendingSelectedSegmentIndex(completed_idx + 1);

    recomputeScanAggregateStats();
    refreshScanSegmentList();
    pushScanSegmentsToPlot();
    updateScanRunUi();
    updateFooter();
}

void PlannerScreen::notifyScanSegmentSaved() {
    // Phase B of the per-segment hand-off. Fires when the controller's
    // _dc_end_sequence has finished /dc/end_and_save and the GP8800
    // actuator is fully retracted. Now safe to either auto-advance
    // (automatic mode) or prompt the operator (manual mode).
    if (!scan_dc_save_in_flight_) {
        return;
    }
    scan_dc_save_in_flight_ = false;

    SessionCache& cache = activeSession();
    const int next_idx = scan_pending_next_segment_idx_;
    scan_pending_next_segment_idx_ = -1;

    if (next_idx < 0) {
        cache.scan_run_state = ScanRunState::Completed;
        cache.scan_active_segment_index = -1;
        recomputeScanAggregateStats();
        updateScanRunUi();
        updateFooter();
        return;
    }

    cache.scan_active_segment_index = next_idx;
    recomputeScanAggregateStats();
    updateScanRunUi();
    updateFooter();

    const bool automatic_mode =
        cache.scan_progression_mode != QStringLiteral("manual");
    if (automatic_mode) {
        startSegmentExecution(next_idx, /*resume_action=*/false);
        updateScanRunUi();
        updateFooter();
        return;
    }

    const int choice = BdrMessageBox::question(
        this,
        QStringLiteral("Segment complete"),
        QStringLiteral("Segment finished and saved. Proceed to the next segment now?"),
        BdrMessageBox::Yes);
    if (choice == BdrMessageBox::Yes) {
        startSegmentExecution(next_idx, /*resume_action=*/false);
        updateScanRunUi();
        updateFooter();
        return;
    }
    if (lbl_scan_status_pill_text_) {
        lbl_scan_status_pill_text_->setText(QStringLiteral("Paused"));
    }
}

void PlannerScreen::updateScanRunUi() {
    if (!scan_page_) {
        return;
    }
    const SessionCache* cache = activeSessionPtr();
    const ScanRunState run_state =
        cache ? cache->scan_run_state : ScanRunState::Idle;

    // Lock the cruise-speed slider while a scan is mid-flight. The value is
    // committed to /mpc_accel_controller's `max_linear_velocity` ROS param
    // exactly once at scan-start; mid-scan changes would silently desync UI
    // from controller state, so we just disable the input.
    if (slider_coverage_scan_speed_) {
        slider_coverage_scan_speed_->setEnabled(run_state != ScanRunState::Running);
    }

    // Single-segment ("complete scan") mode collapses redundant per-segment
    // UI: the Current Segment card duplicates Overall Progress, the Segment
    // Status list has one row, and the legend chip's counts never change
    // usefully. Only override on the Scan step — applySessionToUi already
    // hides these for non-Scan steps via the on_scan branch.
    if (current_step_ == PlannerStep::Scan) {
        const bool collapse_segment_ui =
            !cache || cache->scan_segments.size() <= 1;
        if (scan_current_segment_card_) {
            scan_current_segment_card_->setVisible(!collapse_segment_ui);
        }
        if (scan_segment_status_card_) {
            scan_segment_status_card_->setVisible(!collapse_segment_ui);
        }
        if (scan_legend_chip_) {
            scan_legend_chip_->setVisible(!collapse_segment_ui);
        }
    }

    // Status pill text + dot color.
    if (lbl_scan_status_pill_text_ && lbl_scan_status_pill_dot_) {
        QString pill_text;
        QString dot_color;
        if (scan_discarded_) {
            // Terminal post-Discard state — outranks every other label so
            // the operator sees "Discarded" until they press Complete
            // Mission and Stage 5 tears down.
            pill_text = QStringLiteral("Discarded");
            dot_color = QStringLiteral("#B91C1C");
        } else if (scan_discard_in_flight_) {
            pill_text = QStringLiteral("Discarding…");
            dot_color = QStringLiteral("#F59E0B");
        } else if (scan_manual_override_active_) {
            pill_text = QStringLiteral("Manual Override");
            dot_color = QStringLiteral("#F59E0B");
        } else if (scan_dc_save_in_flight_) {
            pill_text = QStringLiteral("Saving…");
            dot_color = QStringLiteral("#F59E0B");
        } else {
            switch (run_state) {
                case ScanRunState::Idle:      pill_text = QStringLiteral("Ready");      dot_color = QStringLiteral("#71717B"); break;
                case ScanRunState::Running:   pill_text = QStringLiteral("Scanning");   dot_color = QStringLiteral("#00D492"); break;
                case ScanRunState::Paused:
                    pill_text = scan_estop_latched_ ? QStringLiteral("E-Stop") : QStringLiteral("Paused");
                    dot_color = QStringLiteral("#F59E0B");
                    break;
                case ScanRunState::Completed: pill_text = QStringLiteral("Completed");  dot_color = QStringLiteral("#00D492"); break;
            }
        }
        lbl_scan_status_pill_text_->setText(pill_text);
        lbl_scan_status_pill_dot_->setPixmap(
            loadSvgPixmap(QStringLiteral(":/assets/missionplanner/status_dot.svg"), 8, 8, dot_color));
    }

    // Start / Pause / Resume button face.
    if (btn_scan_start_pause_ && lbl_scan_start_pause_text_ && lbl_scan_start_pause_icon_) {
        QString label;
        QString icon_path = QStringLiteral(":/assets/missionplanner/scan_play.svg");
        bool enabled = true;
        switch (run_state) {
            case ScanRunState::Idle:
                label = QStringLiteral("Start Scan");
                break;
            case ScanRunState::Running:
                label = QStringLiteral("Pause");
                icon_path = QStringLiteral(":/assets/missionplanner/scan_pause.svg");
                break;
            case ScanRunState::Paused:
                label = QStringLiteral("Resume Scan");
                break;
            case ScanRunState::Completed:
                label = QStringLiteral("Mission Complete");
                enabled = false;
                break;
        }
        if (scan_manual_override_active_ && run_state != ScanRunState::Completed) {
            label = QStringLiteral("Manual Override");
            enabled = false;
            icon_path = QStringLiteral(":/assets/missionplanner/scan_pause.svg");
        }
        if (scan_dc_save_in_flight_ && run_state != ScanRunState::Completed) {
            // Lock out Continue/Resume while the controller is mid
            // /dc/end_and_save. Re-enabled by notifyScanSegmentSaved
            // (Phase B) once the GP8800 actuator finishes retracting.
            label = QStringLiteral("Saving…");
            enabled = false;
            icon_path = QStringLiteral(":/assets/missionplanner/scan_pause.svg");
        }
        lbl_scan_start_pause_text_->setText(label);
        lbl_scan_start_pause_icon_->setPixmap(
            loadSvgPixmap(icon_path, 20, 20, QStringLiteral("#FFFFFF")));
        btn_scan_start_pause_->setEnabled(enabled);
    }
    if (btn_scan_emergency_stop_) {
        btn_scan_emergency_stop_->setEnabled(
            run_state == ScanRunState::Running || run_state == ScanRunState::Paused);
        if (lbl_scan_emergency_stop_text_) {
            lbl_scan_emergency_stop_text_->setText(
                scan_estop_latched_ ? QStringLiteral("Clear E-Stop")
                                    : QStringLiteral("Emergency Stop"));
        }
    }
    if (btn_scan_cancel_) {
        // Dual-mode button. Three visual states:
        //   1. Cancel Scan (amber #FE9A00, X icon) — Running/Paused with
        //      estop OR manual-override unlocked. Mid-scan abort path.
        //   2. Discard Scan (dark red #B91C1C, trash icon) — Completed and
        //      not yet discarded. Post-completion delete path.
        //   3. Discarded (grey, trash icon, disabled) — terminal state
        //      after Discard finishes (success or terminal failure).
        // The connected lambda dispatches to onScanCancelClicked vs
        // onScanDiscardClicked based on run_state == Completed.
        const bool is_discarded = scan_discarded_;
        const bool is_completed_state = run_state == ScanRunState::Completed;
        const bool is_discard_mode = is_completed_state || is_discarded;

        QString style;
        QString icon_path;
        QString label;
        bool enabled = false;

        if (is_discarded) {
            // Terminal "Discarded" — grey, disabled, trash icon. Operator
            // advances by pressing Complete Mission.
            style = QStringLiteral(
                "QPushButton { background: rgba(82,82,91,0.4); border: none; "
                "border-radius: 10px; }"
                "QPushButton:disabled { background: rgba(82,82,91,0.4); "
                "color: rgba(228,228,231,0.5); }");
            icon_path = QStringLiteral(":/assets/missionplanner/scan_discard.svg");
            label = scan_discard_failed_
                ? QStringLiteral("Discarded (failed)")
                : QStringLiteral("Discarded");
            enabled = false;
        } else if (is_completed_state) {
            // Discard Scan — dark red (#B91C1C, one shade darker than
            // E-Stop's #DC2626 so the two reds read as related-but-distinct).
            style = QStringLiteral(
                "QPushButton { background: #B91C1C; border: none; "
                "border-radius: 10px; }"
                "QPushButton:hover { background: #DC2626; }"
                "QPushButton:disabled { background: rgba(82,82,91,0.4); "
                "color: rgba(228,228,231,0.5); }");
            icon_path = QStringLiteral(":/assets/missionplanner/scan_discard.svg");
            label = scan_discard_in_flight_ ? QStringLiteral("Discarding…")
                                            : QStringLiteral("Discard Scan");
            enabled = !scan_discard_in_flight_;
        } else {
            // Cancel Scan — amber #FE9A00 per Figma node 162:233. Gated
            // behind unlocked (estop OR manual-override) AND not mid-save.
            style = QStringLiteral(
                "QPushButton { background: #FE9A00; border: none; "
                "border-radius: 10px; }"
                "QPushButton:hover { background: #FFAA22; }"
                "QPushButton:disabled { background: rgba(82,82,91,0.4); "
                "color: rgba(228,228,231,0.5); }");
            icon_path = QStringLiteral(":/assets/missionplanner/scan_cancel.svg");
            label = scan_cancel_in_flight_ ? QStringLiteral("Cancelling…")
                                           : QStringLiteral("Cancel Scan");
            const bool unlocked =
                scan_estop_latched_ || scan_manual_override_engaged_once_;
            enabled = unlocked &&
                      !scan_cancel_in_flight_ &&
                      !scan_dc_save_in_flight_;
        }

        btn_scan_cancel_->setStyleSheet(style);
        btn_scan_cancel_->setEnabled(enabled);
        if (lbl_scan_cancel_text_) {
            lbl_scan_cancel_text_->setText(label);
        }
        if (lbl_scan_cancel_icon_) {
            lbl_scan_cancel_icon_->setPixmap(
                loadSvgPixmap(icon_path, 20, 20, QStringLiteral("#FFFFFF")));
        }
        // Suppress unused warning when neither branch needs the variable.
        (void)is_discard_mode;
    }

    // Current Segment card.
    if (cache && lbl_scan_active_segment_) {
        if (cache->scan_active_segment_index >= 0 &&
            cache->scan_active_segment_index <
                static_cast<int>(cache->scan_segments.size())) {
            lbl_scan_active_segment_->setText(
                cache->scan_segments[cache->scan_active_segment_index].name);
        } else {
            lbl_scan_active_segment_->setText(QStringLiteral("--"));
        }
    }
    if (cache && bar_scan_active_progress_ && lbl_scan_active_progress_value_) {
        double pct = 0.0;
        if (cache->scan_active_segment_index >= 0 &&
            cache->scan_active_segment_index <
                static_cast<int>(cache->scan_segments.size())) {
            pct = cache->scan_segments[cache->scan_active_segment_index].progress_pct;
        }
        bar_scan_active_progress_->setValue(static_cast<int>(std::round(pct * 10.0)));
        lbl_scan_active_progress_value_->setText(
            QString::number(static_cast<int>(std::round(pct))) + QStringLiteral("%"));
    }
    if (cache && lbl_scan_active_quality_) {
        double q = 0.0;
        if (cache->scan_active_segment_index >= 0 &&
            cache->scan_active_segment_index <
                static_cast<int>(cache->scan_segments.size())) {
            q = cache->scan_segments[cache->scan_active_segment_index].quality_pct;
        }
        lbl_scan_active_quality_->setText(
            QString::number(q, 'f', 0) + QStringLiteral("%"));
    }

    // Overall Progress card.
    if (cache && bar_scan_total_coverage_ && lbl_scan_total_coverage_value_) {
        bar_scan_total_coverage_->setValue(
            static_cast<int>(std::round(cache->scan_total_coverage_pct * 10.0)));
        lbl_scan_total_coverage_value_->setText(
            QString::number(cache->scan_total_coverage_pct, 'f', 1) + QStringLiteral("%"));
    }
    if (cache && bar_scan_total_quality_ && lbl_scan_total_quality_value_) {
        bar_scan_total_quality_->setValue(
            static_cast<int>(std::round(cache->scan_avg_quality_pct * 10.0)));
        lbl_scan_total_quality_value_->setText(
            QString::number(cache->scan_avg_quality_pct, 'f', 1) + QStringLiteral("%"));
    }
    if (cache && lbl_scan_time_value_) {
        const qint64 secs = cache->scan_elapsed_ms / 1000;
        lbl_scan_time_value_->setText(
            QString::asprintf("%02lld:%02lld", secs / 60, secs % 60));
    }

    // Statistics card.
    if (cache && lbl_scan_stats_distance_) {
        lbl_scan_stats_distance_->setText(
            QString::number(cache->scan_distance_traveled_m, 'f', 1) + QStringLiteral(" m"));
    }
    if (cache && lbl_scan_stats_points_) {
        lbl_scan_stats_points_->setText(
            QString::number(cache->scan_total_points_m, 'f', 2) + QStringLiteral("M"));
    }
    if (cache && lbl_scan_stats_avg_quality_) {
        lbl_scan_stats_avg_quality_->setText(
            QString::number(cache->scan_avg_quality_pct, 'f', 1) + QStringLiteral("%"));
    }
    if (cache && lbl_scan_stats_eta_) {
        if (cache->scan_estimated_ms_left < 0) {
            lbl_scan_stats_eta_->setText(QStringLiteral("--:--"));
        } else {
            const qint64 secs = cache->scan_estimated_ms_left / 1000;
            lbl_scan_stats_eta_->setText(
                QString::asprintf("%02lld:%02lld", secs / 60, secs % 60));
        }
    }

    // Center bar summary "MM:SS • A/B segments" — always shown to mirror
    // Figma exactly. Empty / single-segment runs read as "0/1 segments".
    if (cache && lbl_scan_run_summary_) {
        const qint64 secs = cache->scan_elapsed_ms / 1000;
        int progressed_or_completed = 0;
        int total_selected = 0;
        for (size_t i = 0; i < cache->scan_segments.size(); ++i) {
            const auto& seg = cache->scan_segments[i];
            if (seg.selected || seg.completed) {
                ++total_selected;
                if (seg.completed ||
                    (static_cast<int>(i) == cache->scan_active_segment_index &&
                     seg.progress_pct > 0.0)) {
                    ++progressed_or_completed;
                }
            }
        }
        lbl_scan_run_summary_->setText(
            QString::asprintf("%02lld:%02lld \xE2\x80\xA2 %d/%d segments",
                              secs / 60, secs % 60, progressed_or_completed, total_selected));
    }

    // Legend chip counts.
    if (cache && lbl_scan_legend_completed_ && lbl_scan_legend_active_ &&
        lbl_scan_legend_pending_) {
        int completed = 0;
        int active = 0;
        int pending = 0;
        for (size_t i = 0; i < cache->scan_segments.size(); ++i) {
            const auto& seg = cache->scan_segments[i];
            if (!seg.selected && !seg.completed) {
                continue;
            }
            if (seg.completed) {
                ++completed;
            } else if (static_cast<int>(i) == cache->scan_active_segment_index) {
                ++active;
            } else {
                ++pending;
            }
        }
        lbl_scan_legend_completed_->setText(
            QStringLiteral("Completed: %1").arg(completed));
        lbl_scan_legend_active_->setText(QStringLiteral("Active: %1").arg(active));
        lbl_scan_legend_pending_->setText(QStringLiteral("Pending: %1").arg(pending));
    }

    refreshScanSegmentStatusList();
    updateScanLiveTelemetry();
    updateScanManualOverrideIndicator();
}

void PlannerScreen::refreshScanSegmentStatusList() {
    if (!list_scan_segment_status_) {
        return;
    }
    const SessionCache* cache = activeSessionPtr();
    // The previous spinner label is destroyed by QListWidget::clear; null
    // the QPointer explicitly so the timer's next tick won't try to paint
    // into a dangling label before the new active row is created below.
    scan_segment_spinner_label_.clear();

    // Preserve the operator's scroll position across rebuilds. Without this,
    // the high-frequency refresh cadence (telemetry tick + DC events) snaps
    // the scrollbar back to 0 on every redraw, making it impossible to scroll
    // down to inspect later segments. The QListWidget API does not expose a
    // "model in-place update" without a real model, so we keep the simple
    // clear+rebuild but save and restore the vertical scroll value around it.
    auto* scroll_bar = list_scan_segment_status_->verticalScrollBar();
    const int saved_scroll = scroll_bar ? scroll_bar->value() : 0;

    list_scan_segment_status_->clear();
    if (!cache) {
        return;
    }
    for (size_t i = 0; i < cache->scan_segments.size(); ++i) {
        const auto& seg = cache->scan_segments[i];
        if (!seg.selected && !seg.completed) {
            continue;  // Don't show unselected segments.
        }

        const bool is_completed = seg.completed;
        const bool is_active = !is_completed &&
                               static_cast<int>(i) == cache->scan_active_segment_index;
        int row_height = 52;
        // QSS selectors are scoped to a unique objectName + leading-dot
        // (strict class match) so the background + border + border-radius
        // do NOT cascade onto child QWidgets/QLabels. Without this, every
        // child widget inside the row inherited the 2 px tinted border and
        // painted a ghost rounded rectangle inside the card (most visible
        // around the "Coverage" row in active state). Border width, fills,
        // and heights mirror the Figma segment-status spec
        // (node 139:858 / 139:876 / 139:887).
        QString row_style = QStringLiteral(
            ".QWidget#segmentRow { background: #18181B; border: 2px solid #3F3F47; "
            "  border-radius: 10px; }");
        if (is_completed) {
            row_height = 88;
            row_style = QStringLiteral(
                ".QWidget#segmentRow { background: rgba(0,188,125,0.10); "
                "  border: 2px solid rgba(0,188,125,0.30); border-radius: 10px; }");
        } else if (is_active) {
            row_height = 68;
            row_style = QStringLiteral(
                ".QWidget#segmentRow { background: rgba(43,127,255,0.10); "
                "  border: 2px solid rgba(43,127,255,0.30); border-radius: 10px; }");
        }

        auto* item = new QListWidgetItem(list_scan_segment_status_);
        list_scan_segment_status_->addItem(item);
        auto* row = new QWidget(list_scan_segment_status_);
        row->setObjectName(QStringLiteral("segmentRow"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        row->setStyleSheet(row_style);
        auto* row_layout = new QHBoxLayout(row);
        // Figma spec: pt-[14] pb-[2] px-[14]. The previous symmetric (14,14,14,14)
        // padded the bottom too far, leaving a visible gap between the Coverage
        // row and the card's bottom edge in the active state.
        row_layout->setContentsMargins(14, 14, 14, 2);
        row_layout->setSpacing(8);

        // Helper: every child container we add inside the row must be
        // explicitly transparent. Even one stray non-transparent QWidget here
        // gets composited on top of the row's tinted background and looks
        // like a "grey box inside the card" — which is exactly what the
        // operator was reporting on the active row.
        auto make_transparent_panel = [row](QWidget* parent) -> QWidget* {
            auto* w = new QWidget(parent);
            w->setAttribute(Qt::WA_TranslucentBackground, true);
            w->setAttribute(Qt::WA_NoSystemBackground, true);
            w->setAutoFillBackground(false);
            w->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
            return w;
        };

        auto* meta = make_transparent_panel(row);
        auto* meta_layout = new QVBoxLayout(meta);
        meta_layout->setContentsMargins(0, 0, 0, 0);
        meta_layout->setSpacing(4);
        meta_layout->addWidget(makeTextLabel(
            meta,
            seg.name,
            QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 700; "
                           "color: #FFFFFF;")));
        if (is_completed) {
            auto* coverage_row = make_transparent_panel(meta);
            auto* coverage_layout = new QHBoxLayout(coverage_row);
            coverage_layout->setContentsMargins(0, 0, 0, 0);
            coverage_layout->setSpacing(8);
            coverage_layout->addWidget(makeTextLabel(
                coverage_row,
                QStringLiteral("Coverage"),
                QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; "
                               "color: #9F9FA9;")));
            coverage_layout->addStretch(1);
            coverage_layout->addWidget(makeTextLabel(
                coverage_row,
                QStringLiteral("100%"),
                QStringLiteral("font-family: 'Liberation Mono'; font-size: 12px; font-weight: 400; "
                               "color: #D4D4D8;")));
            meta_layout->addWidget(coverage_row);

            auto* quality_row = make_transparent_panel(meta);
            auto* quality_layout = new QHBoxLayout(quality_row);
            quality_layout->setContentsMargins(0, 0, 0, 0);
            quality_layout->setSpacing(8);
            quality_layout->addWidget(makeTextLabel(
                quality_row,
                QStringLiteral("Quality"),
                QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; "
                               "color: #9F9FA9;")));
            quality_layout->addStretch(1);
            quality_layout->addWidget(makeTextLabel(
                quality_row,
                QString::number(seg.quality_pct, 'f', 0) + QStringLiteral("%"),
                QStringLiteral("font-family: 'Liberation Mono'; font-size: 12px; font-weight: 400; "
                               "color: #D4D4D8;")));
            meta_layout->addWidget(quality_row);
        } else if (is_active) {
            auto* coverage_row = make_transparent_panel(meta);
            auto* coverage_layout = new QHBoxLayout(coverage_row);
            coverage_layout->setContentsMargins(0, 0, 0, 0);
            coverage_layout->setSpacing(8);
            coverage_layout->addWidget(makeTextLabel(
                coverage_row,
                QStringLiteral("Coverage"),
                QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; "
                               "color: #9F9FA9;")));
            coverage_layout->addStretch(1);
            coverage_layout->addWidget(makeTextLabel(
                coverage_row,
                QString::number(seg.progress_pct, 'f', 0) + QStringLiteral("%"),
                QStringLiteral("font-family: 'Liberation Mono'; font-size: 12px; font-weight: 400; "
                               "color: #D4D4D8;")));
            meta_layout->addWidget(coverage_row);
        }
        row_layout->addWidget(meta, 1);

        // Indicator: 16x16 per Figma spec (139:862 / 139:880 / 139:891).
        auto* indicator = new QLabel(row);
        indicator->setFixedSize(16, 16);
        indicator->setAttribute(Qt::WA_TranslucentBackground, true);
        indicator->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        indicator->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        if (is_completed) {
            indicator->setPixmap(loadSvgPixmap(
                QStringLiteral(":/assets/missionplanner/scan_check.svg"),
                16, 16, QStringLiteral("#00D492")));
        } else if (is_active) {
            // Render the spinner at the current rotation angle; the timer
            // started below keeps it spinning while the segment is in
            // progress (Figma node 139:880 shows the same -84.74° rotation
            // applied to the 3/4-circle ring icon to convey motion).
            indicator->setPixmap(loadRotatedSvgPixmap(
                QStringLiteral(":/assets/missionplanner/scan_segment_active.svg"),
                16, 16, QStringLiteral("#3B82F6"),
                static_cast<qreal>(scan_segment_spinner_angle_)));
            scan_segment_spinner_label_ = indicator;
            ensureScanSegmentSpinnerTimer();
        } else {
            indicator->setPixmap(loadSvgPixmap(
                QStringLiteral(":/assets/missionplanner/status_dot.svg"),
                10, 10, QStringLiteral("#52525B")));
        }
        // Anchor the indicator to the top of the card so it sits inline with
        // the title row, matching the Figma layout (the icon is in the same
        // horizontal row as the segment name, not vertically centred over the
        // entire card).
        row_layout->addWidget(indicator, 0, Qt::AlignTop);

        item->setSizeHint(QSize(0, row_height));
        list_scan_segment_status_->setItemWidget(item, row);
    }

    // No active row was found this refresh — stop the spinner timer so we
    // don't burn CPU rotating into a label that no longer exists.
    if (!scan_segment_spinner_label_ && scan_segment_spinner_timer_) {
        scan_segment_spinner_timer_->stop();
    }

    // Restore the previous scroll position, clamped to the new content range.
    // Done after the layout has been finalized via items + setItemWidget so
    // QScrollBar::maximum() reflects the rebuilt content height.
    if (scroll_bar) {
        list_scan_segment_status_->doItemsLayout();
        const int new_max = scroll_bar->maximum();
        scroll_bar->setValue(std::min(saved_scroll, new_max));
    }
}

void PlannerScreen::ensureScanSegmentSpinnerTimer() {
    if (!scan_segment_spinner_timer_) {
        scan_segment_spinner_timer_ = new QTimer(this);
        // 30 ms ticks * 12° per tick = full revolution every ~900 ms.
        scan_segment_spinner_timer_->setInterval(30);
        connect(scan_segment_spinner_timer_, &QTimer::timeout,
                this, &PlannerScreen::tickScanSegmentSpinner);
    }
    if (!scan_segment_spinner_timer_->isActive()) {
        scan_segment_spinner_timer_->start();
    }
}

void PlannerScreen::tickScanSegmentSpinner() {
    if (!scan_segment_spinner_label_) {
        if (scan_segment_spinner_timer_) {
            scan_segment_spinner_timer_->stop();
        }
        return;
    }
    scan_segment_spinner_angle_ = (scan_segment_spinner_angle_ + 12) % 360;
    scan_segment_spinner_label_->setPixmap(loadRotatedSvgPixmap(
        QStringLiteral(":/assets/missionplanner/scan_segment_active.svg"),
        16, 16, QStringLiteral("#3B82F6"),
        static_cast<qreal>(scan_segment_spinner_angle_)));
}

void PlannerScreen::updateScanLiveTelemetry() {
    const SessionCache* cache = activeSessionPtr();
    if (!cache || cache->scan_run_state != ScanRunState::Running) {
        if (lbl_scan_telemetry_speed_) {
            lbl_scan_telemetry_speed_->setText(QStringLiteral("0.00 m/s"));
        }
        last_telemetry_valid_ = false;
    }
    if (!live_robot_pose_.has_value()) {
        return;
    }
    const PathState& p = *live_robot_pose_;
    if (lbl_scan_telemetry_pos_x_) {
        lbl_scan_telemetry_pos_x_->setText(
            QString::number(p.point.x, 'f', 2) + QStringLiteral(" m"));
    }
    if (lbl_scan_telemetry_pos_y_) {
        lbl_scan_telemetry_pos_y_->setText(
            QString::number(p.point.y, 'f', 2) + QStringLiteral(" m"));
    }
    if (lbl_scan_telemetry_heading_) {
        const double deg = p.heading * 180.0 / M_PI;
        lbl_scan_telemetry_heading_->setText(
            QString::number(deg, 'f', 1) + QStringLiteral("\xC2\xB0"));
    }

    // Speed = distance(last, now) / dt.
    const QDateTime now = QDateTime::currentDateTime();
    if (last_telemetry_valid_ && lbl_scan_telemetry_speed_) {
        const double dt = std::max(0.001,
                                   last_telemetry_ts_.msecsTo(now) / 1000.0);
        const double dx = p.point.x - last_telemetry_xy_.x;
        const double dy = p.point.y - last_telemetry_xy_.y;
        const double v = std::sqrt(dx * dx + dy * dy) / dt;
        lbl_scan_telemetry_speed_->setText(
            QString::number(v, 'f', 2) + QStringLiteral(" m/s"));
    }
    last_telemetry_ts_ = now;
    last_telemetry_xy_ = p.point;
    last_telemetry_valid_ = true;
}

// ============================================================================
// Stage 4 (Scan execution) — slots
// ============================================================================

void PlannerScreen::onScanStartPauseClicked() {
    SessionCache& cache = activeSession();
    if (scan_manual_override_active_ && cache.scan_run_state != ScanRunState::Completed) {
        setInlineStatus(QStringLiteral("Click the map view to hand control back to autonomy."),
                        QStringLiteral("#F59E0B"));
        return;
    }
    switch (cache.scan_run_state) {
        case ScanRunState::Idle:
        {
            const int first_idx = firstPendingSelectedSegmentIndex();
            if (first_idx < 0) {
                BdrMessageBox::information(
                    this,
                    QStringLiteral("No pending segments"),
                    QStringLiteral("All selected segments are already complete."));
                return;
            }
            scan_pause_explicit_ = false;
            scan_manual_resume_after_override_ = false;
            startSegmentExecution(first_idx, /*resume_action=*/false);
            updateScanRunUi();
            updateFooter();
            break;
        }
        case ScanRunState::Running:
            cache.scan_run_state = ScanRunState::Paused;
            scan_pause_explicit_ = true;
            scan_manual_resume_after_override_ = false;
            if (scan_tick_timer_) {
                scan_tick_timer_->stop();
            }
            emit scanPauseRequested();
            updateScanRunUi();
            updateFooter();
            break;
        case ScanRunState::Paused:
            if (scan_estop_latched_) {
                scan_estop_latched_ = false;
            }
            scan_pause_explicit_ = false;
            if (cache.scan_active_segment_index >= 0 &&
                cache.scan_active_segment_index < static_cast<int>(cache.scan_segments.size()) &&
                cache.scan_segments[static_cast<size_t>(cache.scan_active_segment_index)].completed) {
                startSegmentExecution(cache.scan_active_segment_index, /*resume_action=*/false);
            } else {
                cache.scan_run_state = ScanRunState::Running;
                if (scan_tick_timer_ && !scan_tick_timer_->isActive()) {
                    scan_tick_timer_->start();
                }
                emit scanResumeRequested();
            }
            updateScanRunUi();
            updateFooter();
            break;
        case ScanRunState::Completed:
            break;
    }
}

void PlannerScreen::onScanEmergencyStopClicked() {
    SessionCache& cache = activeSession();
    if (cache.scan_run_state == ScanRunState::Completed) {
        return;
    }
    if (!scan_estop_latched_) {
        scan_estop_latched_ = true;
        cache.scan_run_state = ScanRunState::Paused;
        if (scan_tick_timer_) {
            scan_tick_timer_->stop();
        }
        // Operator pre-empted any in-flight DC save handoff. Drop the
        // pending-next-segment pointer so we don't auto-advance once
        // /dc/end_and_save eventually returns and segment_saved fires
        // — the operator is now driving via E-Stop / Resume instead.
        scan_dc_save_in_flight_ = false;
        scan_pending_next_segment_idx_ = -1;
    } else {
        scan_estop_latched_ = false;
        const bool waiting_for_next_segment =
            cache.scan_active_segment_index >= 0 &&
            cache.scan_active_segment_index < static_cast<int>(cache.scan_segments.size()) &&
            cache.scan_segments[static_cast<size_t>(cache.scan_active_segment_index)].completed;
        if (cache.scan_run_state != ScanRunState::Completed && !waiting_for_next_segment) {
            cache.scan_run_state = ScanRunState::Running;
            if (scan_tick_timer_ && !scan_tick_timer_->isActive()) {
                scan_tick_timer_->start();
            }
        }
    }
    emit emergencyStopRequested();
    updateScanRunUi();
    updateFooter();
}

void PlannerScreen::onScanCancelClicked() {
    // Defensive re-check of the gate. updateScanRunUi already disables the
    // button when these conditions don't hold, but guard against signal
    // races (e.g. button disabled mid-event-dispatch).
    if (scan_cancel_in_flight_ || scan_dc_save_in_flight_) {
        return;
    }
    SessionCache& cache = activeSession();
    if (cache.scan_run_state == ScanRunState::Completed) {
        return;
    }
    if (!scan_estop_latched_ && !scan_manual_override_engaged_once_) {
        return;
    }

    const int confirm = BdrMessageBox::question(
        this,
        QStringLiteral("Cancel scan"),
        QStringLiteral(
            "All scan data collected so far will be permanently deleted, "
            "and the mission will reset back to Map Processing so you can "
            "re-plan and try again. The robot pipeline will keep running. "
            "This cannot be undone."),
        BdrMessageBox::No);
    if (confirm != BdrMessageBox::Yes) {
        return;
    }

    qInfo("[PlannerScreen] Cancel Scan confirmed — emitting cancelScanRequested; "
          "estop_latched=%d manual_override_once=%d",
          scan_estop_latched_ ? 1 : 0,
          scan_manual_override_engaged_once_ ? 1 : 0);

    // Lock the UI immediately so the operator can't double-fire while
    // AppShell hits the controller. AppShell calls notifyScanCancelled()
    // when /dc/cancel_scan returns (or hits its 8 s ceiling), which clears
    // this flag and navigates back to Map Processing.
    scan_cancel_in_flight_ = true;
    if (scan_tick_timer_) {
        scan_tick_timer_->stop();
    }
    setScanManualOverride(false);
    stopScanCameraStream();
    updateScanRunUi();
    updateFooter();

    emit cancelScanRequested();
}

void PlannerScreen::notifyScanCancelled(bool success) {
    qInfo("[PlannerScreen] notifyScanCancelled — controller %s",
          success ? "succeeded" : "failed (proceeding with UI reset anyway)");

    SessionCache& cache = activeSession();

    // Wipe per-mission Stage-4 runtime state. Mirrors the bottom half of
    // enterScanStage() but without auto-rebuilding segments — the operator
    // is being sent back to Map Processing precisely so they can re-plan.
    cache.scan_run_state = ScanRunState::Idle;
    cache.scan_active_segment_index = -1;
    cache.scan_total_coverage_pct = 0.0;
    cache.scan_avg_quality_pct = 0.0;
    cache.scan_total_points_m = 0.0;
    cache.scan_distance_traveled_m = 0.0;
    cache.scan_elapsed_ms = 0;
    cache.scan_estimated_ms_left = -1;
    cache.scan_segments.clear();
    cache.scan_splits_dirty = true;
    cache.scan_waypoints_published = false;

    // Clear the planned path / coverage so the operator must re-plan
    // before the next scan. Map + hull are intentionally preserved so they
    // don't have to re-load and re-process the point cloud.
    cache.planning_complete = false;
    cache.planned_swaths.clear();
    cache.planned_route.clear();
    cache.planned_path.clear();

    // Reset Stage-4 control flags.
    scan_cancel_in_flight_ = false;
    scan_estop_latched_ = false;
    scan_pause_explicit_ = false;
    scan_manual_override_engaged_once_ = false;
    scan_manual_resume_after_override_ = false;
    scan_dc_save_in_flight_ = false;
    scan_pending_next_segment_idx_ = -1;
    scan_active_segment_path_hint_ = 0;
    scan_last_quality_sample_ms_ = 0;
    scan_quality_segment_index_ = -1;
    scan_camera_stream_requested_ = false;
    if (scan_tick_timer_) {
        scan_tick_timer_->stop();
    }
    setScanManualOverride(false);
    stopScanCameraStream();

    // Send the operator back to Map Processing. navigateToStep() also
    // re-applies the session to the UI, which will refresh the stepper,
    // footer gates, and hide the Stage-4 widgets.
    navigateToStep(PlannerStep::MapProcessing);
}

void PlannerScreen::onScanDiscardClicked() {
    // Defensive re-check. Same dispatch lambda gates this, but guard against
    // signal races (button disabled mid-event-dispatch).
    if (scan_discard_in_flight_ || scan_discarded_) {
        return;
    }
    SessionCache& cache = activeSession();
    if (cache.scan_run_state != ScanRunState::Completed) {
        return;
    }

    const int confirm = BdrMessageBox::question(
        this,
        QStringLiteral("Discard scan"),
        QStringLiteral(
            "The completed mission's scan data will be permanently deleted "
            "from the robot. The mission will reset back to Map Processing "
            "so you can re-plan from scratch. The robot pipeline will keep "
            "running. This cannot be undone."),
        BdrMessageBox::No);
    if (confirm != BdrMessageBox::Yes) {
        return;
    }

    qInfo("[PlannerScreen] Discard Scan confirmed — emitting discardScanRequested");

    scan_discard_in_flight_ = true;
    if (scan_tick_timer_) {
        scan_tick_timer_->stop();
    }
    setScanManualOverride(false);
    stopScanCameraStream();
    updateScanRunUi();
    updateFooter();

    emit discardScanRequested();
}

void PlannerScreen::notifyScanDiscarded(bool success) {
    qInfo("[PlannerScreen] notifyScanDiscarded — controller %s",
          success ? "succeeded" : "FAILED after retry");

    // Discard now mirrors Cancel: the operator is sent back to Map
    // Processing so they can re-plan from scratch. The previous "stay on
    // Stage 5 with planned path preserved" behavior was deliberately
    // dropped because the confirmation dialog promises a return to Map
    // Processing and the operator's next action is always to re-plan.
    //
    // Failure path: if /dc/cancel_scan failed both attempts, mission data
    // may still be on the robot. Surface that loudly with a warning popup
    // BEFORE navigating away, so the operator can ssh in and clean up
    // /R_DATA/<date>/ manually if needed. We still navigate after the
    // popup is dismissed — the OCU has no way to recover the partial
    // mission cleanly anyway, and pinning the operator to Stage 5 with
    // a "Discarded (failed)" button only delays the same outcome.

    if (!success) {
        BdrMessageBox::warning(
            this,
            QStringLiteral("Discard incomplete"),
            QStringLiteral(
                "The robot did not confirm deletion of the mission data. "
                "Some scan files may still be on the robot under "
                "/R_DATA/<today>/ and the mission GNSS log may still be "
                "open. SSH into the robot to verify and clean up manually "
                "before starting another mission. The OCU will return to "
                "Map Processing now."));
    }

    SessionCache& cache = activeSession();

    // Mirror notifyScanCancelled's reset block. Map + hull are intentionally
    // preserved (point cloud is expensive to reload); everything downstream
    // of planning is wiped so re-plan is forced.
    cache.scan_run_state = ScanRunState::Idle;
    cache.scan_active_segment_index = -1;
    cache.scan_total_coverage_pct = 0.0;
    cache.scan_avg_quality_pct = 0.0;
    cache.scan_total_points_m = 0.0;
    cache.scan_distance_traveled_m = 0.0;
    cache.scan_elapsed_ms = 0;
    cache.scan_estimated_ms_left = -1;
    cache.scan_segments.clear();
    cache.scan_splits_dirty = true;
    cache.scan_waypoints_published = false;

    cache.planning_complete = false;
    cache.planned_swaths.clear();
    cache.planned_route.clear();
    cache.planned_path.clear();

    // Reset every Stage-4 control flag, including the three discard flags.
    // Once we navigate away these become unreachable until enterScanStage()
    // runs again, which re-resets them — this is belt-and-suspenders so a
    // future navigation path that bypasses enterScanStage() can't latch a
    // stale "Discarded" button.
    scan_cancel_in_flight_ = false;
    scan_discard_in_flight_ = false;
    scan_discarded_ = false;
    scan_discard_failed_ = false;
    scan_estop_latched_ = false;
    scan_pause_explicit_ = false;
    scan_manual_override_engaged_once_ = false;
    scan_manual_resume_after_override_ = false;
    scan_dc_save_in_flight_ = false;
    scan_pending_next_segment_idx_ = -1;
    scan_active_segment_path_hint_ = 0;
    scan_last_quality_sample_ms_ = 0;
    scan_quality_segment_index_ = -1;
    scan_camera_stream_requested_ = false;
    if (scan_tick_timer_) {
        scan_tick_timer_->stop();
    }
    setScanManualOverride(false);
    stopScanCameraStream();

    navigateToStep(PlannerStep::MapProcessing);
}

void PlannerScreen::onCompleteMissionClicked(const char* trigger) {
    const SessionCache* cache = activeSessionPtr();
    const bool dev_force_complete =
        qEnvironmentVariable("BDR_DEV_START_AT_SCAN").trimmed() == QStringLiteral("1");
    const bool can_complete =
        dev_force_complete ||
        (cache && (cache->scan_run_state == ScanRunState::Completed ||
                   scan_manual_override_engaged_once_));
    if (!can_complete) {
        return;
    }
    const int confirm = BdrMessageBox::question(
        this,
        QStringLiteral("Complete mission"),
        QStringLiteral("This will stop the pipeline and return to Dashboard.\nContinue?"),
        BdrMessageBox::No);
    if (confirm != BdrMessageBox::Yes) {
        return;
    }
    setScanManualOverride(false);
    stopScanCameraStream();
    emit completeMissionRequested();
}

void PlannerScreen::onScanFooterBackClicked() {
    navigateToStep(PlannerStep::ScanSplitting);
}

void PlannerScreen::recomputeScanAggregateStats() {
    SessionCache& cache = activeSession();
    int total_selected = 0;
    double total_coverage = 0.0;
    double quality_sum = 0.0;
    int quality_count = 0;
    for (const auto& seg : cache.scan_segments) {
        if (!seg.selected && !seg.completed) {
            continue;
        }
        ++total_selected;
        const double pct = seg.completed ? 100.0 : seg.progress_pct;
        total_coverage += pct;
        if (seg.completed || pct > 0.0) {
            quality_sum += seg.quality_pct;
            ++quality_count;
        }
    }

    cache.scan_total_coverage_pct =
        total_selected > 0 ? (total_coverage / static_cast<double>(total_selected)) : 0.0;
    cache.scan_avg_quality_pct =
        quality_count > 0 ? (quality_sum / static_cast<double>(quality_count)) : 0.0;
    cache.scan_total_points_m = cache.scan_distance_traveled_m * 0.28;

    if (cache.scan_run_state == ScanRunState::Running &&
        cache.scan_total_coverage_pct > 0.1) {
        const double remaining = std::max(0.0, 100.0 - cache.scan_total_coverage_pct);
        cache.scan_estimated_ms_left =
            static_cast<qint64>(cache.scan_elapsed_ms * (remaining / cache.scan_total_coverage_pct));
    } else if (cache.scan_run_state == ScanRunState::Completed) {
        cache.scan_estimated_ms_left = 0;
    } else {
        cache.scan_estimated_ms_left = -1;
    }
}

double PlannerScreen::computeReprojectionQualityPercent(const PathStateList& segment_path,
                                                        const std::vector<Point2D>& trail) {
    if (segment_path.size() < 2 || trail.size() < 2) {
        return 0.0;
    }

    std::vector<Point2D> path_points;
    path_points.reserve(segment_path.size());
    for (const auto& st : segment_path) {
        path_points.push_back(st.point);
    }

    constexpr double kAssociationMeters = 1.0;
    constexpr size_t kTrailStride = 6;  // keep load low
    double total_error = 0.0;
    int used = 0;

    for (size_t t = 0; t < trail.size(); t += kTrailStride) {
        const auto& p = trail[t];
        double best = std::numeric_limits<double>::max();
        for (size_t i = 0; i + 1 < path_points.size(); ++i) {
            const auto& a = path_points[i];
            const auto& b = path_points[i + 1];
            const double dx = b.x - a.x;
            const double dy = b.y - a.y;
            const double len_sq = dx * dx + dy * dy;
            if (len_sq < 1e-9) {
                continue;
            }
            const double t_proj = std::clamp(
                ((p.x - a.x) * dx + (p.y - a.y) * dy) / len_sq, 0.0, 1.0);
            const double px = a.x + t_proj * dx;
            const double py = a.y + t_proj * dy;
            best = std::min(best, std::hypot(p.x - px, p.y - py));
        }
        if (best <= kAssociationMeters) {
            total_error += best;
            ++used;
        }
    }

    if (used == 0) {
        return 0.0;
    }
    const double avg_error = total_error / static_cast<double>(used);
    return std::clamp(100.0 * (1.0 - (avg_error / kAssociationMeters)), 0.0, 100.0);
}

void PlannerScreen::maybeScheduleScanQualityUpdate() {
    SessionCache& cache = activeSession();
    if (cache.scan_run_state != ScanRunState::Running ||
        !scan_quality_watcher_ || scan_quality_watcher_->isRunning()) {
        return;
    }
    const int idx = cache.scan_active_segment_index;
    if (idx < 0 || idx >= static_cast<int>(cache.scan_segments.size())) {
        return;
    }
    const auto& seg = cache.scan_segments[static_cast<size_t>(idx)];
    if (seg.completed || seg.path.size() < 2 || live_robot_trail_.size() < 2) {
        return;
    }

    // Gate quality sampling until the robot has actually reached the first
    // waypoint of the active segment. `scan_active_segment_path_hint_` is
    // the segment-relative index of the path leg the robot is closest to;
    // it stays at 0 until the robot has driven past seg.path[1]. Sampling
    // before that point produces a meaningless reprojection score against a
    // few centimetres of trail and inflates the running EWMA forever.
    if (scan_active_segment_path_hint_ < 1) {
        return;
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (scan_last_quality_sample_ms_ != 0 &&
        (now_ms - scan_last_quality_sample_ms_) < 5000) {
        return;
    }
    scan_last_quality_sample_ms_ = now_ms;
    scan_quality_segment_index_ = idx;
    const PathStateList path_copy = seg.path;
    const std::vector<Point2D> trail_copy = live_robot_trail_;
    scan_quality_watcher_->setFuture(QtConcurrent::run(
        [path_copy, trail_copy]() {
            return PlannerScreen::computeReprojectionQualityPercent(path_copy, trail_copy);
        }));
}

// BDR_REWIRE: dev-only fast-forward for screenshots / UI iteration. Triggered
// by AppShellWindow when env BDR_DEV_START_AT_SCAN=1 is set on boot.
void PlannerScreen::devForceJumpToScanStep() {
    SessionCache& cache = activeSession();
    if (cache.scan_segments.empty()) {
        SessionCache::ScanSegment seg1;
        seg1.name = QStringLiteral("Segment 1");
        seg1.selected = true;
        seg1.completed = true;
        seg1.progress_pct = 100.0;
        seg1.quality_pct = 98.0;

        SessionCache::ScanSegment seg2;
        seg2.name = QStringLiteral("Segment 2");
        seg2.selected = true;
        seg2.completed = false;
        seg2.progress_pct = 45.0;
        seg2.quality_pct = 95.0;

        SessionCache::ScanSegment seg3;
        seg3.name = QStringLiteral("Segment 3");
        seg3.selected = true;
        seg3.completed = false;
        seg3.progress_pct = 0.0;
        seg3.quality_pct = 0.0;

        cache.scan_segments = {seg1, seg2, seg3};
        cache.scan_active_segment_index = 1;
        cache.scan_run_state = ScanRunState::Idle;
        cache.scan_total_coverage_pct = 0.0;
        cache.scan_avg_quality_pct = 0.0;
        cache.scan_total_points_m = 0.0;
        cache.scan_distance_traveled_m = 0.0;
        cache.scan_elapsed_ms = 0;
        cache.scan_estimated_ms_left = -1;
        live_robot_pose_ = PathState(Point2D{2.0, 2.0}, 1.57079632679);
    }
    navigateToStep(PlannerStep::Scan);
}

void PlannerScreen::onScanTick() {
    SessionCache& cache = activeSession();
    if (cache.scan_run_state != ScanRunState::Running) {
        return;
    }
    cache.scan_elapsed_ms += scan_tick_timer_ ? scan_tick_timer_->interval() : 1000;
    recomputeScanAggregateStats();
    updateScanRunUi();
    updateFooter();
}

void PlannerScreen::onScanManualTeleopTick() {
    if (!scanManualTeleopAllowed()) {
        return;
    }
    emitScanTeleopTwistCommand();
}

void PlannerScreen::syncScanManualTeleopTimer() {
    if (!scan_manual_teleop_timer_) {
        return;
    }
    const bool want_timer =
        current_step_ == PlannerStep::Scan && scan_manual_override_active_;
    if (want_timer) {
        if (!scan_manual_teleop_timer_->isActive()) {
            scan_manual_teleop_timer_->start();
        }
    } else if (scan_manual_teleop_timer_->isActive()) {
        scan_manual_teleop_timer_->stop();
    }
}

void PlannerScreen::setScanManualOverride(bool active) {
    const bool had_keys_down =
        scan_key_w_down_ || scan_key_a_down_ || scan_key_s_down_ || scan_key_d_down_;
    if (scan_manual_override_active_ == active && (!had_keys_down || active)) {
        updateScanManualOverrideIndicator();
        return;
    }

    scan_manual_override_active_ = active;
    if (!active) {
        scan_key_w_down_ = false;
        scan_key_a_down_ = false;
        scan_key_s_down_ = false;
        scan_key_d_down_ = false;
        emitScanZeroTeleopTwist();
    } else {
        setFocus(Qt::OtherFocusReason);
    }

    updateScanManualOverrideIndicator();
    syncScanManualTeleopTimer();
}

void PlannerScreen::updateScanManualOverrideIndicator() {
    if (!lbl_scan_manual_override_state_) {
        return;
    }
    if (scan_manual_override_active_ && scanManualTeleopAllowed()) {
        lbl_scan_manual_override_state_->setText(
            QStringLiteral("Manual Override: Active (%1 rad/s)")
                .arg(scan_teleop_angular_speed_rps_, 0, 'f', 1));
        lbl_scan_manual_override_state_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 700; color: #10B981;"));
    } else if (scan_manual_override_active_) {
        lbl_scan_manual_override_state_->setText(QStringLiteral("Manual Override: Waiting"));
        lbl_scan_manual_override_state_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 700; color: #D97706;"));
    } else if (scan_manual_override_engaged_once_) {
        lbl_scan_manual_override_state_->setText(QStringLiteral("Manual Override: Released"));
        lbl_scan_manual_override_state_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 600; color: #9F9FA9;"));
    } else {
        lbl_scan_manual_override_state_->setText(QStringLiteral("Manual Override: Inactive"));
        lbl_scan_manual_override_state_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 600; color: #9F9FA9;"));
    }
}

bool PlannerScreen::scanManualTeleopAllowed() const {
    const SessionCache* cache = activeSessionPtr();
    return current_step_ == PlannerStep::Scan && scan_manual_override_active_ && cache;
}

bool PlannerScreen::isDescendantOfScanCamera(const QWidget* widget) const {
    if (!widget || !scan_camera_view_) {
        return false;
    }
    const QWidget* cursor = widget;
    while (cursor) {
        if (cursor == scan_camera_view_) {
            return true;
        }
        cursor = cursor->parentWidget();
    }
    return false;
}

bool PlannerScreen::isDescendantOfScanMap(const QWidget* widget) const {
    if (!widget || !plot_) {
        return false;
    }
    const QWidget* cursor = widget;
    while (cursor) {
        if (cursor == plot_) {
            return true;
        }
        cursor = cursor->parentWidget();
    }
    return false;
}

void PlannerScreen::emitScanTeleopTwistCommand() {
    if (!scanManualTeleopAllowed()) {
        return;
    }
    double linear_x = 0.0;
    double angular_z = 0.0;
    if (scan_key_w_down_ && !scan_key_s_down_) {
        linear_x = scan_teleop_linear_speed_mps_;
    } else if (scan_key_s_down_ && !scan_key_w_down_) {
        linear_x = -scan_teleop_linear_speed_mps_;
    }
    if (scan_key_a_down_ && !scan_key_d_down_) {
        angular_z = scan_teleop_angular_speed_rps_;
    } else if (scan_key_d_down_ && !scan_key_a_down_) {
        angular_z = -scan_teleop_angular_speed_rps_;
    }
    emit scanTeleopTwistRequested(linear_x, angular_z);
}

void PlannerScreen::emitScanZeroTeleopTwist() {
    emit scanTeleopTwistRequested(0.0, 0.0);
}

void PlannerScreen::onScanCameraClicked() {
    if (current_step_ != PlannerStep::Scan) {
        return;
    }
    SessionCache& cache = activeSession();

    if (!scan_manual_override_active_) {
        scan_manual_override_engaged_once_ = true;
        scan_manual_resume_after_override_ = cache.scan_run_state == ScanRunState::Running;
        if (cache.scan_run_state == ScanRunState::Running) {
            cache.scan_run_state = ScanRunState::Paused;
            if (scan_tick_timer_) {
                scan_tick_timer_->stop();
            }
            emit scanPauseRequested();
        } else {
            // Best-effort autonomy drop for idle/paused entry into manual mode.
            emit scanPauseRequested();
        }
        setScanManualOverride(true);
        setInlineStatus(QStringLiteral("Manual override enabled. Click map to return to autonomy."),
                        QStringLiteral("#F59E0B"));
        updateScanRunUi();
        updateFooter();
        return;
    }
    setFocus(Qt::OtherFocusReason);
}

void PlannerScreen::onScanMapClicked() {
    if (current_step_ != PlannerStep::Scan) {
        return;
    }
    SessionCache& cache = activeSession();
    if (!scan_manual_override_active_) {
        return;
    }

    bool has_active_pending_segment = false;
    const int idx = cache.scan_active_segment_index;
    if (idx >= 0 && idx < static_cast<int>(cache.scan_segments.size())) {
        has_active_pending_segment = !cache.scan_segments[static_cast<size_t>(idx)].completed;
    }
    const bool should_auto_resume =
        scan_manual_resume_after_override_ && !scan_pause_explicit_ && !scan_estop_latched_ &&
        has_active_pending_segment;

    setScanManualOverride(false);
    if (should_auto_resume) {
        cache.scan_run_state = ScanRunState::Running;
        if (scan_tick_timer_ && !scan_tick_timer_->isActive()) {
            scan_tick_timer_->start();
        }
        emit scanResumeRequested();
        setInlineStatus(QStringLiteral("Autonomy resumed for the active segment."),
                        QStringLiteral("#00D492"));
    } else if (scan_pause_explicit_) {
        setInlineStatus(
            QStringLiteral("Autonomy handoff acknowledged. Scan remains paused until Resume Scan."),
            QStringLiteral("#71717B"));
    } else {
        setInlineStatus(QStringLiteral("Manual override released."),
                        QStringLiteral("#71717B"));
    }
    scan_manual_resume_after_override_ = false;
    updateScanRunUi();
    updateFooter();
}

void PlannerScreen::scheduleScanCameraRestart(const QString& reason) {
    Q_UNUSED(reason);
    if (!scan_camera_restart_timer_ || current_step_ != PlannerStep::Scan || !scan_camera_view_) {
        return;
    }
    if (!scan_camera_stream_requested_ || scan_camera_view_->isPlaying()) {
        return;
    }
    if (scan_camera_restart_timer_->isActive()) {
        return;
    }
    scan_camera_restart_timer_->start();
}

void PlannerScreen::stopScanCameraStream() {
    if (!scan_camera_stream_requested_ &&
        (!scan_camera_view_ || !scan_camera_view_->isPlaying())) {
        return;
    }
    if (scan_camera_view_) {
        scan_camera_view_->stopStream();
    }
    if (scan_camera_restart_timer_ && scan_camera_restart_timer_->isActive()) {
        scan_camera_restart_timer_->stop();
    }
    scan_camera_stream_requested_ = false;
}

}  // namespace f2c_cpp
