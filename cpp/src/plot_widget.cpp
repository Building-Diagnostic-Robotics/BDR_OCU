#include "plot_widget.hpp"
#include "coverage_geometry.hpp"

#include <algorithm>
#include <cmath>

#include <QBrush>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QPolygonF>
#include <QResizeEvent>
#include <QStringLiteral>
#include <QToolTip>
#include <QWheelEvent>

namespace f2c_cpp {

// =============================================================================
// PlotWidget Implementation
// =============================================================================

PlotWidget::PlotWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(400, 400);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    
    // White background
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::white);
    setAutoFillBackground(true);
    setPalette(pal);
}

void PlotWidget::setPoints(const std::vector<Point2D>& points) {
    points_ = points;
    updateDataBounds();
    update();
}

void PlotWidget::setPolygon(const Polygon2D& poly) {
    polygon_ = poly;
    updateDataBounds();
    update();
}

void PlotWidget::setROI(const Polygon2D& roi) {
    roi_ = roi;
    update();
}

void PlotWidget::setObstacles(const std::vector<Obstacle2D>& obstacles) {
    obstacles_ = obstacles;
    if (selected_obstacle_idx_ >= static_cast<int>(obstacles_.size())) {
        selected_obstacle_idx_ = -1;
        emit obstacleSelectionChanged(-1);
    }
    update();
}

void PlotWidget::setSwaths(const SwathList& swaths) {
    swaths_ = swaths;
    update();
}

void PlotWidget::setRoute(const PathStateList& route) {
    route_ = route;
    update();
}

void PlotWidget::setPath(const PathStateList& path) {
    path_ = path;
    update();
}

void PlotWidget::setRobotPose(const std::optional<PathState>& pose) {
    robot_pose_ = pose;
    update();
}

void PlotWidget::setRobotTrail(const std::vector<Point2D>& trail) {
    robot_trail_ = trail;
    update();
}

void PlotWidget::setRobotMarkerSize(double size_meters) {
    robot_marker_size_ = std::max(0.05, size_meters);
    update();
}

void PlotWidget::setCustomPath(const std::vector<Point2D>& path,
                               const std::vector<bool>& visited) {
    custom_waypoints_ = path;
    custom_waypoint_states_ = visited;
    if (custom_waypoint_states_.size() < custom_waypoints_.size()) {
        custom_waypoint_states_.resize(custom_waypoints_.size(), false);
    }
    update();
}

void PlotWidget::setShowCustomPath(bool show) {
    show_custom_path_ = show;
    update();
}

void PlotWidget::setCustomDrawMode(bool enabled) {
    custom_draw_mode_ = enabled;
}

void PlotWidget::setReprojectionLines(const std::vector<ReprojectionLine>& lines) {
    reproj_lines_ = lines;
    hovered_reproj_index_ = -1;
    update();
}

void PlotWidget::clearReprojectionLines() {
    reproj_lines_.clear();
    hovered_reproj_index_ = -1;
    update();
}

void PlotWidget::setLiveOverlay(bool enabled, const std::vector<QString>& lines) {
    show_live_overlay_ = enabled;
    live_overlay_lines_ = lines;
    update();
}

void PlotWidget::setScanSegments(const std::vector<PathStateList>& segments,
                                 const std::vector<QString>& labels,
                                 const std::vector<double>& lengths,
                                 const std::vector<int>& turns,
                                 bool visible,
                                 const std::vector<bool>& selected) {
    int prev_hover = hovered_scan_segment_;
    scan_segments_ = segments;
    scan_segment_labels_ = labels;
    scan_segment_lengths_ = lengths;
    scan_segment_turns_ = turns;
    scan_segment_selected_ = selected;
    show_scan_segments_ = visible;
    // setScanSegments is the planning-stage entry point. Clear any
    // Stage 4 overlay that was attached on a previous frame so we don't
    // paint stale statuses against fresh geometry. PlannerScreen
    // re-applies the overlay (if still in Scan step) immediately after.
    scan_segment_statuses_.clear();
    scan_active_progress_pct_ = 0.0;
    // Preserve hover if still valid; otherwise clear
    if (!show_scan_segments_ || scan_segments_.empty() ||
        prev_hover < 0 || prev_hover >= static_cast<int>(scan_segments_.size())) {
        hovered_scan_segment_ = -1;
    } else {
        hovered_scan_segment_ = prev_hover;
    }
    updateDataBounds();
    update();
}

void PlotWidget::setActiveScanSegment(int idx) {
    active_scan_segment_ = idx;
    update();
}

void PlotWidget::setScanSegmentsOverlay(
    const std::vector<ScanSegmentStatus>& statuses,
    double active_progress_pct) {
    // Empty statuses = clear overlay, revert to planning palette.
    scan_segment_statuses_ = statuses;
    scan_active_progress_pct_ =
        std::clamp(active_progress_pct, 0.0, 100.0);
    update();
}

void PlotWidget::startRectangleMode() {
    drawing_rectangle_ = true;
    rect_points_.clear();
    selecting_ = false;  // Cancel any other selection
    selection_points_.clear();
    setCursor(Qt::CrossCursor);
    update();
}

void PlotWidget::cancelRectangleMode() {
    drawing_rectangle_ = false;
    rect_points_.clear();
    setCursor(Qt::ArrowCursor);
    update();
}

void PlotWidget::setDarkMode(bool enabled) {
    dark_mode_ = enabled;
    
    QPalette pal = palette();
    if (planner_preview_mode_) {
        pal.setColor(QPalette::Window, dark_mode_ ? QColor(QStringLiteral("#09090B")) : Qt::white);
        pal.setColor(QPalette::WindowText, dark_mode_ ? Qt::white : Qt::black);
    } else if (dark_mode_) {
        pal.setColor(QPalette::Window, QColor(30, 30, 35));
        pal.setColor(QPalette::WindowText, Qt::white);
    } else {
        pal.setColor(QPalette::Window, Qt::white);
        pal.setColor(QPalette::WindowText, Qt::black);
    }
    setPalette(pal);
    update();
}

void PlotWidget::setPlannerPreviewMode(bool enabled) {
    planner_preview_mode_ = enabled;

    QPalette pal = palette();
    if (planner_preview_mode_) {
        pal.setColor(QPalette::Window, dark_mode_ ? QColor(QStringLiteral("#09090B")) : Qt::white);
        pal.setColor(QPalette::WindowText, dark_mode_ ? Qt::white : Qt::black);
    } else if (dark_mode_) {
        pal.setColor(QPalette::Window, QColor(30, 30, 35));
        pal.setColor(QPalette::WindowText, Qt::white);
    } else {
        pal.setColor(QPalette::Window, Qt::white);
        pal.setColor(QPalette::WindowText, Qt::black);
    }
    setPalette(pal);
    update();
}

double PlotWidget::distanceToLineSegment(const QPointF& mouse, const QPointF& p1, const QPointF& p2) const {
    double dx = p2.x() - p1.x();
    double dy = p2.y() - p1.y();
    double len_sq = dx * dx + dy * dy;
    
    if (len_sq < 1e-10) {
        // Degenerate line (points are the same)
        return std::hypot(mouse.x() - p1.x(), mouse.y() - p1.y());
    }
    
    // Project mouse onto line, clamped to segment
    double t = std::max(0.0, std::min(1.0, 
        ((mouse.x() - p1.x()) * dx + (mouse.y() - p1.y()) * dy) / len_sq));
    
    double proj_x = p1.x() + t * dx;
    double proj_y = p1.y() + t * dy;
    
    return std::hypot(mouse.x() - proj_x, mouse.y() - proj_y);
}

void PlotWidget::clearAll() {
    points_.clear();
    polygon_.clear();
    roi_.clear();
    obstacles_.clear();
    swaths_.clear();
    route_.clear();
    path_.clear();
    robot_pose_.reset();
    robot_trail_.clear();
    custom_waypoints_.clear();
    custom_waypoint_states_.clear();
    scan_segments_.clear();
    scan_segment_labels_.clear();
    scan_segment_lengths_.clear();
    scan_segment_turns_.clear();
    scan_segment_selected_.clear();
    show_scan_segments_ = false;
    hovered_scan_segment_ = -1;
    active_scan_segment_ = -1;
    reproj_lines_.clear();
    hovered_reproj_index_ = -1;
    selection_points_.clear();
    selecting_ = false;
    selected_obstacle_idx_ = -1;
    emit obstacleSelectionChanged(-1);
    update();
}

void PlotWidget::clearPoints() { points_.clear(); update(); }
void PlotWidget::clearPolygon() { polygon_.clear(); update(); }
void PlotWidget::clearROI() { roi_.clear(); update(); }
void PlotWidget::clearObstacles() {
    obstacles_.clear();
    selected_obstacle_idx_ = -1;
    emit obstacleSelectionChanged(-1);
    update();
}
void PlotWidget::clearSwaths() { swaths_.clear(); update(); }
void PlotWidget::clearRoute() { route_.clear(); update(); }
void PlotWidget::clearPath() { path_.clear(); update(); }

void PlotWidget::resetView() {
    updateDataBounds();
    fitToData();
    update();
}

void PlotWidget::zoomIn() {
    scale_ *= 1.2;
    update();
}

void PlotWidget::zoomOut() {
    scale_ /= 1.2;
    update();
}

void PlotWidget::startROISelection() {
    selecting_ = true;
    selecting_roi_ = true;
    selection_points_.clear();
    setCursor(Qt::CrossCursor);
    update();
}

void PlotWidget::startObstacleSelection() {
    selecting_ = true;
    selecting_roi_ = false;
    selection_points_.clear();
    setCursor(Qt::CrossCursor);
    update();
}

void PlotWidget::finishSelection() {
    if (!selecting_ || selection_points_.size() < 3) {
        cancelSelection();
        return;
    }
    
    selecting_ = false;
    setCursor(Qt::ArrowCursor);
    
    Polygon2D poly = selection_points_;
    selection_points_.clear();
    
    if (selecting_roi_) {
        emit roiSelected(poly);
    } else {
        emit obstacleSelected(poly);
    }
    
    update();
}

void PlotWidget::cancelSelection() {
    selecting_ = false;
    selection_points_.clear();
    setCursor(Qt::ArrowCursor);
    emit selectionCancelled();
    update();
}

void PlotWidget::undoLastPoint() {
    if (!selection_points_.empty()) {
        selection_points_.pop_back();
        update();
    }
}

Polygon2D PlotWidget::getSelectedPolygon() const {
    return selection_points_;
}

void PlotWidget::clearObstacleSelection() {
    if (selected_obstacle_idx_ != -1) {
        selected_obstacle_idx_ = -1;
        emit obstacleSelectionChanged(-1);
        update();
    }
}

void PlotWidget::updateDataBounds() {
    data_min_x_ = data_min_y_ = std::numeric_limits<double>::max();
    data_max_x_ = data_max_y_ = std::numeric_limits<double>::lowest();
    
    auto updateBounds = [&](const Point2D& p) {
        data_min_x_ = std::min(data_min_x_, p.x);
        data_max_x_ = std::max(data_max_x_, p.x);
        data_min_y_ = std::min(data_min_y_, p.y);
        data_max_y_ = std::max(data_max_y_, p.y);
    };
    
    for (const auto& p : points_) updateBounds(p);
    for (const auto& p : polygon_) updateBounds(p);
    for (const auto& p : roi_) updateBounds(p);
    for (const auto& obs : obstacles_) {
        for (const auto& p : obs.outer) updateBounds(p);
        for (const auto& hole : obs.holes) {
            for (const auto& p : hole) updateBounds(p);
        }
    }
    for (const auto& sw : swaths_) {
        updateBounds(sw.start);
        updateBounds(sw.end);
    }
    for (const auto& st : path_) updateBounds(st.point);
    for (const auto& seg : scan_segments_) {
        for (const auto& st : seg) {
            updateBounds(st.point);
        }
    }
    if (robot_pose_.has_value()) {
        updateBounds(robot_pose_->point);
    }
    for (const auto& trail_pt : robot_trail_) {
        updateBounds(trail_pt);
    }
    for (const auto& wp : custom_waypoints_) {
        updateBounds(wp);
    }
    
    if (data_min_x_ > data_max_x_) {
        // Default to a 10m x 10m view centered at origin when no data
        data_min_x_ = -5; data_max_x_ = 5;
        data_min_y_ = -5; data_max_y_ = 5;
    }
    
    // Add margin
    const double margin_ratio = planner_preview_mode_ ? 0.025 : 0.05;
    double margin_x = (data_max_x_ - data_min_x_) * margin_ratio;
    double margin_y = (data_max_y_ - data_min_y_) * margin_ratio;
    data_min_x_ -= margin_x;
    data_max_x_ += margin_x;
    data_min_y_ -= margin_y;
    data_max_y_ += margin_y;
}

void PlotWidget::fitToData() {
    updateDataBounds();
    
    double data_w = data_max_x_ - data_min_x_;
    double data_h = data_max_y_ - data_min_y_;
    
    if (data_w < 1e-10) data_w = 1;
    if (data_h < 1e-10) data_h = 1;
    
    const double canvas_padding = planner_preview_mode_ ? 18.0 : 40.0;
    double scale_x = std::max(1.0, width() - canvas_padding) / data_w;
    double scale_y = std::max(1.0, height() - canvas_padding) / data_h;
    scale_ = std::min(scale_x, scale_y);
    
    double center_x = (data_min_x_ + data_max_x_) / 2;
    double center_y = (data_min_y_ + data_max_y_) / 2;
    
    offset_x_ = width() / 2 - center_x * scale_;
    offset_y_ = height() / 2 + center_y * scale_;  // Y flipped
}

QPointF PlotWidget::worldToScreen(const Point2D& p) const {
    return QPointF(p.x * scale_ + offset_x_, -p.y * scale_ + offset_y_);
}

Point2D PlotWidget::screenToWorld(const QPointF& p) const {
    return Point2D((p.x() - offset_x_) / scale_, -(p.y() - offset_y_) / scale_);
}

void PlotWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    
    QPainter painter(this);
    const bool dense_planner_points = planner_preview_mode_ && points_.size() > 60000;
    painter.setRenderHint(QPainter::Antialiasing, !dense_planner_points);
    
    // Background
    painter.fillRect(rect(),
                     planner_preview_mode_ ? (dark_mode_ ? QColor(QStringLiteral("#09090B"))
                                                         : QColor(Qt::white))
                                           : (dark_mode_ ? QColor(30, 30, 35)
                                                         : QColor(Qt::white)));
    
    // Draw grid
    if (!planner_preview_mode_) {
        QColor grid_color = dark_mode_ ? QColor(60, 60, 70) : QColor(200, 200, 200);
        painter.setPen(QPen(grid_color, 1, Qt::DashLine));
        const double visible_span =
            std::max(1e-6, std::max(data_max_x_ - data_min_x_, data_max_y_ - data_min_y_));
        const double grid_step = std::pow(10, std::floor(std::log10(visible_span / 5)));
        for (double x = std::floor(data_min_x_ / grid_step) * grid_step; x <= data_max_x_; x += grid_step) {
            QPointF p1 = worldToScreen(Point2D(x, data_min_y_));
            QPointF p2 = worldToScreen(Point2D(x, data_max_y_));
            painter.drawLine(p1, p2);
        }
        for (double y = std::floor(data_min_y_ / grid_step) * grid_step; y <= data_max_y_; y += grid_step) {
            QPointF p1 = worldToScreen(Point2D(data_min_x_, y));
            QPointF p2 = worldToScreen(Point2D(data_max_x_, y));
            painter.drawLine(p1, p2);
        }
    }
    
    // Draw points
    if (!points_.empty()) {
        const QColor point_color =
            planner_preview_mode_
                ? (dark_mode_ ? QColor(0, 212, 146, points_.size() > 140000 ? 160 : 210)
                              : QColor(5, 150, 105, points_.size() > 140000 ? 165 : 215))
                : QColor(100, 100, 100, 150);
        if (dense_planner_points) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(point_color, points_.size() > 140000 ? 1.0 : 2.0));
            for (const auto& p : points_) {
                painter.drawPoint(worldToScreen(p).toPoint());
            }
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(point_color);
            const qreal point_radius = planner_preview_mode_ ? 1.0 : 2.0;
            for (const auto& p : points_) {
                QPointF sp = worldToScreen(p);
                painter.drawEllipse(sp, point_radius, point_radius);
            }
        }
    }
    
    // Draw polygon
    if (!polygon_.empty()) {
        const QColor hull_pen =
            planner_preview_mode_
                ? (dark_mode_ ? QColor(QStringLiteral("#34D399"))
                              : QColor(QStringLiteral("#059669")))
                : QColor(Qt::black);
        const QColor hull_fill =
            planner_preview_mode_
                ? QColor(dark_mode_ ? QStringLiteral("#00D492") : QStringLiteral("#059669"))
                : QColor(Qt::transparent);
        painter.setPen(planner_preview_mode_ ? QPen(hull_pen, 1.6) : QPen(hull_pen, 2));
        painter.setBrush(planner_preview_mode_ ? QColor(hull_fill.red(),
                                                        hull_fill.green(),
                                                        hull_fill.blue(),
                                                        dark_mode_ ? 18 : 14)
                                              : Qt::NoBrush);
        QPolygonF poly_qp;
        for (const auto& p : polygon_) {
            poly_qp << worldToScreen(p);
        }
        poly_qp << poly_qp.first();  // Close polygon
        painter.drawPolygon(poly_qp);
    }
    
    // Draw ROI
    if (!roi_.empty()) {
        const QColor roi_pen = planner_preview_mode_
                                   ? (dark_mode_ ? QColor(QStringLiteral("#60A5FA"))
                                                 : QColor(QStringLiteral("#2563EB")))
                                   : QColor(Qt::green);
        painter.setPen(QPen(roi_pen, planner_preview_mode_ ? 1.8 : 2.0, Qt::DashLine));
        painter.setBrush(planner_preview_mode_ ? QColor(roi_pen.red(),
                                                        roi_pen.green(),
                                                        roi_pen.blue(),
                                                        dark_mode_ ? 32 : 24)
                                              : Qt::NoBrush);
        QPolygonF roi_qp;
        for (const auto& p : roi_) {
            roi_qp << worldToScreen(p);
        }
        roi_qp << roi_qp.first();
        painter.drawPolygon(roi_qp);
    }
    
    // Draw obstacles
    for (size_t oi = 0; oi < obstacles_.size(); ++oi) {
        const auto& obs = obstacles_[oi];
        const bool selected = (static_cast<int>(oi) == selected_obstacle_idx_);
        const QColor obstacle_pen =
            planner_preview_mode_
                ? (selected ? QColor(QStringLiteral("#FBBF24"))
                            : (dark_mode_ ? QColor(QStringLiteral("#F97316"))
                                          : QColor(QStringLiteral("#EA580C"))))
                : (selected ? QColor(255, 193, 7) : QColor(Qt::red));
        const QColor obstacle_fill =
            planner_preview_mode_
                ? QColor(obstacle_pen.red(),
                         obstacle_pen.green(),
                         obstacle_pen.blue(),
                         selected ? 62 : (dark_mode_ ? 42 : 34))
                : (selected ? QColor(255, 193, 7, 55) : QColor(255, 0, 0, 50));
        painter.setPen(QPen(obstacle_pen, selected ? 3 : (planner_preview_mode_ ? 1.8 : 2.0)));
        painter.setBrush(obstacle_fill);
        QPainterPath path;
        path.setFillRule(Qt::OddEvenFill);
        QPolygonF outer_qp;
        for (const auto& p : obs.outer) {
            outer_qp << worldToScreen(p);
        }
        if (!outer_qp.isEmpty()) {
            outer_qp << outer_qp.first();
            path.addPolygon(outer_qp);
        }
        for (const auto& hole : obs.holes) {
            QPolygonF hole_qp;
            for (const auto& p : hole) {
                hole_qp << worldToScreen(p);
            }
            if (!hole_qp.isEmpty()) {
                hole_qp << hole_qp.first();
                path.addPolygon(hole_qp);
            }
        }
        painter.drawPath(path);
    }
    
    // Draw swaths
    if (!swaths_.empty()) {
        painter.setPen(planner_preview_mode_
                           ? QPen(dark_mode_ ? QColor(QStringLiteral("#34D399"))
                                             : QColor(QStringLiteral("#059669")),
                                  1.3,
                                  Qt::DashLine)
                           : QPen(Qt::blue, 1, Qt::DashLine));
        for (const auto& sw : swaths_) {
            QPointF p1 = worldToScreen(sw.start);
            QPointF p2 = worldToScreen(sw.end);
            painter.drawLine(p1, p2);
        }
    }
    
    // Draw route
    if (!planner_preview_mode_ && !route_.empty()) {
        painter.setPen(QPen(QColor(255, 140, 0), 1.5));
        for (size_t i = 1; i < route_.size(); ++i) {
            QPointF p1 = worldToScreen(route_[i-1].point);
            QPointF p2 = worldToScreen(route_[i].point);
            painter.drawLine(p1, p2);
        }
    }
    
    // Draw path
    if (!path_.empty()) {
        const QColor path_color = planner_preview_mode_
                                      ? (dark_mode_ ? QColor(QStringLiteral("#34D399"))
                                                    : QColor(QStringLiteral("#059669")))
                                      : QColor(Qt::red);
        painter.setPen(QPen(path_color, planner_preview_mode_ ? 1.8 : 1.5));
        for (size_t i = 1; i < path_.size(); ++i) {
            QPointF p1 = worldToScreen(path_[i-1].point);
            QPointF p2 = worldToScreen(path_[i].point);
            painter.drawLine(p1, p2);
        }
        
        // Start and end markers
        if (!path_.empty()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(planner_preview_mode_
                                 ? QColor(dark_mode_ ? QStringLiteral("#34D399")
                                                     : QStringLiteral("#059669"))
                                 : QColor(Qt::green));
            QPointF start = worldToScreen(path_.front().point);
            painter.drawEllipse(start, 6, 6);

            if (!planner_preview_mode_) {
                painter.setBrush(Qt::red);
                QPointF end = worldToScreen(path_.back().point);
                painter.drawRect(QRectF(end.x() - 5, end.y() - 5, 10, 10));
            }
        }
    }

    // Draw scan segments
    if (show_scan_segments_ && !scan_segments_.empty()) {
        static const QVector<QColor> palette = {
            QColor("#1f77b4"), QColor("#ff7f0e"), QColor("#2ca02c"),
            QColor("#d62728"), QColor("#9467bd"), QColor("#8c564b"),
            QColor("#e377c2"), QColor("#7f7f7f"), QColor("#bcbd22"), QColor("#17becf")
        };
        // Stage 4 overlay colors (Figma-driven). Active = solid green up to
        // robot's projected point, dashed grey ahead. Completed = solid
        // green. Pending = dashed grey. Unselected = faded grey.
        const QColor kOverlayCompleted(QStringLiteral("#10B981"));
        const QColor kOverlayActive(QStringLiteral("#10B981"));
        const QColor kOverlayPending(QStringLiteral("#9F9FA9"));
        const QColor kOverlayUnselected =
            dark_mode_ ? QColor(QStringLiteral("#52525B"))
                       : QColor(QStringLiteral("#D4D4D8"));
        const bool overlay_mode =
            scan_segment_statuses_.size() == scan_segments_.size();

        for (int i = 0; i < static_cast<int>(scan_segments_.size()); ++i) {
            const auto& seg = scan_segments_[i];
            if (seg.size() < 2) continue;
            const bool is_selected =
                i < static_cast<int>(scan_segment_selected_.size())
                    ? scan_segment_selected_[static_cast<size_t>(i)]
                    : true;
            QColor base = palette[i % palette.size()];
            bool hovered = (i == hovered_scan_segment_) || (i == active_scan_segment_);
            QColor penColor;
            if (!is_selected) {
                penColor = dark_mode_ ? QColor(QStringLiteral("#71717B"))
                                      : QColor(QStringLiteral("#9CA3AF"));
                penColor.setAlpha(190);
            } else {
                penColor = hovered ? base.lighter(130) : base;
                penColor.setAlpha(hovered ? 255 : 210);
            }
            const double pen_width = !is_selected ? 2.0 : (hovered ? 4.0 : 3.0);

            if (overlay_mode) {
                // Status-driven render. Selected gating is preserved — an
                // unselected segment in overlay mode renders as faded
                // dashed grey regardless of status.
                const ScanSegmentStatus status =
                    scan_segment_statuses_[static_cast<size_t>(i)];
                if (!is_selected) {
                    QPen unsel_pen(kOverlayUnselected, 2.0,
                                   Qt::DashLine, Qt::RoundCap);
                    painter.setPen(unsel_pen);
                    for (size_t j = 1; j < seg.size(); ++j) {
                        painter.drawLine(worldToScreen(seg[j - 1].point),
                                         worldToScreen(seg[j].point));
                    }
                    penColor = kOverlayUnselected;
                } else if (status == ScanSegmentStatus::Completed) {
                    QPen p(kOverlayCompleted, hovered ? 4.0 : 3.0,
                           Qt::SolidLine, Qt::RoundCap);
                    painter.setPen(p);
                    for (size_t j = 1; j < seg.size(); ++j) {
                        painter.drawLine(worldToScreen(seg[j - 1].point),
                                         worldToScreen(seg[j].point));
                    }
                    penColor = kOverlayCompleted;
                } else if (status == ScanSegmentStatus::Pending) {
                    QPen p(kOverlayPending, hovered ? 3.5 : 2.5,
                           Qt::DashLine, Qt::RoundCap);
                    painter.setPen(p);
                    for (size_t j = 1; j < seg.size(); ++j) {
                        painter.drawLine(worldToScreen(seg[j - 1].point),
                                         worldToScreen(seg[j].point));
                    }
                    penColor = kOverlayPending;
                } else {
                    // Active: split polyline at the cumulative-length
                    // distance corresponding to scan_active_progress_pct_.
                    // Walk legs accumulating; when cumulative crosses the
                    // target, interpolate within that leg for a smooth
                    // split rather than snapping to the leg boundary.
                    double total_len = 0.0;
                    for (size_t j = 1; j < seg.size(); ++j) {
                        total_len += std::hypot(
                            seg[j].point.x - seg[j - 1].point.x,
                            seg[j].point.y - seg[j - 1].point.y);
                    }
                    const double target =
                        (scan_active_progress_pct_ / 100.0) * total_len;
                    QPen solid_pen(kOverlayActive, hovered ? 4.0 : 3.0,
                                   Qt::SolidLine, Qt::RoundCap);
                    QPen dashed_pen(kOverlayPending, hovered ? 3.5 : 2.5,
                                    Qt::DashLine, Qt::RoundCap);
                    double cumulative = 0.0;
                    for (size_t j = 1; j < seg.size(); ++j) {
                        const auto& a = seg[j - 1].point;
                        const auto& b = seg[j].point;
                        const double leg = std::hypot(b.x - a.x, b.y - a.y);
                        const double leg_end = cumulative + leg;
                        if (target <= 0.0) {
                            painter.setPen(dashed_pen);
                            painter.drawLine(worldToScreen(a),
                                             worldToScreen(b));
                        } else if (leg_end <= target || leg < 1e-9) {
                            painter.setPen(solid_pen);
                            painter.drawLine(worldToScreen(a),
                                             worldToScreen(b));
                        } else if (cumulative >= target) {
                            painter.setPen(dashed_pen);
                            painter.drawLine(worldToScreen(a),
                                             worldToScreen(b));
                        } else {
                            const double t = (target - cumulative) / leg;
                            const Point2D split{a.x + t * (b.x - a.x),
                                                a.y + t * (b.y - a.y)};
                            painter.setPen(solid_pen);
                            painter.drawLine(worldToScreen(a),
                                             worldToScreen(split));
                            painter.setPen(dashed_pen);
                            painter.drawLine(worldToScreen(split),
                                             worldToScreen(b));
                        }
                        cumulative = leg_end;
                    }
                    penColor = kOverlayActive;
                }
                penColor.setAlpha(hovered ? 255 : 220);
            } else {
                QPen pen(penColor, pen_width, Qt::SolidLine, Qt::RoundCap);
                painter.setPen(pen);
                for (size_t j = 1; j < seg.size(); ++j) {
                    painter.drawLine(worldToScreen(seg[j - 1].point),
                                     worldToScreen(seg[j].point));
                }
            }
            // Start/end markers
            painter.setPen(Qt::NoPen);
            painter.setBrush(is_selected ? Qt::white : QColor(QStringLiteral("#D4D4D8")));
            QPointF start = worldToScreen(seg.front().point);
            painter.drawEllipse(start, is_selected ? 4.0 : 3.0, is_selected ? 4.0 : 3.0);
            painter.setBrush(penColor);
            painter.drawEllipse(start, is_selected ? 7.0 : 5.0, is_selected ? 7.0 : 5.0);

            painter.setBrush(is_selected ? Qt::white : QColor(QStringLiteral("#D4D4D8")));
            QPointF end = worldToScreen(seg.back().point);
            const double inner = is_selected ? 8.0 : 6.0;
            const double outer = is_selected ? 12.0 : 9.0;
            painter.drawRect(QRectF(end.x() - inner / 2.0, end.y() - inner / 2.0, inner, inner));
            painter.setBrush(penColor);
            painter.drawRect(QRectF(end.x() - outer / 2.0, end.y() - outer / 2.0, outer, outer));

            // Label with icon at midpoint
            QPointF mid = worldToScreen(seg[seg.size() / 2].point);
            painter.setPen(is_selected
                               ? (dark_mode_ ? Qt::white : Qt::black)
                               : QColor(QStringLiteral("#9F9FA9")));
            painter.setFont(QFont("Sans Serif", 8, QFont::Bold));
            QString label = (i < static_cast<int>(scan_segment_labels_.size()))
                ? scan_segment_labels_[i] : QString("Segment %1").arg(i + 1);
            double len = (i < static_cast<int>(scan_segment_lengths_.size())) ? scan_segment_lengths_[i] : 0.0;
            int turns = (i < static_cast<int>(scan_segment_turns_.size())) ? scan_segment_turns_[i] : 0;
            painter.drawText(mid + QPointF(8, -8),
                             QString("🛰 %1 • %2 m • %3 turns").arg(label).arg(len, 0, 'f', 1).arg(turns));
        }
    }
    
    // Draw custom waypoint path
    if (show_custom_path_ && !custom_waypoints_.empty()) {
        painter.setPen(QPen(QColor(0, 150, 136), 2, Qt::SolidLine, Qt::RoundCap));
        for (size_t i = 1; i < custom_waypoints_.size(); ++i) {
            painter.drawLine(worldToScreen(custom_waypoints_[i-1]),
                             worldToScreen(custom_waypoints_[i]));
        }
        
        painter.setFont(QFont("Sans Serif", 8));
        for (size_t i = 0; i < custom_waypoints_.size(); ++i) {
            bool visited = (i < custom_waypoint_states_.size()) && custom_waypoint_states_[i];
            painter.setPen(QPen(Qt::black, 1));
            painter.setBrush(visited ? QColor(76, 175, 80) : QColor(0, 188, 212));
            QPointF pt = worldToScreen(custom_waypoints_[i]);
            painter.drawEllipse(pt, 5, 5);
            
            QString label = QString("#%1 (%2, %3)")
                .arg(i + 1)
                .arg(custom_waypoints_[i].x, 0, 'f', 2)
                .arg(custom_waypoints_[i].y, 0, 'f', 2);
            painter.drawText(pt + QPointF(8, -6), label);
        }
    }
    
    // Draw robot trail (live position history)
    if (!robot_trail_.empty()) {
        const QColor trail_color = planner_preview_mode_
                                       ? (dark_mode_ ? QColor(56, 189, 248, 190)
                                                     : QColor(37, 99, 235, 200))
                                       : QColor(30, 144, 255, 180);
        painter.setPen(QPen(trail_color,
                            planner_preview_mode_ ? 1.5 : 2.0,
                            Qt::SolidLine,
                            Qt::RoundCap));
        for (size_t i = 1; i < robot_trail_.size(); ++i) {
            painter.drawLine(worldToScreen(robot_trail_[i-1]),
                             worldToScreen(robot_trail_[i]));
        }
    }
    
    if (robot_pose_.has_value()) {
        const auto& pose = robot_pose_.value();
        const double base = robot_marker_size_;
        const double wing = base * 0.6;
        
        Point2D tip(
            pose.point.x + base * std::cos(pose.heading),
            pose.point.y + base * std::sin(pose.heading));
        Point2D left(
            pose.point.x + wing * std::cos(pose.heading + 2.5),
            pose.point.y + wing * std::sin(pose.heading + 2.5));
        Point2D right(
            pose.point.x + wing * std::cos(pose.heading - 2.5),
            pose.point.y + wing * std::sin(pose.heading - 2.5));
        
        QPolygonF tri;
        tri << worldToScreen(tip)
            << worldToScreen(left)
            << worldToScreen(right);
        
        painter.setPen(planner_preview_mode_
                           ? QPen(dark_mode_ ? QColor(QStringLiteral("#E4E4E7"))
                                             : QColor(QStringLiteral("#18181B")),
                                  1.5)
                           : QPen(QColor(128, 0, 128), 2));
        painter.setBrush(planner_preview_mode_ ? QColor(dark_mode_ ? QStringLiteral("#00D492")
                                                                   : QStringLiteral("#059669"))
                                               : QColor(255, 192, 203, 230));
        painter.drawPolygon(tri);

        if (!planner_preview_mode_) {
            QPointF center = worldToScreen(pose.point);
            painter.setPen(QPen(Qt::black, 1));
            painter.setFont(QFont("Sans Serif", 8, QFont::Bold));
            painter.drawText(center + QPointF(8, -8), "Robot");
        }
    }
    
    // Draw origin marker (robot position at 0,0)
    {
        QPointF origin = worldToScreen(Point2D(0.0, 0.0));
        
        // Check if origin is within view bounds (with some margin)
        if (origin.x() > -50 && origin.x() < width() + 50 &&
            origin.y() > -50 && origin.y() < height() + 50) {
            
            const QColor origin_color = QColor(220, 20, 60);
            const qreal crosshair_radius = planner_preview_mode_ ? 12.0 : 18.0;
            const qreal circle_radius = planner_preview_mode_ ? 9.0 : 12.0;

            // Draw crosshairs
            painter.setPen(QPen(origin_color, 2));
            painter.drawLine(origin.x() - crosshair_radius, origin.y(),
                             origin.x() + crosshair_radius, origin.y());
            painter.drawLine(origin.x(), origin.y() - crosshair_radius,
                             origin.x(), origin.y() + crosshair_radius);
            
            // Draw circle around origin
            painter.setPen(QPen(origin_color, 2));
            painter.setBrush(QColor(220, 20, 60, 40));
            painter.drawEllipse(origin, circle_radius, circle_radius);
            
            // Draw inner dot
            painter.setPen(Qt::NoPen);
            painter.setBrush(origin_color);
            painter.drawEllipse(origin, 3, 3);

            if (!planner_preview_mode_) {
                painter.setPen(origin_color);
                painter.setFont(QFont("Sans Serif", 9, QFont::Bold));
                painter.drawText(origin.x() + 16, origin.y() - 8, "Origin");
                painter.setFont(QFont("Sans Serif", 8));
                painter.drawText(origin.x() + 16, origin.y() + 6, "(0, 0)");
            }
        }
    }
    
    // Draw reprojection error lines
    if (!reproj_lines_.empty()) {
        for (size_t i = 0; i < reproj_lines_.size(); ++i) {
            const auto& line = reproj_lines_[i];
            QPointF wp_screen = worldToScreen(line.waypoint);
            QPointF tr_screen = worldToScreen(line.traversed);
            
            bool is_hovered = (static_cast<int>(i) == hovered_reproj_index_);
            
            // Draw line: red normally, green when hovered
            QPen pen(is_hovered ? QColor(0, 200, 0) : QColor(220, 50, 50));
            pen.setWidth(is_hovered ? 3 : 2);
            painter.setPen(pen);
            painter.drawLine(wp_screen, tr_screen);
            
            // Draw small circles at endpoints
            painter.setBrush(is_hovered ? QColor(0, 200, 0) : QColor(220, 50, 50));
            painter.drawEllipse(wp_screen, 4, 4);
            painter.drawEllipse(tr_screen, 4, 4);
            
            // If hovered, also show error text near the line
            if (is_hovered) {
                double error_cm = line.error_m * 100.0;
                QPointF mid((wp_screen.x() + tr_screen.x()) / 2,
                           (wp_screen.y() + tr_screen.y()) / 2);
                painter.setPen(Qt::white);
                painter.setFont(QFont("Arial", 10, QFont::Bold));
                
                // Draw background for readability
                QString text = QString("%1 cm").arg(error_cm, 0, 'f', 1);
                QRectF textRect = painter.fontMetrics().boundingRect(text);
                textRect.moveCenter(mid);
                textRect.adjust(-3, -2, 3, 2);
                painter.fillRect(textRect, QColor(0, 0, 0, 180));
                painter.drawText(textRect, Qt::AlignCenter, text);
            }
        }
    }
    
    // Draw selection in progress
    if (selecting_ && !selection_points_.empty()) {
        painter.setPen(QPen(Qt::magenta, 1.5));
        painter.setBrush(QColor(255, 0, 255, 30));
        
        QPolygonF sel_qp;
        for (const auto& p : selection_points_) {
            sel_qp << worldToScreen(p);
        }
        
        // Draw preview line to cursor
        if (cursor_pos_ != QPointF()) {
            sel_qp << cursor_pos_;
        }
        
        if (selection_points_.size() >= 3) {
            sel_qp << sel_qp.first();  // Close for fill
            painter.drawPolygon(sel_qp);
        } else if (selection_points_.size() >= 2) {
            painter.setBrush(Qt::NoBrush);
            painter.drawPolyline(sel_qp);
        }
        
        // Draw points
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::magenta);
        for (const auto& p : selection_points_) {
            QPointF sp = worldToScreen(p);
            painter.drawEllipse(sp, 4, 4);
        }
    }
    
    // Draw rectangle in progress (3-click tool)
    if (drawing_rectangle_ && !rect_points_.empty()) {
        painter.setPen(QPen(QColor(0, 150, 255), 2, Qt::DashLine));
        painter.setBrush(QColor(0, 150, 255, 40));
        
        if (rect_points_.size() == 1) {
            // First point set - draw line to cursor
            QPointF p1 = worldToScreen(rect_points_[0]);
            painter.drawLine(p1, cursor_pos_);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 150, 255));
            painter.drawEllipse(p1, 6, 6);
        } else if (rect_points_.size() == 2) {
            // Two points set - show rectangle preview based on cursor position
            Point2D p1 = rect_points_[0];
            Point2D p2 = rect_points_[1];
            Point2D cursor_world = screenToWorld(cursor_pos_);
            
            // Calculate perpendicular direction
            double dx = p2.x - p1.x;
            double dy = p2.y - p1.y;
            double len = std::hypot(dx, dy);
            if (len > 1e-6) {
                double perp_x = -dy / len;
                double perp_y = dx / len;
                
                // Project cursor onto perpendicular to get width
                double width = (cursor_world.x - p1.x) * perp_x + (cursor_world.y - p1.y) * perp_y;
                
                // Calculate 4 corners
                Point2D c1 = p1;
                Point2D c2 = p2;
                Point2D c3 = {p2.x + width * perp_x, p2.y + width * perp_y};
                Point2D c4 = {p1.x + width * perp_x, p1.y + width * perp_y};
                
                QPolygonF rect_preview;
                rect_preview << worldToScreen(c1) << worldToScreen(c2) 
                             << worldToScreen(c3) << worldToScreen(c4);
                rect_preview << rect_preview.first();
                
                painter.drawPolygon(rect_preview);
                
                // Draw dimension text
                painter.setPen(QColor(0, 150, 255));
                painter.setFont(QFont("Sans Serif", 9, QFont::Bold));
                QPointF mid1 = (worldToScreen(c1) + worldToScreen(c2)) / 2;
                QPointF mid2 = (worldToScreen(c1) + worldToScreen(c4)) / 2;
                painter.drawText(mid1 + QPointF(5, -5), QString("%1 m").arg(len, 0, 'f', 2));
                painter.drawText(mid2 + QPointF(5, -5), QString("%1 m").arg(std::abs(width), 0, 'f', 2));
            }
            
            // Draw corner points
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 150, 255));
            painter.drawEllipse(worldToScreen(p1), 6, 6);
            painter.drawEllipse(worldToScreen(p2), 6, 6);
        }
        
        // Draw instructions
        painter.setPen(dark_mode_ ? Qt::white : Qt::black);
        painter.setFont(QFont("Sans Serif", 9));
        QString hint;
        if (rect_points_.empty()) {
            hint = "Click first corner";
        } else if (rect_points_.size() == 1) {
            hint = "Click to define base edge direction";
        } else {
            hint = "Click to set rectangle width";
        }
        painter.drawText(10, height() - 10, hint);
    }
    
    // Draw title
    if (!planner_preview_mode_) {
        painter.setPen(dark_mode_ ? Qt::white : Qt::black);
        painter.setFont(QFont("Sans Serif", 10, QFont::Bold));
        painter.drawText(10, 20, "2D Projection / Coverage");
    }
    
    // Draw live stats overlay (top-right)
    if (!planner_preview_mode_ && show_live_overlay_ && !live_overlay_lines_.empty()) {
        QFont overlay_font("Sans Serif", 9);
        painter.setFont(overlay_font);
        QFontMetrics fm(overlay_font);
        
        int max_width = 0;
        for (const auto& line : live_overlay_lines_) {
            max_width = std::max(max_width, fm.horizontalAdvance(line));
        }
        int line_height = fm.height();
        int padding = 8;
        int box_w = max_width + padding * 2;
        int box_h = static_cast<int>(live_overlay_lines_.size()) * line_height + padding * 2;
        
        QRect box(rect().width() - box_w - 10, 10, box_w, box_h);
        QColor bg = dark_mode_ ? QColor(30, 30, 35, 200) : QColor(255, 255, 255, 220);
        QColor fg = dark_mode_ ? Qt::white : Qt::black;
        
        painter.setPen(Qt::NoPen);
        painter.setBrush(bg);
        painter.drawRoundedRect(box, 6, 6);
        
        painter.setPen(fg);
        int y = box.top() + padding + fm.ascent();
        for (const auto& line : live_overlay_lines_) {
            painter.drawText(box.left() + padding, y, line);
            y += line_height;
        }
    }
}

void PlotWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (custom_draw_mode_) {
            Point2D world = screenToWorld(event->pos());
            emit customWaypointRequested(world);
            return;
        }
        
        // Rectangle drawing mode (3-click)
        if (drawing_rectangle_) {
            Point2D world = screenToWorld(event->pos());
            rect_points_.push_back(world);
            
            if (rect_points_.size() >= 3) {
                // Complete the rectangle
                Point2D p1 = rect_points_[0];
                Point2D p2 = rect_points_[1];
                Point2D p3 = rect_points_[2];
                
                // Calculate perpendicular direction
                double dx = p2.x - p1.x;
                double dy = p2.y - p1.y;
                double len = std::hypot(dx, dy);
                
                if (len > 1e-6) {
                    double perp_x = -dy / len;
                    double perp_y = dx / len;
                    
                    // Project p3 onto perpendicular to get width
                    double width = (p3.x - p1.x) * perp_x + (p3.y - p1.y) * perp_y;
                    
                    // Build rectangle polygon
                    Polygon2D rect;
                    rect.push_back(p1);
                    rect.push_back(p2);
                    rect.push_back({p2.x + width * perp_x, p2.y + width * perp_y});
                    rect.push_back({p1.x + width * perp_x, p1.y + width * perp_y});
                    
                    // Exit rectangle mode and emit
                    drawing_rectangle_ = false;
                    rect_points_.clear();
                    setCursor(Qt::ArrowCursor);
                    emit rectangleCompleted(rect);
                }
            }
            update();
            return;
        }
        
        if (selecting_) {
            Point2D world = screenToWorld(event->pos());
            selection_points_.push_back(world);
            update();
        } else {
            // Click-to-select obstacles (only when not selecting/drawing)
            Point2D world = screenToWorld(event->pos());
            int hit_idx = -1;
            for (int i = static_cast<int>(obstacles_.size()) - 1; i >= 0; --i) {
                if (pointInObstacleShapeLocal(world, obstacles_[static_cast<size_t>(i)])) {
                    hit_idx = i;
                    break;
                }
            }
            if (hit_idx != -1) {
                if (selected_obstacle_idx_ != hit_idx) {
                    selected_obstacle_idx_ = hit_idx;
                    emit obstacleSelectionChanged(selected_obstacle_idx_);
                    update();
                }
                return;
            }
            if (selected_obstacle_idx_ != -1) {
                selected_obstacle_idx_ = -1;
                emit obstacleSelectionChanged(-1);
                update();
            }

            // Start panning
            panning_ = true;
            pan_start_ = event->pos();
            pan_offset_x_ = offset_x_;
            pan_offset_y_ = offset_y_;
            setCursor(Qt::ClosedHandCursor);
        }
    } else if (event->button() == Qt::RightButton) {
        if (drawing_rectangle_) {
            cancelRectangleMode();
        } else if (selecting_) {
            finishSelection();
        }
    } else if (event->button() == Qt::MiddleButton) {
        resetView();
    }
}

void PlotWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    // Double-click to finish polygon selection (ROI or obstacle)
    if (selecting_ && event->button() == Qt::LeftButton) {
        if (selection_points_.size() >= 3) {
            finishSelection();
        }
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void PlotWidget::keyPressEvent(QKeyEvent* event) {
    // Delete selected obstacle
    if (!selecting_ && !drawing_rectangle_) {
        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
            if (selected_obstacle_idx_ >= 0 &&
                selected_obstacle_idx_ < static_cast<int>(obstacles_.size())) {
                emit obstacleDeleteRequested(selected_obstacle_idx_);
                selected_obstacle_idx_ = -1;
                emit obstacleSelectionChanged(-1);
                update();
                return;
            }
        }
    }

    // Press Enter/Return to finish polygon selection
    if (selecting_) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (selection_points_.size() >= 3) {
                finishSelection();
            }
            return;
        } else if (event->key() == Qt::Key_Escape) {
            cancelSelection();
            return;
        }
    }
    
    // Escape to cancel rectangle mode
    if (drawing_rectangle_ && event->key() == Qt::Key_Escape) {
        cancelRectangleMode();
        return;
    }
    
    QWidget::keyPressEvent(event);
}

void PlotWidget::mouseMoveEvent(QMouseEvent* event) {
    cursor_pos_ = event->pos();
    
    if (panning_) {
        offset_x_ = pan_offset_x_ + (event->pos().x() - pan_start_.x());
        offset_y_ = pan_offset_y_ + (event->pos().y() - pan_start_.y());
        update();
    } else if (selecting_) {
        update();  // Redraw preview line
    }
    
    // Hover on scan segments
    if (show_scan_segments_ && !scan_segments_.empty()) {
        int old_hover = hovered_scan_segment_;
        hovered_scan_segment_ = -1;
        const double hover_threshold = 8.0;
        double best = hover_threshold;
        for (int i = 0; i < static_cast<int>(scan_segments_.size()); ++i) {
            const auto& seg = scan_segments_[i];
            for (size_t j = 1; j < seg.size(); ++j) {
                double dist = distanceToLineSegment(event->pos(),
                    worldToScreen(seg[j-1].point), worldToScreen(seg[j].point));
                if (dist < best) {
                    best = dist;
                    hovered_scan_segment_ = i;
                }
            }
        }
        if (hovered_scan_segment_ != old_hover) {
            update();
            if (hovered_scan_segment_ >= 0) {
                QString label = (hovered_scan_segment_ < scan_segment_labels_.size())
                    ? scan_segment_labels_[hovered_scan_segment_] : QString("Segment %1").arg(hovered_scan_segment_ + 1);
                double len = (hovered_scan_segment_ < scan_segment_lengths_.size())
                    ? scan_segment_lengths_[hovered_scan_segment_] : 0.0;
                int turns = (hovered_scan_segment_ < scan_segment_turns_.size())
                    ? scan_segment_turns_[hovered_scan_segment_] : 0;
                QString tip = QString("%1 — %2 m, %3 turns").arg(label).arg(len, 0, 'f', 1).arg(turns);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                QToolTip::showText(event->globalPosition().toPoint(), tip, this);
#else
                QToolTip::showText(event->globalPos(), tip, this);
#endif
                return;  // keep tooltip active
            }
        }
        if (hovered_scan_segment_ >= 0) {
            return;
        }
    }
    
    // Check hover on reprojection lines
    if (!reproj_lines_.empty()) {
        int old_hovered = hovered_reproj_index_;
        hovered_reproj_index_ = -1;
        
        const double hover_threshold = 8.0;  // pixels
        double min_dist = hover_threshold;
        
        for (size_t i = 0; i < reproj_lines_.size(); ++i) {
            QPointF wp_screen = worldToScreen(reproj_lines_[i].waypoint);
            QPointF tr_screen = worldToScreen(reproj_lines_[i].traversed);
            double dist = distanceToLineSegment(event->pos(), wp_screen, tr_screen);
            
            if (dist < min_dist) {
                min_dist = dist;
                hovered_reproj_index_ = static_cast<int>(i);
            }
        }
        
        if (hovered_reproj_index_ != old_hovered) {
            update();
            
            // Show reprojection tooltip if hovering a line
            if (hovered_reproj_index_ >= 0) {
                const auto& line = reproj_lines_[hovered_reproj_index_];
                double error_cm = line.error_m * 100.0;
                QString tip = QString("WP %1: %2 cm error")
                    .arg(line.waypoint_index + 1)
                    .arg(error_cm, 0, 'f', 1);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                QToolTip::showText(event->globalPosition().toPoint(), tip, this);
#else
                QToolTip::showText(event->globalPos(), tip, this);
#endif
                return;  // Don't show coordinate tooltip
            }
        }
        
        // If still hovering a reprojection line, keep showing its tooltip
        if (hovered_reproj_index_ >= 0) {
            return;
        }
    }
    
    // Show coordinates in tooltip
    Point2D world = screenToWorld(event->pos());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QToolTip::showText(event->globalPosition().toPoint(), 
                      QString("(%1, %2)").arg(world.x, 0, 'f', 2).arg(world.y, 0, 'f', 2));
#else
    QToolTip::showText(event->globalPos(), 
                      QString("(%1, %2)").arg(world.x, 0, 'f', 2).arg(world.y, 0, 'f', 2));
#endif
}

void PlotWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (panning_) {
            panning_ = false;
            setCursor(Qt::ArrowCursor);
        }
    }
}

void PlotWidget::wheelEvent(QWheelEvent* event) {
    double factor = (event->angleDelta().y() > 0) ? 1.2 : (1.0 / 1.2);
    
    // Zoom centered on cursor
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QPointF cursor = event->position();
#else
    QPointF cursor = event->posF();
#endif
    Point2D world_before = screenToWorld(cursor);
    
    scale_ *= factor;
    
    // Adjust offset to keep cursor position fixed
    offset_x_ = cursor.x() - world_before.x * scale_;
    offset_y_ = cursor.y() + world_before.y * scale_;
    
    update();
}

void PlotWidget::resizeEvent(QResizeEvent* event) {
    Q_UNUSED(event);
    // Optionally fit to data on resize
}

}  // namespace f2c_cpp
