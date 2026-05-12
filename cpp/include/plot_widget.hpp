#pragma once

#include "coverage_pipeline.hpp"

#include <QPoint>
#include <QPointF>
#include <QWidget>

#include <optional>
#include <vector>

#include <QString>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;

namespace f2c_cpp {

struct ReprojectionLine {
    Point2D waypoint;
    Point2D traversed;
    double error_m = 0;
    int waypoint_index = 0;
};

class PlotWidget : public QWidget {
    Q_OBJECT

public:
    explicit PlotWidget(QWidget* parent = nullptr);

    void setPoints(const std::vector<Point2D>& points);
    void setPolygon(const Polygon2D& poly);
    void setROI(const Polygon2D& roi);
    void setObstacles(const std::vector<Obstacle2D>& obstacles);
    void setSwaths(const SwathList& swaths);
    void setRoute(const PathStateList& route);
    void setPath(const PathStateList& path);
    void setRobotPose(const std::optional<PathState>& pose);
    void setRobotTrail(const std::vector<Point2D>& trail);
    void setRobotMarkerSize(double size_meters);
    void setCustomPath(const std::vector<Point2D>& path,
                       const std::vector<bool>& visited);
    void setShowCustomPath(bool show);
    void setCustomDrawMode(bool enabled);
    void setReprojectionLines(const std::vector<ReprojectionLine>& lines);
    void clearReprojectionLines();
    int getHoveredReprojectionIndex() const { return hovered_reproj_index_; }
    void setLiveOverlay(bool enabled, const std::vector<QString>& lines);
    void setScanSegments(const std::vector<PathStateList>& segments,
                         const std::vector<QString>& labels,
                         const std::vector<double>& lengths,
                         const std::vector<int>& turns,
                         bool visible,
                         const std::vector<bool>& selected = {});
    void setActiveScanSegment(int idx);

    enum class ScanSegmentStatus { Pending, Active, Completed };
    void setScanSegmentsOverlay(const std::vector<ScanSegmentStatus>& statuses,
                                double active_progress_pct);

    void startRectangleMode();
    void cancelRectangleMode();
    bool isDrawingRectangle() const { return drawing_rectangle_; }

    void setDarkMode(bool enabled);
    bool isDarkMode() const { return dark_mode_; }
    void setPlannerPreviewMode(bool enabled);

    // Link health overlay. When offline, paint an amber border around
    // the viewport plus a small "robot pose stale (Xs)" caption near
    // the robot marker so the operator visually knows the displayed
    // pose is the last-known one, not live. Driven by AppShellWindow's
    // LinkHealthMonitor — passes the seconds-since-disconnect for the
    // caption. since_ms<0 means "online" (clears the overlay).
    void setLinkOffline(bool offline, qint64 since_ms);

    void clearAll();
    void clearPoints();
    void clearPolygon();
    void clearROI();
    void clearObstacles();
    void clearSwaths();
    void clearRoute();
    void clearPath();

    void resetView();
    void zoomIn();
    void zoomOut();

    void startROISelection();
    void startObstacleSelection();
    void finishSelection();
    void cancelSelection();
    void undoLastPoint();

    bool isSelecting() const { return selecting_; }
    Polygon2D getSelectedPolygon() const;
    int selectedObstacleIndex() const { return selected_obstacle_idx_; }
    void clearObstacleSelection();

signals:
    void roiSelected(const Polygon2D& roi);
    void obstacleSelected(const Polygon2D& obstacle);
    void selectionCancelled();
    void obstacleSelectionChanged(int index);
    void obstacleDeleteRequested(int index);
    void customWaypointRequested(const Point2D& point);
    void rectangleCompleted(const Polygon2D& rect);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    std::vector<Point2D> points_;
    Polygon2D polygon_;
    Polygon2D roi_;
    std::vector<Obstacle2D> obstacles_;
    SwathList swaths_;
    PathStateList route_;
    PathStateList path_;
    std::optional<PathState> robot_pose_;
    std::vector<Point2D> robot_trail_;
    double robot_marker_size_ = 0.6;
    std::vector<Point2D> custom_waypoints_;
    std::vector<bool> custom_waypoint_states_;
    bool show_custom_path_ = false;
    bool custom_draw_mode_ = false;
    std::vector<PathStateList> scan_segments_;
    std::vector<QString> scan_segment_labels_;
    std::vector<double> scan_segment_lengths_;
    std::vector<int> scan_segment_turns_;
    std::vector<bool> scan_segment_selected_;
    bool show_scan_segments_ = false;
    int hovered_scan_segment_ = -1;
    int active_scan_segment_ = -1;
    std::vector<ScanSegmentStatus> scan_segment_statuses_;
    double scan_active_progress_pct_ = 0.0;

    std::vector<ReprojectionLine> reproj_lines_;
    int hovered_reproj_index_ = -1;
    bool show_live_overlay_ = false;
    std::vector<QString> live_overlay_lines_;

    bool drawing_rectangle_ = false;
    std::vector<Point2D> rect_points_;

    bool dark_mode_ = false;
    bool planner_preview_mode_ = false;
    bool link_offline_ = false;
    qint64 link_offline_since_ms_ = 0;

    double scale_ = 1.0;
    double offset_x_ = 0.0;
    double offset_y_ = 0.0;
    double data_min_x_ = 0, data_max_x_ = 1;
    double data_min_y_ = 0, data_max_y_ = 1;

    bool selecting_ = false;
    bool selecting_roi_ = false;
    std::vector<Point2D> selection_points_;
    QPointF cursor_pos_;

    int selected_obstacle_idx_ = -1;

    bool panning_ = false;
    QPoint pan_start_;
    double pan_offset_x_, pan_offset_y_;

    QPointF worldToScreen(const Point2D& p) const;
    Point2D screenToWorld(const QPointF& p) const;
    void updateDataBounds();
    void fitToData();
    double distanceToLineSegment(const QPointF& mouse, const QPointF& p1, const QPointF& p2) const;
};

}  // namespace f2c_cpp
