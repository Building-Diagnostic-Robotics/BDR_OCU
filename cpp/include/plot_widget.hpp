#pragma once

#include "coverage_pipeline.hpp"
#include "touch_canvas_gestures.hpp"

#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <optional>
#include <vector>

#include <QString>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QTouchEvent;
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

    // Transient on-map measurement overlay (no coverage impact).
    enum class MeasureMode { None, Distance, Area };
    void startMeasure(MeasureMode mode);
    void clearMeasure();
    bool isMeasuring() const { return measure_mode_ != MeasureMode::None; }

    void fitToTrail();

    // Transient robot->start approach connector preview (dotted + arrowheads,
    // visually distinct from the solid coverage path). Empty pts clears it.
    void setApproachConnector(const std::vector<Point2D>& pts, bool unsafe);
    void setApproachObstacles(const std::vector<Obstacle2D>& obstacles);

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
    void startCutSelection();
    void startEraseMode();
    void clearEraseMode(bool notify_exited = true);
    bool isErasing() const { return erase_mode_; }
    void finishSelection();
    void cancelSelection();
    void undoLastPoint();

    bool isSelecting() const { return selection_purpose_ != SelectionPurpose::None; }
    Polygon2D getSelectedPolygon() const;
    int selectedObstacleIndex() const { return selected_obstacle_idx_; }
    void clearObstacleSelection();

signals:
    void roiSelected(const Polygon2D& roi);
    void obstacleSelected(const Polygon2D& obstacle);
    void cutRegionSelected(const Polygon2D& region);
    void selectionCancelled();
    void obstacleSelectionChanged(int index);
    void obstacleDeleteRequested(int index);
    void customWaypointRequested(const Point2D& point);
    void rectangleCompleted(const Polygon2D& rect);
    void measureCleared();
    void eraseModeExited();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    /** Touch pan / pinch. QTouchEvent has no dedicated virtual in Qt 5. */
    bool event(QEvent* event) override;

private:
    // Zoom clamp, as a multiple of the fit-to-data scale. The floor only
    // stops a pinch-out flinging the cloud into a dot; the ceiling has to
    // leave room to pick apart the centimetre-scale reprojection error
    // lines, which needs roughly 20x fit.
    static constexpr double kMinScaleOfFit = 1.0 / 8.0;
    static constexpr double kMaxScaleOfFit = 32.0;

    std::vector<Point2D> points_;
    // Cached density raster of `points_` (a responsive base layer drawn in
    // place of per-point ellipses). Rebuilt only when `points_` or the theme
    // change, so zoom/pan is a single blit instead of re-drawing every point.
    // `point_cloud_image_bounds_` is the world-space rect the image maps onto.
    QImage point_cloud_image_;
    QRectF point_cloud_image_bounds_;
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
    // The zoom clamp is relative to the fit-to-data scale, so it stays off
    // until the bounds mean something. Before the first updateDataBounds()
    // the placeholder 0..1 span implies a nonsense fit scale.
    bool data_bounds_valid_ = false;

    // Touch pan / pinch. A finger that stays inside the slop circle is
    // replayed as a left click, so obstacle selection and the measure /
    // rectangle / waypoint taps stay reachable by touch.
    touch_gestures::GestureState touch_;
    touch::SynthesizedMouseGuard touch_guard_;

    enum class SelectionPurpose { None, Roi, Obstacle, Cut };
    SelectionPurpose selection_purpose_ = SelectionPurpose::None;
    std::vector<Point2D> selection_points_;
    QPointF cursor_pos_;

    MeasureMode measure_mode_ = MeasureMode::None;
    std::vector<Point2D> measure_points_;
    bool measure_finished_ = false;

    std::vector<Point2D> approach_connector_;
    bool approach_connector_unsafe_ = false;
    std::vector<Obstacle2D> approach_obstacles_;

    bool erase_mode_ = false;
    int erase_hover_idx_ = -1;

    int selected_obstacle_idx_ = -1;

    bool panning_ = false;
    QPoint pan_start_;
    double pan_offset_x_, pan_offset_y_;

    QPointF worldToScreen(const Point2D& p) const;
    Point2D screenToWorld(const QPointF& p) const;
    void updateDataBounds();
    /** Scale at which the data bounds exactly fill the canvas — what
        fitToData() lands on, without mutating the view. Zero when the
        bounds are not meaningful yet. */
    double fitScale() const;
    /** Multiplies the zoom by `factor`, clamped, holding the world point
        under `anchor` fixed. Shared by the wheel and pinch. */
    void zoomAtScreenPoint(double factor, const QPointF& anchor);
    /** Touch pan / pinch. True when the event is consumed. */
    bool handleTouchGesture(QTouchEvent* event);
    // Rasterize `points_` into `point_cloud_image_` (density-modulated alpha,
    // theme-aware color). No-op (clears the image) when there are no points.
    void rebuildPointCloudImage();
    void fitToData();
    void drawActiveSelection(QPainter& painter);
    void drawMeasureOverlay(QPainter& painter);
    void drawApproachConnector(QPainter& painter);
    void drawApproachObstacles(QPainter& painter);
    int obstacleIndexAt(const Point2D& world) const;
    double distanceToLineSegment(const QPointF& mouse, const QPointF& p1, const QPointF& p2) const;
};

}  // namespace f2c_cpp
