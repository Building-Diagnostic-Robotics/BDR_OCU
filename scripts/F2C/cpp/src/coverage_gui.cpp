/**
 * @file coverage_gui.cpp
 * @brief Qt6 GUI implementation for coverage planning
 */

#include "coverage_gui.hpp"
#include "transfer_manager.hpp"
#include "data_transfer_dialog.hpp"

#include <QApplication>
#include <QGridLayout>
#include <QStyle>
#include <QFont>
#include <QPen>
#include <QBrush>
#include <QPainterPath>
#include <QToolTip>
#include <QFileInfo>
#include <QFile>
#include <QFrame>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTabWidget>
#include <QStandardPaths>
#include <QSignalBlocker>
#include <QThread>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QStyleFactory>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstdlib>

// PCL for 3D point cloud preview
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <pcl/common/transforms.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/registration/icp.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>

namespace f2c_cpp {

// =============================================================================
// VideoStreamWidget Implementation
// =============================================================================

VideoStreamWidget::VideoStreamWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    
    // Set black background
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setAutoFillBackground(true);
    setPalette(pal);
    
    // Initialize GStreamer (safe to call multiple times)
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }
}

VideoStreamWidget::~VideoStreamWidget() {
    destroyPipeline();
}

void VideoStreamWidget::setupPipeline(int port) {
    destroyPipeline();
    
    // FPV-optimized low-latency receiver pipeline
    // - rtpjitterbuffer latency=20: Minimal 20ms buffer for stability
    // - do-lost=true: Handle packet loss gracefully
    // - Works with both old (640x480@15fps) and new (480x360@25fps) sender configs
    QString pipelineStr = QString(
        "udpsrc port=%1 buffer-size=212992 "
        "caps=\"application/x-rtp, media=video, encoding-name=H264, payload=96, clock-rate=90000\" "
        "! rtpjitterbuffer latency=20 do-lost=true "
        "! rtph264depay "
        "! h264parse "
        "! avdec_h264 "
        "! videoconvert "
        "! autovideosink sync=false name=videosink"
    ).arg(port);
    
    GError* error = nullptr;
    pipeline_ = gst_parse_launch(pipelineStr.toUtf8().constData(), &error);
    
    if (error) {
        QString errMsg = QString("Pipeline error: %1").arg(error->message);
        g_error_free(error);
        emit streamError(errMsg);
        std::cerr << "[VideoStream] " << errMsg.toStdString() << std::endl;
        return;
    }
    
    if (!pipeline_) {
        emit streamError("Failed to create pipeline");
        return;
    }
    
    // Set up bus sync handler to embed video in widget
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
    gst_bus_set_sync_handler(bus, busSyncHandler, this, nullptr);
    gst_object_unref(bus);
    
    // Start timer to poll bus messages (Qt-friendly approach instead of GLib main loop)
    if (!bus_poll_timer_) {
        bus_poll_timer_ = new QTimer(this);
        connect(bus_poll_timer_, &QTimer::timeout, this, &VideoStreamWidget::pollBus);
    }
    bus_poll_timer_->start(100);  // Poll every 100ms
    
    current_port_ = port;
    std::cout << "[VideoStream] Pipeline created for port " << port << std::endl;
}

GstBusSyncReply VideoStreamWidget::busSyncHandler(GstBus* bus, GstMessage* msg, gpointer data) {
    Q_UNUSED(bus);
    VideoStreamWidget* self = static_cast<VideoStreamWidget*>(data);
    
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT) {
        if (gst_is_video_overlay_prepare_window_handle_message(msg)) {
            // Embed video in our widget
            WId winId = self->winId();
            gst_video_overlay_set_window_handle(
                GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg)), 
                static_cast<guintptr>(winId)
            );
            gst_message_unref(msg);
            return GST_BUS_DROP;
        }
    }
    return GST_BUS_PASS;
}

void VideoStreamWidget::pollBus() {
    if (!pipeline_) return;
    
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
    if (!bus) return;
    
    GstMessage* msg;
    while ((msg = gst_bus_pop(bus)) != nullptr) {
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(msg, &err, &debug);
                QString errMsg = QString("Stream error: %1").arg(err ? err->message : "unknown");
                std::cerr << "[VideoStream] Error: " << errMsg.toStdString() << std::endl;
                if (debug) std::cerr << "[VideoStream] Debug: " << debug << std::endl;
                if (err) g_error_free(err);
                if (debug) g_free(debug);
                emit streamError(errMsg);
                break;
            }
            case GST_MESSAGE_EOS:
                std::cout << "[VideoStream] End of stream" << std::endl;
                playing_ = false;
                emit streamStopped();
                break;
            case GST_MESSAGE_STATE_CHANGED:
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
                    GstState old_state, new_state, pending_state;
                    gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                    std::cout << "[VideoStream] State: " << gst_element_state_get_name(old_state) 
                              << " -> " << gst_element_state_get_name(new_state) << std::endl;
                    if (new_state == GST_STATE_PLAYING && !playing_) {
                        playing_ = true;
                        emit streamStarted();
                    } else if (new_state == GST_STATE_NULL && playing_) {
                        playing_ = false;
                        emit streamStopped();
                    }
                }
                break;
            default:
                break;
        }
        gst_message_unref(msg);
    }
    
    gst_object_unref(bus);
}

// Remove the old callback - no longer used
gboolean VideoStreamWidget::busCallback(GstBus* bus, GstMessage* msg, gpointer data) {
    Q_UNUSED(bus);
    Q_UNUSED(msg);
    Q_UNUSED(data);
    return TRUE;
}

void VideoStreamWidget::startStream(int port) {
    setupPipeline(port);
    if (pipeline_) {
        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            emit streamError("Failed to start stream");
            std::cerr << "[VideoStream] Failed to set pipeline to PLAYING" << std::endl;
        } else {
            std::cout << "[VideoStream] Starting stream on port " << port << std::endl;
        }
    }
}

void VideoStreamWidget::stopStream() {
    // Fully destroy the pipeline to release all resources (including UDP socket)
    // This ensures clean restart when switching cameras
    destroyPipeline();
        emit streamStopped();
        std::cout << "[VideoStream] Stream stopped" << std::endl;
}

void VideoStreamWidget::destroyPipeline() {
    // Stop the bus polling timer first
    if (bus_poll_timer_) {
        bus_poll_timer_->stop();
    }
    
    if (pipeline_) {
        // Set pipeline to NULL state
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        
        // Wait for the state change to complete (with 1 second timeout)
        // This ensures resources like UDP sockets are fully released
        gst_element_get_state(pipeline_, nullptr, nullptr, GST_SECOND);
        
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        playing_ = false;
    }
}

void VideoStreamWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (auto_start_on_show_ && !playing_) {
        startStream(current_port_);
    }
}

void VideoStreamWidget::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    // Optionally stop when hidden to save resources
    // stopStream();
}

// =============================================================================
// PlotWidget and CoverageGUI Implementation
// =============================================================================

namespace {

constexpr double kWaypointDuplicateEpsilon = 1e-6;
constexpr double kConnectorObstacleClearanceM = 0.3;
constexpr const char* kOperationCancelledMessage = "Operation cancelled";

PathStateList dedupePathStates(const PathStateList& path) {
    PathStateList filtered;
    filtered.reserve(path.size());
    for (const auto& state : path) {
        if (!filtered.empty()) {
            double dx = state.point.x - filtered.back().point.x;
            double dy = state.point.y - filtered.back().point.y;
            if (std::fabs(dx) <= kWaypointDuplicateEpsilon &&
                std::fabs(dy) <= kWaypointDuplicateEpsilon) {
                continue;
            }
        }
        filtered.push_back(state);
    }
    return filtered;
}

static bool pointsAreDedupedEquivalent(const Point2D& a, const Point2D& b) {
    return std::fabs(a.x - b.x) <= kWaypointDuplicateEpsilon &&
           std::fabs(a.y - b.y) <= kWaypointDuplicateEpsilon;
}

static int countLeadingConnectorFlags(const std::vector<int>& flags) {
    int count = 0;
    while (count < static_cast<int>(flags.size()) && flags[static_cast<size_t>(count)] == 0) {
        ++count;
    }
    return count;
}

// Build dc flags for planned paths using a boundary-inclusive split:
// - connector-only prefix stays dc=0
// - collection starts at the connector->coverage boundary point
//   (last connector point / first coverage point in shared-boundary cases)
static std::vector<int> buildPlannedDataCollectionFlags(
    size_t path_size,
    bool collect_data,
    bool is_home_path,
    int connector_prefix_count) {
    std::vector<int> flags(path_size, 0);
    if (path_size == 0 || is_home_path || !collect_data) {
        return flags;
    }

    flags.assign(path_size, 1);
    if (connector_prefix_count <= 0) {
        return flags;
    }

    const size_t prefix = std::min<size_t>(
        static_cast<size_t>(connector_prefix_count), path_size);
    if (prefix >= path_size) {
        std::fill(flags.begin(), flags.end(), 0);
        return flags;
    }

    const size_t dc_start_idx = prefix - 1;  // include boundary point in collection
    if (dc_start_idx > 0) {
        std::fill(flags.begin(), flags.begin() + static_cast<std::ptrdiff_t>(dc_start_idx), 0);
    }
    return flags;
}

static void dedupePathWithFlags(PathStateList& path, std::vector<int>& flags) {
    if (path.size() != flags.size()) {
        // Fallback: zero all flags if sizes are inconsistent.
        flags.assign(path.size(), 0);
    }
    PathStateList filtered;
    std::vector<int> filtered_flags;
    filtered.reserve(path.size());
    filtered_flags.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        const auto& state = path[i];
        if (!filtered.empty()) {
            double dx = state.point.x - filtered.back().point.x;
            double dy = state.point.y - filtered.back().point.y;
            if (std::fabs(dx) <= kWaypointDuplicateEpsilon &&
                std::fabs(dy) <= kWaypointDuplicateEpsilon) {
                continue;
            }
        }
        filtered.push_back(state);
        filtered_flags.push_back(flags[i]);
    }
    path = std::move(filtered);
    flags = std::move(filtered_flags);
}

static bool savePathWithFlagsToCSV(
    const PathStateList& path,
    const std::vector<int>& flags,
    const QString& filename) {
    std::ofstream file(filename.toStdString());
    if (!file.is_open()) {
        return false;
    }
    file << "x,y,dc\n";
    const size_t n = std::min(path.size(), flags.size());
    for (size_t i = 0; i < n; ++i) {
        file << std::fixed << std::setprecision(6)
             << path[i].point.x << "," << path[i].point.y << ","
             << flags[i] << "\n";
    }
    return true;
}

} // namespace

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
    if (path_connector_prefix_count_ > static_cast<int>(path_.size())) {
        path_connector_prefix_count_ = static_cast<int>(path_.size());
    }
    if (path_connector_prefix_count_ < 0) {
        path_connector_prefix_count_ = 0;
    }
    update();
}

void PlotWidget::setPathConnectorPrefixCount(int count) {
    path_connector_prefix_count_ = std::max(0, count);
    if (path_connector_prefix_count_ > static_cast<int>(path_.size())) {
        path_connector_prefix_count_ = static_cast<int>(path_.size());
    }
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
                                 bool visible) {
    int prev_hover = hovered_scan_segment_;
    scan_segments_ = segments;
    scan_segment_labels_ = labels;
    scan_segment_lengths_ = lengths;
    scan_segment_turns_ = turns;
    show_scan_segments_ = visible;
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
    if (dark_mode_) {
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
    path_connector_prefix_count_ = 0;
    robot_pose_.reset();
    robot_trail_.clear();
    custom_waypoints_.clear();
    custom_waypoint_states_.clear();
    scan_segments_.clear();
    scan_segment_labels_.clear();
    scan_segment_lengths_.clear();
    scan_segment_turns_.clear();
    show_scan_segments_ = false;
    hovered_scan_segment_ = -1;
    active_scan_segment_ = -1;
    reproj_lines_.clear();
    hovered_reproj_index_ = -1;
    selection_points_.clear();
    measure_points_.clear();
    measure_mode_ = false;
    emit measureDistanceUpdated(0.0, false);
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
void PlotWidget::clearPath() {
    path_.clear();
    path_connector_prefix_count_ = 0;
    update();
}

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
    measure_mode_ = false;
    measure_points_.clear();
    emit measureDistanceUpdated(0.0, false);
    selecting_ = true;
    selecting_roi_ = true;
    selection_points_.clear();
    setCursor(Qt::CrossCursor);
    update();
}

void PlotWidget::startObstacleSelection() {
    measure_mode_ = false;
    measure_points_.clear();
    emit measureDistanceUpdated(0.0, false);
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

void PlotWidget::setMeasureMode(bool enabled) {
    measure_mode_ = enabled;
    measure_points_.clear();
    if (measure_mode_) {
        setCursor(Qt::CrossCursor);
    } else if (!selecting_ && !drawing_rectangle_ && !panning_) {
        setCursor(Qt::ArrowCursor);
    }
    emit measureDistanceUpdated(0.0, false);
    update();
}

void PlotWidget::clearMeasurePoints() {
    measure_points_.clear();
    emit measureDistanceUpdated(0.0, false);
    update();
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
    double margin_x = (data_max_x_ - data_min_x_) * 0.05;
    double margin_y = (data_max_y_ - data_min_y_) * 0.05;
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
    
    double scale_x = (width() - 40) / data_w;
    double scale_y = (height() - 40) / data_h;
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
    painter.setRenderHint(QPainter::Antialiasing);
    
    // Background
    painter.fillRect(rect(), dark_mode_ ? QColor(30, 30, 35) : Qt::white);
    
    // Draw grid
    QColor grid_color = dark_mode_ ? QColor(60, 60, 70) : QColor(200, 200, 200);
    painter.setPen(QPen(grid_color, 1, Qt::DashLine));
    double grid_step = std::pow(10, std::floor(std::log10(std::max(data_max_x_ - data_min_x_, data_max_y_ - data_min_y_) / 5)));
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
    
    // Draw points
    if (!points_.empty()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(100, 100, 100, 150));
        for (const auto& p : points_) {
            QPointF sp = worldToScreen(p);
            painter.drawEllipse(sp, 2, 2);  // Larger points for visibility
        }
    }
    
    // Draw polygon
    if (!polygon_.empty()) {
        painter.setPen(QPen(Qt::black, 2));
        painter.setBrush(Qt::NoBrush);
        QPolygonF poly_qp;
        for (const auto& p : polygon_) {
            poly_qp << worldToScreen(p);
        }
        poly_qp << poly_qp.first();  // Close polygon
        painter.drawPolygon(poly_qp);
    }
    
    // Draw ROI
    if (!roi_.empty()) {
        painter.setPen(QPen(Qt::green, 2));
        painter.setBrush(Qt::NoBrush);
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
        painter.setPen(QPen(selected ? QColor(255, 193, 7) : Qt::red, selected ? 3 : 2));
        painter.setBrush(selected ? QColor(255, 193, 7, 55) : QColor(255, 0, 0, 50));
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
        painter.setPen(QPen(Qt::blue, 1, Qt::DashLine));
        for (const auto& sw : swaths_) {
            QPointF p1 = worldToScreen(sw.start);
            QPointF p2 = worldToScreen(sw.end);
            painter.drawLine(p1, p2);
        }
    }
    
    // Draw route
    if (!route_.empty()) {
        painter.setPen(QPen(QColor(255, 140, 0), 1.5));
        for (size_t i = 1; i < route_.size(); ++i) {
            QPointF p1 = worldToScreen(route_[i-1].point);
            QPointF p2 = worldToScreen(route_[i].point);
            painter.drawLine(p1, p2);
        }
    }
    
    // Draw path
    if (!path_.empty()) {
        const int split_idx = std::clamp(path_connector_prefix_count_, 0, static_cast<int>(path_.size()));
        if (split_idx > 1) {
                painter.setPen(QPen(QColor(255, 165, 0), 3.0, Qt::DashLine, Qt::RoundCap));  // connector segment (orange)
            for (int i = 1; i < split_idx; ++i) {
                QPointF p1 = worldToScreen(path_[static_cast<size_t>(i - 1)].point);
                QPointF p2 = worldToScreen(path_[static_cast<size_t>(i)].point);
                painter.drawLine(p1, p2);
            }
        }
        if (path_.size() >= 2 && static_cast<size_t>(split_idx) < path_.size()) {
            painter.setPen(QPen(Qt::red, 1.5, Qt::SolidLine, Qt::RoundCap));  // coverage segment
            const size_t start_i = std::max<size_t>(1, static_cast<size_t>(split_idx));
            for (size_t i = start_i; i < path_.size(); ++i) {
                QPointF p1 = worldToScreen(path_[i - 1].point);
                QPointF p2 = worldToScreen(path_[i].point);
                painter.drawLine(p1, p2);
            }
        }
        
        // Start and end markers
        if (!path_.empty()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::green);
            QPointF start = worldToScreen(path_.front().point);
            painter.drawEllipse(start, 6, 6);
            
            painter.setBrush(Qt::red);
            QPointF end = worldToScreen(path_.back().point);
            painter.drawRect(QRectF(end.x() - 5, end.y() - 5, 10, 10));
        }
    }

    // Draw scan segments
    if (show_scan_segments_ && !scan_segments_.empty()) {
        static const QVector<QColor> palette = {
            QColor("#1f77b4"), QColor("#ff7f0e"), QColor("#2ca02c"),
            QColor("#d62728"), QColor("#9467bd"), QColor("#8c564b"),
            QColor("#e377c2"), QColor("#7f7f7f"), QColor("#bcbd22"), QColor("#17becf")
        };
        for (int i = 0; i < static_cast<int>(scan_segments_.size()); ++i) {
            const auto& seg = scan_segments_[i];
            if (seg.size() < 2) continue;
            QColor base = palette[i % palette.size()];
            bool hovered = (i == hovered_scan_segment_) || (i == active_scan_segment_);
            QColor penColor = hovered ? base.lighter(130) : base;
            penColor.setAlpha(hovered ? 255 : 190);
            QPen pen(penColor, hovered ? 4.0 : 3.0, Qt::SolidLine, Qt::RoundCap);
            painter.setPen(pen);
            for (size_t j = 1; j < seg.size(); ++j) {
                painter.drawLine(worldToScreen(seg[j-1].point),
                                 worldToScreen(seg[j].point));
            }
            // Start/end markers
            painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::white);
            QPointF start = worldToScreen(seg.front().point);
            painter.drawEllipse(start, 4, 4);
            painter.setBrush(penColor);
            painter.drawEllipse(start, 7, 7);

            painter.setBrush(Qt::white);
            QPointF end = worldToScreen(seg.back().point);
            painter.drawRect(QRectF(end.x() - 4, end.y() - 4, 8, 8));
            painter.setBrush(penColor);
            painter.drawRect(QRectF(end.x() - 6, end.y() - 6, 12, 12));

            // Label with icon at midpoint
            QPointF mid = worldToScreen(seg[seg.size() / 2].point);
            painter.setPen(dark_mode_ ? Qt::white : Qt::black);
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
        painter.setPen(QPen(QColor(30, 144, 255, 180), 2, Qt::SolidLine, Qt::RoundCap));
        for (size_t i = 1; i < robot_trail_.size(); ++i) {
            painter.drawLine(worldToScreen(robot_trail_[i-1]),
                             worldToScreen(robot_trail_[i]));
        }
    }

    // Re-draw connector on top so it is not hidden by scan/custom overlays.
    if (!path_.empty()) {
        const int split_idx = std::clamp(path_connector_prefix_count_, 0, static_cast<int>(path_.size()));
        if (split_idx > 1) {
            painter.setPen(QPen(QColor(255, 165, 0), 4.0, Qt::DashLine, Qt::RoundCap));  // orange connector overlay
            for (int i = 1; i < split_idx; ++i) {
                QPointF p1 = worldToScreen(path_[static_cast<size_t>(i - 1)].point);
                QPointF p2 = worldToScreen(path_[static_cast<size_t>(i)].point);
                painter.drawLine(p1, p2);
            }
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
        
        painter.setPen(QPen(QColor(128, 0, 128), 2));
        painter.setBrush(QColor(255, 192, 203, 230));
        painter.drawPolygon(tri);
        
        QPointF center = worldToScreen(pose.point);
        painter.setPen(QPen(Qt::black, 1));
        painter.setFont(QFont("Sans Serif", 8, QFont::Bold));
        painter.drawText(center + QPointF(8, -8), "Robot");
    }
    
    // Draw origin marker (robot position at 0,0)
    {
        QPointF origin = worldToScreen(Point2D(0.0, 0.0));
        
        // Check if origin is within view bounds (with some margin)
        if (origin.x() > -50 && origin.x() < width() + 50 &&
            origin.y() > -50 && origin.y() < height() + 50) {
            
            // Draw crosshairs
            painter.setPen(QPen(QColor(220, 20, 60), 2));  // Crimson color
            painter.drawLine(origin.x() - 18, origin.y(), origin.x() + 18, origin.y());  // Horizontal
            painter.drawLine(origin.x(), origin.y() - 18, origin.x(), origin.y() + 18);  // Vertical
            
            // Draw circle around origin
            painter.setPen(QPen(QColor(220, 20, 60), 2));
            painter.setBrush(QColor(220, 20, 60, 40));  // Semi-transparent fill
            painter.drawEllipse(origin, 12, 12);
            
            // Draw inner dot
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(220, 20, 60));
            painter.drawEllipse(origin, 3, 3);
            
            // Draw label
            painter.setPen(QColor(220, 20, 60));
            painter.setFont(QFont("Sans Serif", 9, QFont::Bold));
            painter.drawText(origin.x() + 16, origin.y() - 8, "Origin");
            painter.setFont(QFont("Sans Serif", 8));
            painter.drawText(origin.x() + 16, origin.y() + 6, "(0, 0)");
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

    // Draw measurement helper
    if (measure_mode_ || !measure_points_.empty()) {
        painter.setPen(QPen(QColor(0, 170, 255), 2, Qt::DashLine));
        painter.setBrush(QColor(0, 170, 255));
        for (const auto& p : measure_points_) {
            painter.drawEllipse(worldToScreen(p), 4, 4);
        }
        if (measure_points_.size() == 1) {
            painter.drawLine(worldToScreen(measure_points_[0]), cursor_pos_);
        } else if (measure_points_.size() >= 2) {
            QPointF p1 = worldToScreen(measure_points_[0]);
            QPointF p2 = worldToScreen(measure_points_[1]);
            painter.drawLine(p1, p2);
            const double dist = std::hypot(
                measure_points_[1].x - measure_points_[0].x,
                measure_points_[1].y - measure_points_[0].y);
            QPointF mid = (p1 + p2) * 0.5;
            painter.setPen(dark_mode_ ? Qt::white : Qt::black);
            painter.setFont(QFont("Sans Serif", 9, QFont::Bold));
            painter.drawText(mid + QPointF(8, -6), QString("%1 m").arg(dist, 0, 'f', 3));
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
    painter.setPen(dark_mode_ ? Qt::white : Qt::black);
    painter.setFont(QFont("Sans Serif", 10, QFont::Bold));
    painter.drawText(10, 20, "2D Projection / Coverage");
    
    // Draw live stats overlay (top-right)
    if (show_live_overlay_ && !live_overlay_lines_.empty()) {
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

static bool pointInPolygonRayCastLocal(const Point2D& p, const Polygon2D& poly) {
    if (poly.size() < 3) return false;
    bool inside = false;
    double x = p.x;
    double y = p.y;
    Point2D p0 = poly.back();
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point2D& p1 = poly[i];
        bool intersects = ((p1.y > y) != (p0.y > y)) &&
            (x < (p0.x - p1.x) * (y - p1.y) / ((p0.y - p1.y) + 1e-12) + p1.x);
        if (intersects) {
            inside = !inside;
        }
        p0 = p1;
    }
    return inside;
}

static bool pointInObstacleShapeLocal(const Point2D& p, const Obstacle2D& obs) {
    if (!pointInPolygonRayCastLocal(p, obs.outer)) {
        return false;
    }
    for (const auto& hole : obs.holes) {
        if (pointInPolygonRayCastLocal(p, hole)) {
            return false;
        }
    }
    return true;
}

static double orient2DLocal(const Point2D& a, const Point2D& b, const Point2D& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static bool onSegmentLocal(const Point2D& a, const Point2D& b, const Point2D& p) {
    const double eps = 1e-12;
    if (std::abs(orient2DLocal(a, b, p)) > eps) return false;
    return (p.x >= std::min(a.x, b.x) - eps && p.x <= std::max(a.x, b.x) + eps &&
            p.y >= std::min(a.y, b.y) - eps && p.y <= std::max(a.y, b.y) + eps);
}

static bool segmentsIntersectLocal(const Point2D& a, const Point2D& b, const Point2D& c, const Point2D& d) {
    const double o1 = orient2DLocal(a, b, c);
    const double o2 = orient2DLocal(a, b, d);
    const double o3 = orient2DLocal(c, d, a);
    const double o4 = orient2DLocal(c, d, b);

    auto sgn = [](double v) -> int {
        const double eps = 1e-12;
        if (v > eps) return 1;
        if (v < -eps) return -1;
        return 0;
    };

    const int s1 = sgn(o1);
    const int s2 = sgn(o2);
    const int s3 = sgn(o3);
    const int s4 = sgn(o4);

    // General case
    if (s1 * s2 < 0 && s3 * s4 < 0) {
        return true;
    }

    // Collinear cases
    if (s1 == 0 && onSegmentLocal(a, b, c)) return true;
    if (s2 == 0 && onSegmentLocal(a, b, d)) return true;
    if (s3 == 0 && onSegmentLocal(c, d, a)) return true;
    if (s4 == 0 && onSegmentLocal(c, d, b)) return true;

    return false;
}

static bool polygonEdgesIntersectLocal(const Polygon2D& a, const Polygon2D& b) {
    if (a.size() < 2 || b.size() < 2) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const Point2D& a0 = a[i];
        const Point2D& a1 = a[(i + 1) % a.size()];
        for (size_t j = 0; j < b.size(); ++j) {
            const Point2D& b0 = b[j];
            const Point2D& b1 = b[(j + 1) % b.size()];
            if (segmentsIntersectLocal(a0, a1, b0, b1)) {
                return true;
            }
        }
    }
    return false;
}

void PlotWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (measure_mode_) {
            Point2D world = screenToWorld(event->pos());
            if (measure_points_.size() >= 2) {
                measure_points_.clear();
            }
            measure_points_.push_back(world);
            if (measure_points_.size() >= 2) {
                const double dist = std::hypot(
                    measure_points_[1].x - measure_points_[0].x,
                    measure_points_[1].y - measure_points_[0].y);
                emit measureDistanceUpdated(dist, true);
            } else {
                emit measureDistanceUpdated(0.0, false);
            }
            update();
            return;
        }
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
        if (measure_mode_) {
            clearMeasurePoints();
        } else if (drawing_rectangle_) {
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
    } else if (selecting_ || measure_mode_) {
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

// =============================================================================
// CoverageGUI Implementation
// =============================================================================

CoverageGUI::CoverageGUI(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("BDR Coverage Planner");
    resize(1500, 900);
    
    // Initialize robot map fetch base (robot-specific suffix applied after registry load)
    local_map_base_ = QDir::homePath() + "/Roofus_maps";
    
    // Initialize CycloneDDS config path (loopback-only for local node communication)
    // Cross-network communication is handled by Zenoh bridge DDS (managed by laptop_teleop.launch.py)
    dds_config_path_ = QDir::homePath() + "/cyclone_loopback.xml";

    // Load persisted settings
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    robot_host_ = settings.value("robot_ip", robot_host_).toString();
    robot_odom_topic_ = settings.value("robot_odom_topic", robot_odom_topic_).toString();
    robot_marker_size_m_ = settings.value("robot_marker_size_m", robot_marker_size_m_).toDouble();

    // Load robot registry and select active robot (robot_id -> hidden static IP)
    loadRobotRegistry();
    const QString persisted_robot_id = settings.value("robot_id", "").toString().trimmed();
    if (robot_registry_.isLoaded() && !robot_registry_.isEmpty()) {
        if (!persisted_robot_id.isEmpty() && setActiveRobotId(persisted_robot_id, false)) {
            // Active robot restored from settings
        } else {
            // Default to first registry entry
            setActiveRobotId(robot_registry_.robots().front().robot_id, true);
        }
    } else if (!persisted_robot_id.isEmpty()) {
        // Registry not available, but still keep identity for local paths/metadata.
        active_robot_id_ = persisted_robot_id;
        active_robot_slug_ = RobotRegistry::slugifyRobotId(active_robot_id_);
    }

    // Apply robot-dependent paths/config (UI is not built yet, so updateUi=false)
    applyActiveRobotProfile(false);
    
    // Set CYCLONEDDS_URI environment variable (loopback-only config)
    if (QFile::exists(dds_config_path_)) {
        qputenv("CYCLONEDDS_URI", dds_config_path_.toUtf8());
        std::cout << "[Coverage Planner] Using CycloneDDS config (loopback): " << dds_config_path_.toStdString() << std::endl;
    } else {
        std::cerr << "[Coverage Planner] Warning: CycloneDDS config not found: " << dds_config_path_.toStdString() << std::endl;
        std::cerr << "[Coverage Planner] Local node communication may not work correctly." << std::endl;
    }
    
    // Initialize Zenoh bridge status
    zenoh_bridge_detected_ = false;
    
    fit_view_pending_ = true;
    last_live_ui_update_ = std::chrono::steady_clock::now();
    
    // Initialize async point cloud loader
    pcd_watcher_ = new QFutureWatcher<PointCloudPtr>(this);
    connect(pcd_watcher_, &QFutureWatcher<PointCloudPtr>::finished, 
            this, &CoverageGUI::onPointCloudLoaded);

    // Initialize async height crop worker
    height_crop_watcher_ = new QFutureWatcher<HeightCropResult>(this);
    connect(height_crop_watcher_, &QFutureWatcher<HeightCropResult>::finished,
            this, &CoverageGUI::onHeightCropFinished);

    // Initialize async obstacle detector
    obstacle_detect_watcher_ = new QFutureWatcher<ObstacleDetectionResult>(this);
    connect(obstacle_detect_watcher_, &QFutureWatcher<ObstacleDetectionResult>::finished,
            this, &CoverageGUI::onAutoDetectObstaclesFinished);

    // Initialize async transit path planner (home / GO TO)
    transit_plan_watcher_ = new QFutureWatcher<PathStateList>(this);
    connect(transit_plan_watcher_, &QFutureWatcher<PathStateList>::finished,
            this, &CoverageGUI::onTransitPathPlanningFinished);
    
    // Load dark mode preference (reuse settings from above)
    dark_mode_ = settings.value("dark_mode", false).toBool();
    
    setupUI();
    applyActiveRobotProfile(true);
    updateLoginUi();
    setupConnections();
    refreshCustomPathUI();
    applyTheme();  // Apply saved theme

    // Initialize ROS2 reconnection timer (will only run when disconnected)
    ros_reconnect_timer_ = new QTimer(this);
    ros_reconnect_timer_->setInterval(5000);  // Try every 5 seconds
    connect(ros_reconnect_timer_, &QTimer::timeout, this, &CoverageGUI::tryReconnectROS2);
    
    // Initialize Zenoh bridge status check timer (periodic monitoring)
    // The Zenoh bridge is managed by laptop_teleop.launch.py; we only monitor it here
    zenoh_check_timer_ = new QTimer(this);
    zenoh_check_timer_->setInterval(5000);  // Check every 5 seconds
    connect(zenoh_check_timer_, &QTimer::timeout, this, &CoverageGUI::checkZenohBridgeStatus);

    // Initialize ROS2 (with error handling so GUI works even if ROS2 fails)
    waypoints_published_ = false;
    ros_initialized_ = false;
    try {
        ros_node_ = rclcpp::Node::make_shared("bdr_coverage_gui");
        waypoint_pub_ = ros_node_->create_publisher<std_msgs::msg::Float64MultiArray>(
            "/f2c_waypoints", 10);
        setupRobotTrackingSubscription();

        // Start ROS2 spinning in background thread
        ros_thread_ = std::thread([this]() {
            rclcpp::spin(ros_node_);
        });
        ros_initialized_ = true;
        setStatus("Ready (ROS2 local, checking Zenoh bridge...)");
        
        // Create teleop dock widget now that ros_node_ is available
        teleop_dock_ = new TeleopDockWidget(ros_node_, this);
        teleop_dock_->setFloating(true);
        teleop_dock_->hide();  // Hidden by default
        addDockWidget(Qt::RightDockWidgetArea, teleop_dock_);
        connect(teleop_dock_->teleopWidget(), &TeleopWidget::statusMessage,
                this, &CoverageGUI::onTeleopStatusMessage);
        
        // Create scan session tracker with GPS subscription
        scan_session_tracker_ = new ScanSessionTracker(ros_node_, this);
    } catch (const std::exception& e) {
        std::cerr << "[Coverage Planner] Warning: ROS2 initialization failed: " << e.what() << std::endl;
        std::cerr << "[Coverage Planner] Starting background reconnection timer..." << std::endl;
        setStatus("Ready (ROS2 unavailable - reconnecting...)");
        
        // Start the reconnection timer
        ros_reconnect_timer_->start();
    }
    
    // Start Zenoh bridge monitoring (runs regardless of ROS2 status)
    checkZenohBridgeStatus();  // Initial check
    zenoh_check_timer_->start();
}

CoverageGUI::~CoverageGUI() {
    // Stop timers
    if (ros_reconnect_timer_) {
        ros_reconnect_timer_->stop();
    }
    if (zenoh_check_timer_) {
        zenoh_check_timer_->stop();
    }

    progress_cancel_requested_.store(true);

    // Avoid use-after-free if background futures are still running
    if (pcd_watcher_ && pcd_watcher_->isRunning()) {
        pcd_watcher_->waitForFinished();
    }
    if (height_crop_watcher_ && height_crop_watcher_->isRunning()) {
        height_crop_watcher_->waitForFinished();
    }
    if (obstacle_detect_watcher_ && obstacle_detect_watcher_->isRunning()) {
        obstacle_detect_watcher_->waitForFinished();
    }
    if (transit_plan_watcher_ && transit_plan_watcher_->isRunning()) {
        transit_plan_watcher_->waitForFinished();
    }

    setProgressCallback(nullptr);
    setCancelCheckCallback(nullptr);
    setObstacleCancelCallback(nullptr);
    
    // Clean up ROS2 resources
    fastlio_sub_.reset();
    waypoint_pub_.reset();
    
    if (ros_node_) {
        rclcpp::shutdown();
    }
    
    // Wait for ROS thread to finish
    if (ros_thread_.joinable()) {
        ros_thread_.join();
    }
    
    ros_node_.reset();
}

void CoverageGUI::setupUI() {
    QWidget* central = new QWidget();
    QVBoxLayout* root_layout = new QVBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);
    
    // Main splitter with left / center / right
    main_splitter_ = new QSplitter(Qt::Horizontal);
    main_splitter_->setChildrenCollapsible(false);
    
    // LEFT: full panel (scroll) and mini palette
    QScrollArea* controls_scroll = new QScrollArea();
    controls_scroll->setWidgetResizable(true);
    controls_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    controls_scroll->setMinimumWidth(390);
    controls_scroll->setMaximumWidth(16777215);  // Allow splitter-driven expansion
    
    QWidget* controls_container = new QWidget();
    QVBoxLayout* controls_layout = new QVBoxLayout(controls_container);
    controls_layout->setContentsMargins(12, 12, 12, 12);
    controls_layout->setSpacing(12);
    
    // Standard panel layout with QGroupBox sections
    controls_layout->addWidget(buildFileControls());
    controls_layout->addWidget(buildRobotLoginControls());
    controls_layout->addWidget(buildRobotTrackingControls());
    controls_layout->addWidget(buildHeightControls());
    controls_layout->addWidget(buildDownsampleControls());
    controls_layout->addWidget(buildHullControls());
    controls_layout->addWidget(buildSimplifyControls());
    controls_layout->addWidget(buildPathPlanningControls());
    controls_layout->addWidget(buildCoverageStatsControls());
    controls_layout->addWidget(buildExportControls());
    controls_layout->addStretch(1);
    
    controls_scroll->setWidget(controls_container);
    left_full_ = controls_scroll;
    left_mini_ = buildLeftMiniPalette();
    
    left_stack_ = new QStackedWidget();
    left_stack_->addWidget(left_full_);
    left_stack_->addWidget(left_mini_);
    
    // CENTER: Plot panel
    QWidget* plot_container = new QWidget();
    QVBoxLayout* plot_layout = new QVBoxLayout(plot_container);
    plot_layout->setContentsMargins(0, 0, 0, 0);
    
    // Plot widget
    plot_ = new PlotWidget();
    plot_->setMinimumSize(400, 400);
    plot_->setRobotMarkerSize(robot_marker_size_m_);
    plot_layout->addWidget(plot_, 1);
    
    // Toolbar for plot
    QHBoxLayout* toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(8, 4, 8, 4);
    QPushButton* btn_reset_view = new QPushButton("Reset View");
    QPushButton* btn_zoom_in = new QPushButton("+");
    QPushButton* btn_zoom_out = new QPushButton("-");
    btn_zoom_in->setFixedWidth(30);
    btn_zoom_out->setFixedWidth(30);
    btn_measure_ = new QPushButton("Measure");
    btn_measure_->setCheckable(true);
    btn_measure_->setToolTip(
        "Click two points on the map to measure Euclidean distance.\n"
        "Right-click to clear points."
    );
    lbl_measure_ = new QLabel("Distance: -");
    toolbar->addWidget(btn_reset_view);
    toolbar->addWidget(btn_zoom_in);
    toolbar->addWidget(btn_zoom_out);
    toolbar->addWidget(btn_measure_);
    toolbar->addWidget(lbl_measure_);
    toolbar->addStretch();
    
    // Dark mode toggle
    btn_dark_mode_ = new QPushButton("🌙 Dark Mode");
    btn_dark_mode_->setCheckable(true);
    btn_dark_mode_->setChecked(dark_mode_);
    btn_dark_mode_->setFixedWidth(110);
    toolbar->addWidget(btn_dark_mode_);
    
    // View FOV button (video panel toggle)
    btn_view_fov_ = new QPushButton("📹 View FOV");
    btn_view_fov_->setCheckable(true);
    btn_view_fov_->setToolTip("Toggle camera feed panel");
    btn_view_fov_->setFixedWidth(100);
    btn_view_fov_->setChecked(true);
    toolbar->addWidget(btn_view_fov_);
    
    plot_layout->addLayout(toolbar);
    
    connect(btn_reset_view, &QPushButton::clicked, plot_, &PlotWidget::resetView);
    connect(btn_zoom_in, &QPushButton::clicked, plot_, &PlotWidget::zoomIn);
    connect(btn_zoom_out, &QPushButton::clicked, plot_, &PlotWidget::zoomOut);
    connect(btn_measure_, &QPushButton::clicked, this, &CoverageGUI::toggleMeasureMode);
    connect(btn_dark_mode_, &QPushButton::toggled, this, &CoverageGUI::toggleDarkMode);
    
    // RIGHT: full panel (layers + video) and mini palette
    right_full_ = buildRightFullPane();
    right_mini_ = buildRightMiniPalette();
    right_stack_ = new QStackedWidget();
    right_stack_->addWidget(right_full_);
    right_stack_->addWidget(right_mini_);
    
    main_splitter_->addWidget(left_stack_);
    main_splitter_->addWidget(plot_container);
    main_splitter_->addWidget(right_stack_);
    main_splitter_->setStretchFactor(0, 0);
    main_splitter_->setStretchFactor(1, 1);
    main_splitter_->setStretchFactor(2, 0);
    main_splitter_->setSizes({left_saved_width_, 800, right_saved_width_});
    
    // Collapse buttons bar (near inner edges)
    QHBoxLayout* collapse_bar = new QHBoxLayout();
    collapse_bar->setContentsMargins(6, 4, 6, 4);
    btn_collapse_left_ = new QToolButton();
    btn_collapse_left_->setAutoRaise(true);
    btn_collapse_left_->setToolTip("Collapse/expand left panel");
    btn_collapse_right_ = new QToolButton();
    btn_collapse_right_->setAutoRaise(true);
    btn_collapse_right_->setToolTip("Collapse/expand right panel");
    collapse_bar->addWidget(btn_collapse_left_, 0, Qt::AlignLeft);
    collapse_bar->addStretch();
    collapse_bar->addWidget(btn_collapse_right_, 0, Qt::AlignRight);
    
    root_layout->addLayout(collapse_bar);
    root_layout->addWidget(main_splitter_, 1);
    root_layout->addWidget(buildQuickActionsBar());
    
    setCentralWidget(central);
    
    // Status bar
    status_bar_ = new QStatusBar();
    setStatusBar(status_bar_);
    
    progress_bar_ = new QProgressBar();
    progress_bar_->setVisible(false);
    progress_bar_->setMaximumWidth(200);
    status_bar_->addPermanentWidget(progress_bar_);

    btn_progress_cancel_ = new QPushButton("Cancel");
    btn_progress_cancel_->setVisible(false);
    btn_progress_cancel_->setEnabled(false);
    status_bar_->addPermanentWidget(btn_progress_cancel_);
    
    // Note: Teleop dock is created later after ROS2 node is initialized
    // See end of constructor where ros_node_ is created
    
    toggleVideoPanel();
    updateCollapseButtons();
}

void CoverageGUI::setupConnections() {
    connect(plot_, &PlotWidget::roiSelected, this, &CoverageGUI::onROISelected);
    connect(plot_, &PlotWidget::obstacleSelected, this, &CoverageGUI::onObstacleSelected);
    connect(plot_, &PlotWidget::selectionCancelled, this, &CoverageGUI::onSelectionCancelled);
    connect(plot_, &PlotWidget::obstacleDeleteRequested, this, &CoverageGUI::onObstacleDeleteRequested);
    connect(plot_, &PlotWidget::obstacleSelectionChanged, this, &CoverageGUI::onObstacleSelectionChanged);
    connect(plot_, &PlotWidget::customWaypointRequested, this, &CoverageGUI::onPlotCustomWaypoint);
    connect(plot_, &PlotWidget::rectangleCompleted, this, &CoverageGUI::onRectangleCompleted);
    connect(plot_, &PlotWidget::measureDistanceUpdated, this, &CoverageGUI::onMeasureDistanceUpdated);
    if (btn_progress_cancel_) {
        connect(btn_progress_cancel_, &QPushButton::clicked, this, &CoverageGUI::onCancelProgressRequested);
    }
    
    // Path mode switching
    if (radio_mode_f2c_) {
        connect(radio_mode_f2c_, &QRadioButton::toggled, this, &CoverageGUI::onPathModeChanged);
    }
    
    if (btn_custom_draw_) {
        connect(btn_custom_draw_, &QPushButton::toggled, this, [this](bool checked) {
            if (checked && btn_measure_ && btn_measure_->isChecked()) {
                btn_measure_->setChecked(false);
                if (plot_) {
                    plot_->setMeasureMode(false);
                }
                if (lbl_measure_) {
                    lbl_measure_->setText("Distance: -");
                }
            }
            custom_draw_enabled_ = checked;
            plot_->setCustomDrawMode(checked && isCustomModeActive());
            if (checked) {
                setStatus("Custom draw enabled - click on the map to add waypoints");
            }
        });
    }
    if (btn_custom_undo_) {
        connect(btn_custom_undo_, &QPushButton::clicked, this, &CoverageGUI::undoCustomWaypoint);
    }
    if (btn_custom_clear_) {
        connect(btn_custom_clear_, &QPushButton::clicked, this, &CoverageGUI::clearCustomWaypoints);
    }

    // Live overlay toggles
    auto connectOverlay = [this](QCheckBox* box) {
        if (box) {
            connect(box, &QCheckBox::toggled, this, &CoverageGUI::rebuildLiveOverlay);
        }
    };
    connectOverlay(chk_live_overlay_);
    connectOverlay(chk_live_show_progress_);
    connectOverlay(chk_live_show_travel_);
    connectOverlay(chk_live_show_remaining_);
    connectOverlay(chk_live_show_eta_);
    connectOverlay(chk_live_show_elapsed_);
    connectOverlay(chk_live_show_speed_);
    
    // Scan planner
    if (btn_make_segments_) {
        connect(btn_make_segments_, &QPushButton::clicked, this, &CoverageGUI::generateScanSegments);
    }
    if (btn_publish_segments_) {
        connect(btn_publish_segments_, &QPushButton::clicked, this, &CoverageGUI::publishSelectedScanSegments);
    }
    if (btn_start_segments_) {
        connect(btn_start_segments_, &QPushButton::clicked, this, &CoverageGUI::startSelectedScanSegments);
    }
    if (list_scan_segments_) {
        connect(list_scan_segments_, &QListWidget::itemSelectionChanged, this, [this]() {
            auto selItems = list_scan_segments_->selectedItems();
            active_scan_segment_idx_ = selItems.isEmpty() ? -1 : list_scan_segments_->row(selItems.first());
            plot_->setActiveScanSegment(active_scan_segment_idx_);
        });
    }
    
    // Collapse buttons
    if (btn_collapse_left_) {
        connect(btn_collapse_left_, &QToolButton::clicked, this, &CoverageGUI::toggleLeftPane);
    }
    if (btn_collapse_right_) {
        connect(btn_collapse_right_, &QToolButton::clicked, this, &CoverageGUI::toggleRightPane);
    }
    
    // Set progress callback
    setProgressCallback([this](int percent, const std::string& msg) {
        const QString text = QString::fromStdString(msg);
        if (QThread::currentThread() == thread()) {
            updateProgress(percent, text);
        } else {
            QMetaObject::invokeMethod(this, [this, percent, text]() {
                updateProgress(percent, text);
            }, Qt::QueuedConnection);
        }
    });
    setCancelCheckCallback([this]() {
        return isProgressCancelRequested();
    });
    setObstacleCancelCallback([this]() {
        return isProgressCancelRequested();
    });
}

void CoverageGUI::toggleLeftPane() {
    if (!main_splitter_ || !left_stack_) return;
    QList<int> sizes = main_splitter_->sizes();
    if (!left_collapsed_) {
        // Collapsing: save current width (always use fixed 390 for full panel)
        left_saved_width_ = 390;
        left_collapsed_ = true;
        left_stack_->setCurrentWidget(left_mini_);
        sizes[0] = 60;
        if (sizes.size() >= 2) {
            sizes[1] = std::max(200, sizes[1] + left_saved_width_ - 60);
        }
    } else {
        // Expanding: restore to fixed width
        left_collapsed_ = false;
        left_stack_->setCurrentWidget(left_full_);
        int restore = 390;  // Fixed width for left panel
        if (sizes.size() >= 3) {
            int center = sizes[1];
            int delta = restore - sizes[0];
            sizes[0] = restore;
            sizes[1] = std::max(200, center - delta);
        }
    }
    main_splitter_->setSizes(sizes);
    updateCollapseButtons();
}

void CoverageGUI::toggleRightPane() {
    if (!main_splitter_ || !right_stack_) return;
    QList<int> sizes = main_splitter_->sizes();
    if (!right_collapsed_) {
        right_saved_width_ = sizes.value(2, right_saved_width_);
        right_saved_width_ = std::max(right_saved_width_, 220);
        right_collapsed_ = true;
        right_stack_->setCurrentWidget(right_mini_);
        if (sizes.size() >= 3) {
            sizes[2] = 60;
            sizes[1] = std::max(200, sizes[1] + right_saved_width_ - 60);
        }
    } else {
        right_collapsed_ = false;
        right_stack_->setCurrentWidget(right_full_);
        int restore = std::max(right_saved_width_, 240);
        if (sizes.size() >= 3) {
            int center = sizes[1];
            int delta = restore - sizes[2];
            sizes[2] = restore;
            sizes[1] = std::max(200, center - delta);
        }
    }
    main_splitter_->setSizes(sizes);
    updateCollapseButtons();
}

void CoverageGUI::updateCollapseButtons() {
    if (btn_collapse_left_) {
        btn_collapse_left_->setIcon(style()->standardIcon(
            left_collapsed_ ? QStyle::SP_ArrowRight : QStyle::SP_ArrowLeft));
    }
    if (btn_collapse_right_) {
        btn_collapse_right_->setIcon(style()->standardIcon(
            right_collapsed_ ? QStyle::SP_ArrowLeft : QStyle::SP_ArrowRight));
    }
}

QWidget* CoverageGUI::buildVideoPanelWidget() {
    QWidget* dock_content = new QWidget();
    QVBoxLayout* dock_layout = new QVBoxLayout(dock_content);
    dock_layout->setContentsMargins(6, 6, 6, 6);
    dock_layout->setSpacing(6);
    
    // Camera selector
    QHBoxLayout* cam_selector = new QHBoxLayout();
    cam_selector->addWidget(new QLabel("Camera:"));
    radio_cam_left_ = new QRadioButton("Left");
    radio_cam_right_ = new QRadioButton("Right");
    radio_cam_left_->setChecked(true);
    
    QButtonGroup* cam_group = new QButtonGroup(this);
    cam_group->addButton(radio_cam_left_);
    cam_group->addButton(radio_cam_right_);
    
    cam_selector->addWidget(radio_cam_left_);
    cam_selector->addWidget(radio_cam_right_);
    cam_selector->addStretch();
    dock_layout->addLayout(cam_selector);
    
    // Port configuration and stream setup
    QHBoxLayout* port_layout = new QHBoxLayout();
    port_layout->addWidget(new QLabel("Port:"));
    spin_video_port_ = new QSpinBox();
    spin_video_port_->setRange(1024, 65535);
    spin_video_port_->setValue(5600);
    spin_video_port_->setToolTip("UDP port for video stream");
    port_layout->addWidget(spin_video_port_);
    
    QPushButton* btn_configure = new QPushButton("📡 Configure");
    btn_configure->setToolTip("Send your IP to the robot to configure stream target");
    port_layout->addWidget(btn_configure);
    port_layout->addStretch();
    dock_layout->addLayout(port_layout);
    
    connect(btn_configure, &QPushButton::clicked, this, &CoverageGUI::publishStreamTarget);
    
    // Video widget
    video_widget_ = new VideoStreamWidget();
    video_widget_->setMinimumSize(320, 240);
    video_widget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    dock_layout->addWidget(video_widget_, 1);
    
    // Status label
    lbl_video_status_ = new QLabel("Stream: Stopped");
    lbl_video_status_->setStyleSheet("color: #888; font-size: 10px;");
    dock_layout->addWidget(lbl_video_status_);
    
    // Control buttons
    QHBoxLayout* controls = new QHBoxLayout();
    btn_video_play_ = new QPushButton("▶ Play");
    btn_video_stop_ = new QPushButton("⏹ Stop");
    btn_video_play_->setObjectName("btn_video_play");
    btn_video_stop_->setObjectName("btn_video_stop");
    btn_video_stop_->setEnabled(false);
    controls->addWidget(btn_video_play_);
    controls->addWidget(btn_video_stop_);
    controls->addStretch();
    dock_layout->addLayout(controls);
    
    dock_content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    dock_content->setMinimumWidth(280);

    // Connections for video panel (btn_view_fov_ created in setupUI)
    connect(btn_view_fov_, &QPushButton::toggled, this, &CoverageGUI::toggleVideoPanel);

    connect(btn_video_play_, &QPushButton::clicked, this, &CoverageGUI::playVideoStream);
    connect(btn_video_stop_, &QPushButton::clicked, this, &CoverageGUI::stopVideoStream);
    
    connect(radio_cam_left_, &QRadioButton::toggled, this, &CoverageGUI::onCameraToggled);
    connect(radio_cam_right_, &QRadioButton::toggled, this, &CoverageGUI::onCameraToggled);
    
    connect(video_widget_, &VideoStreamWidget::streamStarted, this, [this]() {
        lbl_video_status_->setText("Stream: Playing");
        lbl_video_status_->setStyleSheet("color: green; font-size: 10px;");
        btn_video_play_->setEnabled(false);
        btn_video_stop_->setEnabled(true);
    });
    
    connect(video_widget_, &VideoStreamWidget::streamStopped, this, [this]() {
        lbl_video_status_->setText("Stream: Stopped");
        lbl_video_status_->setStyleSheet("color: #888; font-size: 10px;");
        btn_video_play_->setEnabled(true);
        btn_video_stop_->setEnabled(false);
    });
    
    connect(video_widget_, &VideoStreamWidget::streamError, this, [this](const QString& err) {
        lbl_video_status_->setText("Error: " + err);
        lbl_video_status_->setStyleSheet("color: red; font-size: 10px;");
        btn_video_play_->setEnabled(true);
        btn_video_stop_->setEnabled(false);
    });
    
    // Setup ROS2 camera selection publisher if ROS is available
    if (ros_initialized_ && ros_node_) {
        camera_select_pub_ = ros_node_->create_publisher<std_msgs::msg::String>(
            "/stream_camera_select", 10);
        camera_status_sub_ = ros_node_->create_subscription<std_msgs::msg::String>(
            "/stream_camera_status", 10,
            std::bind(&CoverageGUI::onCameraStatusReceived, this, std::placeholders::_1));
    }

    return dock_content;
}

void CoverageGUI::toggleVideoPanel() {
    bool show = btn_view_fov_ && btn_view_fov_->isChecked();
    if (video_panel_widget_) {
        video_panel_widget_->setVisible(show);
    }
    
    // When showing the panel, auto-configure stream target
    if (show && !stream_target_confirmed_) {
        publishStreamTarget();
    }
}

void CoverageGUI::playVideoStream() {
    if (video_widget_) {
        int port = spin_video_port_->value();
        video_widget_->startStream(port);
        lbl_video_status_->setText("Stream: Connecting...");
        lbl_video_status_->setStyleSheet("color: orange; font-size: 10px;");
    }
}

void CoverageGUI::stopVideoStream() {
    if (video_widget_) {
        video_widget_->stopStream();
    }
}

void CoverageGUI::onCameraToggled(bool checked) {
    // Ignore "unchecked" transitions to avoid publishing stale mode
    // when exclusive radio buttons switch.
    if (!checked) return;
    QString camera = "left";
    if (radio_cam_right_ && radio_cam_right_->isChecked()) {
        camera = "right";
    }
    current_streaming_camera_ = camera;
    
    // Lazily create publisher if not yet available
    if (ros_initialized_ && ros_node_ && !camera_select_pub_) {
        camera_select_pub_ = ros_node_->create_publisher<std_msgs::msg::String>(
            "/stream_camera_select", 10);
        camera_status_sub_ = ros_node_->create_subscription<std_msgs::msg::String>(
            "/stream_camera_status", 10,
            std::bind(&CoverageGUI::onCameraStatusReceived, this, std::placeholders::_1));
    }
    
    // Publish camera selection to robot
    if (ros_initialized_ && camera_select_pub_) {
        auto msg = std_msgs::msg::String();
        msg.data = camera.toStdString();
        camera_select_pub_->publish(msg);
        
        setStatus(QString("Switching to %1 camera...").arg(camera));

        if (lbl_video_status_) {
            QString target_display = current_stream_target_;
            if (!target_display.contains(':') && spin_video_port_) {
                target_display += QString(":%1").arg(spin_video_port_->value());
            }
            if (!target_display.isEmpty()) {
                lbl_video_status_->setText(QString("Target: %1 (%2)").arg(target_display, camera));
            } else {
                lbl_video_status_->setText(QString("Switching to %1 camera...").arg(camera));
            }
            lbl_video_status_->setStyleSheet("color: orange; font-size: 10px;");
        }
        
        // If currently playing, restart stream after a delay
        if (video_widget_ && video_widget_->isPlaying()) {
            video_widget_->stopStream();
            
            // Restart stream after pipeline rebuild (~2 seconds)
            QTimer::singleShot(2500, this, [this]() {
                if (video_widget_ && btn_view_fov_->isChecked()) {
                    playVideoStream();
                }
            });
        }
    } else {
        setStatus("ROS2 not available - camera switch requires robot connection");
    }
}

void CoverageGUI::onCameraStatusReceived(const std_msgs::msg::String::SharedPtr msg) {
    QString camera = QString::fromStdString(msg->data);
    current_streaming_camera_ = camera;
    
    QMetaObject::invokeMethod(this, [this, camera]() {
        // Update radio button to match actual streaming camera
        const bool is_right = (camera == "right");
        const QString effective_camera = is_right ? "right" : "left";
        QSignalBlocker blocker_left(radio_cam_left_);
        QSignalBlocker blocker_right(radio_cam_right_);
        if (radio_cam_left_) radio_cam_left_->setChecked(!is_right);
        if (radio_cam_right_) radio_cam_right_->setChecked(is_right);

        if (lbl_video_status_ && !current_stream_target_.isEmpty()) {
            QString target_display = current_stream_target_;
            if (!target_display.contains(':') && spin_video_port_) {
                target_display += QString(":%1").arg(spin_video_port_->value());
            }
            lbl_video_status_->setText(QString("Target: %1 (%2)")
                                           .arg(target_display, effective_camera));
            lbl_video_status_->setStyleSheet("color: green; font-size: 10px;");
        }
        
        setStatus(QString("Streaming: %1 camera").arg(effective_camera));
    }, Qt::QueuedConnection);
}

QString CoverageGUI::detectLocalIP() const {
    // Get all network interfaces and find the best IP for streaming
    // Prefer: 192.168.168.x (RF) > 10.x.x.x (WiFi) > others
    
    QString rf_ip, wifi_ip, other_ip;
    
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        // Skip loopback and down interfaces
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        if (!(iface.flags() & QNetworkInterface::IsRunning)) continue;
        
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) continue;
            
            QString ip = entry.ip().toString();
            
            // Categorize by network
            if (ip.startsWith("192.168.168.")) {
                rf_ip = ip;  // RF network (Microhard)
            } else if (ip.startsWith("10.")) {
                wifi_ip = ip;  // WiFi network
            } else if (!ip.startsWith("127.")) {
                other_ip = ip;  // Other valid IP
            }
        }
    }
    
    // Return in priority order
    if (!rf_ip.isEmpty()) {
        std::cout << "[Stream] Detected RF IP: " << rf_ip.toStdString() << std::endl;
        return rf_ip;
    }
    if (!wifi_ip.isEmpty()) {
        std::cout << "[Stream] Detected WiFi IP: " << wifi_ip.toStdString() << std::endl;
        return wifi_ip;
    }
    if (!other_ip.isEmpty()) {
        std::cout << "[Stream] Detected other IP: " << other_ip.toStdString() << std::endl;
        return other_ip;
    }
    
    std::cerr << "[Stream] Warning: No suitable IP found for streaming" << std::endl;
    return QString();
}

void CoverageGUI::publishStreamTarget() {
    QString local_ip = detectLocalIP();
    
    if (local_ip.isEmpty()) {
        setStatus("Cannot detect local IP for streaming");
        return;
    }
    
    // Lazily create publisher if needed
    if (ros_initialized_ && ros_node_ && !stream_target_pub_) {
        stream_target_pub_ = ros_node_->create_publisher<std_msgs::msg::String>(
            "/stream_target_ip", 10);
        stream_status_sub_ = ros_node_->create_subscription<std_msgs::msg::String>(
            "/stream_status", 10,
            std::bind(&CoverageGUI::onStreamStatusReceived, this, std::placeholders::_1));
    }
    
    if (stream_target_pub_) {
        auto msg = std_msgs::msg::String();
        msg.data = local_ip.toStdString();
        stream_target_pub_->publish(msg);
        
        current_stream_target_ = local_ip;
        stream_target_confirmed_ = false;
        setStatus(QString("Requesting stream to: %1").arg(local_ip));
        
        std::cout << "[Stream] Published stream target: " << local_ip.toStdString() << std::endl;
    } else {
        setStatus("ROS2 not available - cannot configure stream target");
    }
}

void CoverageGUI::onStreamStatusReceived(const std_msgs::msg::String::SharedPtr msg) {
    QString status = QString::fromStdString(msg->data);
    // Format: "ip:port:camera"
    QStringList parts = status.split(":");
    
    QMetaObject::invokeMethod(this, [this, status, parts]() {
        if (parts.size() >= 3) {
            QString ip = parts[0];
            QString port = parts[1];
            QString camera = parts[2];
            
            current_stream_target_ = ip + ":" + port;
            stream_target_confirmed_ = true;
            
            if (lbl_video_status_) {
                lbl_video_status_->setText(QString("Target: %1:%2 (%3)").arg(ip, port, camera));
                lbl_video_status_->setStyleSheet("color: green; font-size: 10px;");
            }
            
            setStatus(QString("Stream configured: %1:%2 (%3 camera)").arg(ip, port, camera));
        }
    }, Qt::QueuedConnection);
}

// =============================================================================
// Data Transfer Implementation
// =============================================================================

void CoverageGUI::openDataTransferDialog() {
    qDebug() << "[CoverageGUI] openDataTransferDialog() called";
    
    // Create the tabbed window lazily
    if (!data_transfer_window_) {
        data_transfer_window_ = new QDialog(this);
        data_transfer_window_->setWindowTitle("Data Transfer");
        data_transfer_window_->setMinimumSize(700, 750);
        data_transfer_window_->setWindowFlags(
            data_transfer_window_->windowFlags() | Qt::WindowMinimizeButtonHint);
        
        QVBoxLayout* layout = new QVBoxLayout(data_transfer_window_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        
        data_transfer_tabs_ = new QTabWidget();
        data_transfer_tabs_->setDocumentMode(true);
        data_transfer_tabs_->setStyleSheet(
            "QTabBar::tab { min-height: 36px; min-width: 160px; font-size: 13px; font-weight: bold; }"
            "QTabBar::tab:selected { color: #1976D2; }");
        
        // Tab 1: Robot → Laptop (existing download dialog)
        data_transfer_dialog_ = new DataTransferDialog();
        data_transfer_dialog_->setRobotHost(robot_host_);
        data_transfer_dialog_->setRobotUser(robot_user_);
        data_transfer_dialog_->setDataPath(robot_data_path_);
        data_transfer_dialog_->setRobotDisplayName(active_robot_id_);
        data_transfer_dialog_->setRobotIdSlug(activeRobotSlug());
        
        connect(data_transfer_dialog_, &DataTransferDialog::transferActive,
                this, &CoverageGUI::onTransferActive);
        connect(data_transfer_dialog_, &DataTransferDialog::transferProgress,
                this, &CoverageGUI::onTransferProgress);
        
        data_transfer_tabs_->addTab(data_transfer_dialog_, "📥 Robot → Laptop");
        
        // Tab 2: Laptop → Cloud (cloud upload dialog)
        cloud_upload_dialog_ = new CloudUploadDialog(scan_session_tracker_);
        
        const QString robotLocalDataRoot = QDir::homePath() + "/robot_data/" + activeRobotSlug();
        cloud_upload_dialog_->setLocalDataPath(robotLocalDataRoot);
        
        connect(cloud_upload_dialog_, &CloudUploadDialog::uploadActive,
                this, &CoverageGUI::onCloudUploadActive);
        
        data_transfer_tabs_->addTab(cloud_upload_dialog_, "☁ Laptop → Cloud");
        
        // Activate cloud upload when its tab is selected
        connect(data_transfer_tabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (index == 1 && cloud_upload_dialog_) {
                cloud_upload_dialog_->activate();
            }
        });
        
        layout->addWidget(data_transfer_tabs_);
    }
    
    // Ensure dialogs match currently active robot
    applyActiveRobotProfile(true);
    
    // Show and bring to front
    data_transfer_window_->show();
    data_transfer_window_->raise();
    data_transfer_window_->activateWindow();
}

void CoverageGUI::onTransferActive(bool active) {
    // Show/hide the progress widget in the main window
    if (transfer_progress_widget_) {
        transfer_progress_widget_->setVisible(active);
    }
    
    if (!active) {
        // Transfer completed - reset progress
        if (transfer_progress_widget_) {
            transfer_progress_widget_->setProgress(0, 0.0, 0, 0, "");
        }
    }
}

void CoverageGUI::onTransferProgress(int percent, double speedMBps) {
    if (transfer_progress_widget_) {
        auto job = TransferManager::instance().currentJob();
        QString sectionName;
        qint64 bytesTransferred = 0;
        qint64 totalBytes = 0;
        
        if (job) {
            sectionName = job->sectionName;
            bytesTransferred = job->transferredBytes;
            totalBytes = job->totalBytes;
        }
        
        transfer_progress_widget_->setProgress(percent, speedMBps, bytesTransferred, totalBytes, sectionName);
    }
}

void CoverageGUI::onShowTransferDialogRequested() {
    // Re-open the data transfer dialog
    openDataTransferDialog();
}

void CoverageGUI::onCancelTransferRequested() {
    TransferManager::instance().cancelCurrentJob();
}

QGroupBox* CoverageGUI::buildFileControls() {
    QGroupBox* box = new QGroupBox("Point Cloud & Network");
    QVBoxLayout* v = new QVBoxLayout(box);
    
    // Communication architecture: CycloneDDS (loopback) + Zenoh bridge (Microhard RF)
    // CycloneDDS handles local node communication on loopback interface
    // Zenoh bridge DDS handles laptop <-> robot communication over Microhard
    // The Zenoh bridge is managed by laptop_teleop.launch.py (not by this application)
    
    // CycloneDDS status
    lbl_dds_status_ = new QLabel();
    lbl_dds_status_->setStyleSheet("color: #666; font-size: 10px;");
    if (QFile::exists(dds_config_path_)) {
        lbl_dds_status_->setText("DDS: CycloneDDS (loopback) ✓");
        lbl_dds_status_->setStyleSheet("color: green; font-size: 10px;");
    } else {
        lbl_dds_status_->setText("DDS: CycloneDDS config missing (~/" + QFileInfo(dds_config_path_).fileName() + ")");
        lbl_dds_status_->setStyleSheet("color: orange; font-size: 10px;");
    }
    v->addWidget(lbl_dds_status_);
    
    // Zenoh bridge status (monitored, not managed)
    lbl_zenoh_status_ = new QLabel("Zenoh Bridge: checking...");
    lbl_zenoh_status_->setStyleSheet("color: #666; font-size: 10px;");
    lbl_zenoh_status_->setToolTip("Zenoh bridge DDS is managed by laptop_teleop.launch.py\n"
                                   "It bridges ROS2 topics between laptop and robot over Microhard RF");
    v->addWidget(lbl_zenoh_status_);
    
    // Robot identity (hide static IP when registry is present)
    const bool using_registry = robot_registry_.isLoaded() && !robot_registry_.isEmpty();
    if (using_registry) {
        QHBoxLayout* robot_layout = new QHBoxLayout();
        robot_layout->addWidget(new QLabel("Robot:"));
        lbl_active_robot_ = new QLabel(active_robot_id_.isEmpty() ? "(not set)" : active_robot_id_);
        lbl_active_robot_->setToolTip("Selected via robots.json (static IP hidden)");
        robot_layout->addWidget(lbl_active_robot_);
        robot_layout->addStretch();
        v->addLayout(robot_layout);
        txt_robot_ip_ = nullptr;  // Don't expose IP in UI
    } else {
        // Robot IP configuration (legacy)
        QHBoxLayout* ip_layout = new QHBoxLayout();
        ip_layout->addWidget(new QLabel("Robot IP:"));
        txt_robot_ip_ = new QLineEdit(robot_host_);
        txt_robot_ip_->setPlaceholderText("e.g. 192.168.168.101");
        txt_robot_ip_->setToolTip("IP address for fetching maps via SSH");
        ip_layout->addWidget(txt_robot_ip_);
        v->addLayout(ip_layout);
    }
    
    // Load from local file
    QPushButton* btn_load = new QPushButton("Load PCD / PLY / XYZ");
    btn_load->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    connect(btn_load, &QPushButton::clicked, this, &CoverageGUI::loadPointCloud);
    v->addWidget(btn_load);
    
    // Fetch from robot via SSH
    QPushButton* btn_fetch = new QPushButton("📡 Fetch Latest from Robot");
    auto updateFetchTooltip = [this, btn_fetch]() {
        QString display_map_dir = local_map_base_;
        if (display_map_dir.startsWith(QDir::homePath())) {
            display_map_dir.replace(0, QDir::homePath().size(), "~");
        }

        const bool using_registry = robot_registry_.isLoaded() && !robot_registry_.isEmpty();
        const QString target = using_registry
                                   ? (active_robot_id_.isEmpty() ? "Active robot" : active_robot_id_)
                                   : QString("%1@%2").arg(robot_user_, robot_host_);
        btn_fetch->setToolTip(QString("Download the latest map from robot (%1)\nSaves to %2/")
                                  .arg(target, display_map_dir));
    };
    updateFetchTooltip();
    btn_fetch->setObjectName("btn_fetch");  // For theme-aware styling
    connect(btn_fetch, &QPushButton::clicked, this, &CoverageGUI::fetchLatestMapFromRobot);
    
    if (txt_robot_ip_) {
        connect(txt_robot_ip_, &QLineEdit::editingFinished, this, [this, updateFetchTooltip]() mutable {
            QString trimmed = txt_robot_ip_->text().trimmed();
            if (trimmed != txt_robot_ip_->text()) {
                txt_robot_ip_->setText(trimmed);
            }
            robot_host_ = trimmed;
            QSettings settings("PilotControl", "BDRCoveragePlanner");
            settings.setValue("robot_ip", robot_host_);
            updateFetchTooltip();
        });
    }
    v->addWidget(btn_fetch);

    // Map alignment controls (loaded map -> current robot frame).
    QHBoxLayout* align_row = new QHBoxLayout();
    align_row->addWidget(new QLabel("Alignment:"));
    combo_align_mode_ = new QComboBox();
    combo_align_mode_->addItem("Initial guess + ICP refinement", "guess_icp");
    combo_align_mode_->setToolTip(
        "Alignment mode used when fitting loaded map to the latest robot map.\n"
        "Initial guess is entered manually (tx, ty, yaw).\n"
        "The alignment workflow fetches the latest robot map automatically.");
    align_row->addWidget(combo_align_mode_, 1);
    v->addLayout(align_row);

    QHBoxLayout* guess_row = new QHBoxLayout();
    guess_row->addWidget(new QLabel("Guess tx (m):"));
    spin_align_tx_ = new QDoubleSpinBox();
    spin_align_tx_->setRange(-1000.0, 1000.0);
    spin_align_tx_->setDecimals(3);
    spin_align_tx_->setSingleStep(0.1);
    spin_align_tx_->setValue(0.0);
    guess_row->addWidget(spin_align_tx_);
    guess_row->addWidget(new QLabel("ty (m):"));
    spin_align_ty_ = new QDoubleSpinBox();
    spin_align_ty_->setRange(-1000.0, 1000.0);
    spin_align_ty_->setDecimals(3);
    spin_align_ty_->setSingleStep(0.1);
    spin_align_ty_->setValue(0.0);
    guess_row->addWidget(spin_align_ty_);
    guess_row->addWidget(new QLabel("yaw (deg):"));
    spin_align_yaw_deg_ = new QDoubleSpinBox();
    spin_align_yaw_deg_->setRange(-180.0, 180.0);
    spin_align_yaw_deg_->setDecimals(2);
    spin_align_yaw_deg_->setSingleStep(1.0);
    spin_align_yaw_deg_->setValue(0.0);
    guess_row->addWidget(spin_align_yaw_deg_);
    v->addLayout(guess_row);

    btn_align_loaded_map_ = new QPushButton("Align Loaded Map to Current Frame");
    btn_align_loaded_map_->setToolTip(
        "Fetch latest map from robot and align currently loaded map to it.\n"
        "Recompute hull/obstacles/path after alignment.");
    connect(btn_align_loaded_map_, &QPushButton::clicked, this, &CoverageGUI::alignLoadedMapToLatestRobotMap);
    v->addWidget(btn_align_loaded_map_);

    lbl_alignment_status_ = new QLabel("Alignment: not run");
    lbl_alignment_status_->setStyleSheet("color: #666; font-size: 10px;");
    v->addWidget(lbl_alignment_status_);
    
    lbl_file_ = new QLabel("No file loaded");
    v->addWidget(lbl_file_);
    
    return box;
}

QGroupBox* CoverageGUI::buildRobotLoginControls() {
    QGroupBox* box = new QGroupBox("Robot Login");
    QVBoxLayout* v = new QVBoxLayout(box);

    // Robot ID selector
    QHBoxLayout* id_layout = new QHBoxLayout();
    id_layout->addWidget(new QLabel("Robot ID:"));
    combo_robot_id_ = new QComboBox();
    combo_robot_id_->setEditable(true);
    combo_robot_id_->setInsertPolicy(QComboBox::NoInsert);
    combo_robot_id_->setToolTip("Select a robot by ID (static IP is hidden).");

    if (robot_registry_.isLoaded() && !robot_registry_.isEmpty()) {
        combo_robot_id_->addItems(robot_registry_.robotIds());
    }
    const int idx = combo_robot_id_->findText(active_robot_id_, Qt::MatchFixedString);
    if (idx >= 0) {
        combo_robot_id_->setCurrentIndex(idx);
    } else if (!active_robot_id_.isEmpty()) {
        combo_robot_id_->setEditText(active_robot_id_);
    }
    id_layout->addWidget(combo_robot_id_, 1);
    v->addLayout(id_layout);

    // Access code (6-digit PIN)
    QHBoxLayout* pin_layout = new QHBoxLayout();
    pin_layout->addWidget(new QLabel("Access code:"));
    txt_access_code_ = new QLineEdit();
    txt_access_code_->setEchoMode(QLineEdit::Password);
    txt_access_code_->setMaxLength(6);
    txt_access_code_->setPlaceholderText("6 digits");
    txt_access_code_->setToolTip("6-digit PIN (checked only during login).");
    txt_access_code_->setValidator(new QRegularExpressionValidator(QRegularExpression("^\\d{0,6}$"), txt_access_code_));
    pin_layout->addWidget(txt_access_code_, 1);
    v->addLayout(pin_layout);

    // Action row
    QHBoxLayout* action_layout = new QHBoxLayout();
    btn_robot_login_ = new QPushButton("Login");
    btn_robot_login_->setMinimumHeight(32);
    action_layout->addWidget(btn_robot_login_);

    lbl_login_status_ = new QLabel("Not logged in");
    lbl_login_status_->setStyleSheet("color: #666; font-size: 10px;");
    action_layout->addWidget(lbl_login_status_, 1);
    v->addLayout(action_layout);

    lbl_login_expiry_ = new QLabel();
    lbl_login_expiry_->setStyleSheet("color: #666; font-size: 10px;");
    v->addWidget(lbl_login_expiry_);

    // Countdown timer (1 Hz)
    login_countdown_timer_ = new QTimer(this);
    login_countdown_timer_->setInterval(1000);
    connect(login_countdown_timer_, &QTimer::timeout, this, &CoverageGUI::onLoginCountdownTick);

    // Hook up UI actions
    connect(btn_robot_login_, &QPushButton::clicked, this, &CoverageGUI::onRobotLoginClicked);
    connect(combo_robot_id_, &QComboBox::currentTextChanged, this, &CoverageGUI::onRobotIdChanged);

    updateLoginUi();
    return box;
}

QGroupBox* CoverageGUI::buildRobotTrackingControls() {
    QGroupBox* box = new QGroupBox("Robot Tracking");
    QVBoxLayout* layout = new QVBoxLayout(box);
    
    QHBoxLayout* topic_layout = new QHBoxLayout();
    topic_layout->addWidget(new QLabel("Odom topic:"));
    txt_robot_topic_ = new QLineEdit(robot_odom_topic_);
    txt_robot_topic_->setPlaceholderText("/Odometry_tilt_corrected_diff");
    topic_layout->addWidget(txt_robot_topic_);
    layout->addLayout(topic_layout);
    
    QHBoxLayout* size_layout = new QHBoxLayout();
    size_layout->addWidget(new QLabel("Marker size (m):"));
    spin_robot_marker_size_ = new QDoubleSpinBox();
    spin_robot_marker_size_->setRange(0.05, 3.0);
    spin_robot_marker_size_->setSingleStep(0.1);
    spin_robot_marker_size_->setValue(robot_marker_size_m_);
    spin_robot_marker_size_->setToolTip("Approximate base length of the robot heading triangle");
    size_layout->addWidget(spin_robot_marker_size_);
    layout->addLayout(size_layout);
    
    
    chk_show_robot_ = new QCheckBox("Show live robot overlay");
    chk_show_robot_->setChecked(true);
    
    btn_clear_robot_trail_ = new QPushButton("Clear trail");
    btn_clear_robot_trail_->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    btn_load_trail_bag_ = new QPushButton("Load Trail from Rosbag");
    btn_load_trail_bag_->setToolTip(
        "Import odometry trail from a rosbag2 folder.\n"
        "Imported trail is transformed with the same map alignment transform.");
    
    lbl_robot_status_ = new QLabel();
    lbl_robot_status_->setStyleSheet("color: #a66f00; font-size: 10px;");
    updateRobotStatusLabel(false);
    
    connect(chk_show_robot_, &QCheckBox::toggled, this, [this]() {
        refreshPlot();
    });
    connect(btn_clear_robot_trail_, &QPushButton::clicked, this, &CoverageGUI::clearRobotTrail);
    connect(btn_load_trail_bag_, &QPushButton::clicked, this, &CoverageGUI::loadTrailFromRosbag);
    connect(txt_robot_topic_, &QLineEdit::editingFinished, this, [this]() {
        QString trimmed = txt_robot_topic_->text().trimmed();
        if (trimmed.isEmpty()) {
            trimmed = "/Odometry_tilt_corrected_diff";
            txt_robot_topic_->setText(trimmed);
        }
        if (trimmed == robot_odom_topic_) {
            return;
        }
        robot_odom_topic_ = trimmed;
        QSettings settings("PilotControl", "BDRCoveragePlanner");
        settings.setValue("robot_odom_topic", robot_odom_topic_);
        updateRobotStatusLabel(false);
        setupRobotTrackingSubscription();
    });
    connect(spin_robot_marker_size_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        robot_marker_size_m_ = value;
        QSettings settings("PilotControl", "BDRCoveragePlanner");
        settings.setValue("robot_marker_size_m", robot_marker_size_m_);
        plot_->setRobotMarkerSize(robot_marker_size_m_);
        refreshPlot();
    });
    
    layout->addWidget(chk_show_robot_);
    layout->addWidget(btn_clear_robot_trail_);
    layout->addWidget(btn_load_trail_bag_);
    layout->addWidget(lbl_robot_status_);
    
    return box;
}

QGroupBox* CoverageGUI::buildHeightControls() {
    QGroupBox* box = new QGroupBox("Height Cropping & 3D View");
    QVBoxLayout* v = new QVBoxLayout(box);
    
    // Info label explaining the Z range filtering
    QLabel* info_label = new QLabel("Filter points relative to robot origin (Z=0):");
    info_label->setStyleSheet("color: #666; font-size: 10px;");
    v->addWidget(info_label);
    
    // Z minimum control (can be negative for below robot)
    QHBoxLayout* h_min = new QHBoxLayout();
    h_min->addWidget(new QLabel("Z min (m):"));
    spin_z_min_ = new QDoubleSpinBox();
    spin_z_min_->setRange(-50.0, 50.0);
    spin_z_min_->setSingleStep(0.05);
    spin_z_min_->setValue(-0.1);  // Default: 0.1m below robot
    spin_z_min_->setToolTip("Minimum Z value (negative = below robot origin)");
    h_min->addWidget(spin_z_min_);
    v->addLayout(h_min);
    
    // Z maximum control
    QHBoxLayout* h_max = new QHBoxLayout();
    h_max->addWidget(new QLabel("Z max (m):"));
    spin_z_max_ = new QDoubleSpinBox();
    spin_z_max_->setRange(-50.0, 50.0);
    spin_z_max_->setSingleStep(0.05);
    spin_z_max_->setValue(0.1);  // Default: 0.1m above robot
    spin_z_max_->setToolTip("Maximum Z value (positive = above robot origin)");
    h_max->addWidget(spin_z_max_);
    v->addLayout(h_max);
    
    QPushButton* btn_apply = new QPushButton("Apply Height Crop");
    btn_apply->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    btn_apply->setToolTip("Keep only points with Z between Z min and Z max\n"
                          "(relative to robot origin at Z=0)");
    connect(btn_apply, &QPushButton::clicked, this, &CoverageGUI::applyHeightCrop);
    v->addWidget(btn_apply);
    
    // 3D Visualization button
    QPushButton* btn_view3d = new QPushButton("View Point Cloud (3D)");
    btn_view3d->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    btn_view3d->setToolTip("Open interactive 3D viewer for the point cloud.\n"
                          "Controls:\n"
                          "  • Left mouse: Rotate view\n"
                          "  • Middle mouse / Shift+Left: Pan\n"
                          "  • Scroll wheel: Zoom\n"
                          "  • R: Reset camera\n"
                          "  • C: Show camera parameters\n"
                          "  • +/-: Increase/decrease point size\n"
                          "  • G: Toggle coordinate system\n"
                          "  • Q: Close viewer");
    connect(btn_view3d, &QPushButton::clicked, this, &CoverageGUI::showPointCloud3D);
    v->addWidget(btn_view3d);
    
    return box;
}

QGroupBox* CoverageGUI::buildDownsampleControls() {
    QGroupBox* box = new QGroupBox("Downsampling");
    QVBoxLayout* v = new QVBoxLayout(box);
    
    QHBoxLayout* method_row = new QHBoxLayout();
    method_row->addWidget(new QLabel("Method"));
    combo_downsample_ = new QComboBox();
    combo_downsample_->addItems({"None", "Random", "Voxel", "Statistical"});
    connect(combo_downsample_, &QComboBox::currentTextChanged, this, &CoverageGUI::updateDownsampleUI);
    method_row->addWidget(combo_downsample_);
    v->addLayout(method_row);
    
    // Random params
    group_random_ = new QGroupBox("Random settings");
    QHBoxLayout* rand_layout = new QHBoxLayout(group_random_);
    rand_layout->addWidget(new QLabel("Max points"));
    spin_max_points_ = new QSpinBox();
    spin_max_points_->setRange(100, 2000000);
    spin_max_points_->setValue(50000);
    rand_layout->addWidget(spin_max_points_);
    v->addWidget(group_random_);
    
    // Voxel params
    group_voxel_ = new QGroupBox("Voxel settings");
    QHBoxLayout* voxel_layout = new QHBoxLayout(group_voxel_);
    voxel_layout->addWidget(new QLabel("Voxel size"));
    spin_voxel_ = new QDoubleSpinBox();
    spin_voxel_->setRange(0.001, 1.0);
    spin_voxel_->setSingleStep(0.005);
    spin_voxel_->setValue(0.05);
    voxel_layout->addWidget(spin_voxel_);
    v->addWidget(group_voxel_);
    
    // Statistical params
    group_stat_ = new QGroupBox("Statistical settings");
    QHBoxLayout* stat_layout = new QHBoxLayout(group_stat_);
    stat_layout->addWidget(new QLabel("Mean K"));
    spin_mean_k_ = new QSpinBox();
    spin_mean_k_->setRange(5, 1000);
    spin_mean_k_->setValue(20);
    stat_layout->addWidget(spin_mean_k_);
    stat_layout->addWidget(new QLabel("Std Ratio"));
    spin_std_ratio_ = new QDoubleSpinBox();
    spin_std_ratio_->setRange(0.1, 5.0);
    spin_std_ratio_->setSingleStep(0.1);
    spin_std_ratio_->setValue(1.0);
    stat_layout->addWidget(spin_std_ratio_);
    v->addWidget(group_stat_);
    
    updateDownsampleUI("None");
    
    QPushButton* btn_down = new QPushButton("Downsample");
    btn_down->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    connect(btn_down, &QPushButton::clicked, this, &CoverageGUI::applyDownsample);
    v->addWidget(btn_down);
    
    return box;
}

QGroupBox* CoverageGUI::buildHullControls() {
    QGroupBox* box = new QGroupBox("2D Projection & Concave Hull");
    QVBoxLayout* v = new QVBoxLayout(box);
    
    QHBoxLayout* h_method = new QHBoxLayout();
    h_method->addWidget(new QLabel("Method:"));
    combo_hull_method_ = new QComboBox();
    combo_hull_method_->addItem("AlphaShape", "alphashape");
    combo_hull_method_->addItem("Delaunay", "delaunay");
    combo_hull_method_->addItem("Grid", "grid");
    h_method->addWidget(combo_hull_method_);
    v->addLayout(h_method);
    
    QHBoxLayout* h = new QHBoxLayout();
    h->addWidget(new QLabel("Parameter:"));
    spin_alpha_ = new QDoubleSpinBox();
    spin_alpha_->setRange(0.01, 10.0);
    spin_alpha_->setSingleStep(0.1);
    spin_alpha_->setValue(1.5);
    spin_alpha_->setToolTip("AlphaShape: smaller=more detail. Grid: grid cell size");
    h->addWidget(spin_alpha_);
    v->addLayout(h);
    
    QPushButton* btn_proj = new QPushButton("Project to 2D & Compute Hull");
    btn_proj->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    connect(btn_proj, &QPushButton::clicked, this, &CoverageGUI::computeHull);
    v->addWidget(btn_proj);
    
    return box;
}

QGroupBox* CoverageGUI::buildSimplifyControls() {
    QGroupBox* box = new QGroupBox("Polygon Simplification");
    QVBoxLayout* v = new QVBoxLayout(box);
    
    QHBoxLayout* h = new QHBoxLayout();
    h->addWidget(new QLabel("Tolerance"));
    spin_simplify_ = new QDoubleSpinBox();
    spin_simplify_->setRange(0.0, 5.0);
    spin_simplify_->setSingleStep(0.05);
    spin_simplify_->setValue(0.1);
    h->addWidget(spin_simplify_);
    v->addLayout(h);
    
    QPushButton* btn_simplify = new QPushButton("Simplify Polygon");
    btn_simplify->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    connect(btn_simplify, &QPushButton::clicked, this, &CoverageGUI::simplifyPolygon);
    v->addWidget(btn_simplify);
    
    return box;
}

QGroupBox* CoverageGUI::buildPathPlanningControls() {
    QGroupBox* box = new QGroupBox("Path Planning");
    QVBoxLayout* v = new QVBoxLayout(box);
    
    // Mode selector
    QHBoxLayout* mode_layout = new QHBoxLayout();
    mode_layout->addWidget(new QLabel("Mode:"));
    radio_mode_f2c_ = new QRadioButton("Coverage planning");
    radio_mode_custom_ = new QRadioButton("Custom Path");
    radio_mode_f2c_->setChecked(true);
    radio_mode_f2c_->setToolTip("Use Fields2Cover library for coverage path planning");
    radio_mode_custom_->setToolTip("Draw custom waypoints on the map");
    mode_layout->addWidget(radio_mode_f2c_);
    mode_layout->addWidget(radio_mode_custom_);
    mode_layout->addStretch();
    v->addLayout(mode_layout);
    
    // Coverage planning controls container
    f2c_controls_widget_ = buildF2CControls();
    v->addWidget(f2c_controls_widget_);
    
    // Custom path controls container
    custom_controls_widget_ = buildCustomPathControls();
    custom_controls_widget_->setVisible(false);
    v->addWidget(custom_controls_widget_);
    
    return box;
}

QWidget* CoverageGUI::buildF2CControls() {
    QWidget* widget = new QWidget();
    QVBoxLayout* v = new QVBoxLayout(widget);
    v->setContentsMargins(0, 0, 0, 0);
    
    // Swath width
    QHBoxLayout* h1 = new QHBoxLayout();
    h1->addWidget(new QLabel("Swath width"));
    spin_swath_ = new QDoubleSpinBox();
    spin_swath_->setRange(0.05, 10.0);
    spin_swath_->setSingleStep(0.05);
    spin_swath_->setValue(1.0);
    h1->addWidget(spin_swath_);
    v->addLayout(h1);
    
    // Headland width
    QHBoxLayout* h2 = new QHBoxLayout();
    h2->addWidget(new QLabel("Headland width"));
    spin_headland_ = new QDoubleSpinBox();
    spin_headland_->setRange(0.0, 10.0);
    spin_headland_->setSingleStep(0.1);
    spin_headland_->setValue(1.0);
    h2->addWidget(spin_headland_);
    v->addLayout(h2);
    
    // Turn radius
    QHBoxLayout* h3 = new QHBoxLayout();
    h3->addWidget(new QLabel("Turn radius"));
    spin_turn_ = new QDoubleSpinBox();
    spin_turn_->setRange(0.0, 20.0);
    spin_turn_->setSingleStep(0.1);
    spin_turn_->setValue(0.5);
    h3->addWidget(spin_turn_);
    v->addLayout(h3);
    
    // Auto-align
    chk_auto_align_ = new QCheckBox("Auto-align to building");
    v->addWidget(chk_auto_align_);
    
    QHBoxLayout* align_box = new QHBoxLayout();
    radio_long_ = new QRadioButton("Parallel (long edge)");
    radio_perp_ = new QRadioButton("Perpendicular");
    radio_perp_->setChecked(true);
    align_box->addWidget(radio_long_);
    align_box->addWidget(radio_perp_);
    v->addLayout(align_box);
    
    // Route pattern
    QHBoxLayout* route_layout = new QHBoxLayout();
    route_layout->addWidget(new QLabel("Route pattern"));
    combo_route_pattern_ = new QComboBox();
    combo_route_pattern_->addItem("Boustrophedon", "boustro");
    combo_route_pattern_->addItem("Snake", "snake");
    combo_route_pattern_->addItem("Spiral", "spiral");
    route_layout->addWidget(combo_route_pattern_);
    v->addLayout(route_layout);
    
    // Path planner
    QHBoxLayout* planner_layout = new QHBoxLayout();
    planner_layout->addWidget(new QLabel("Path planner"));
    combo_path_planner_ = new QComboBox();
    combo_path_planner_->addItem("Dubins curves", "dubins");
    combo_path_planner_->addItem("Dubins curves (CC)", "dubins_cc");
    combo_path_planner_->addItem("Reeds-Shepp", "reeds");
    combo_path_planner_->addItem("Reeds-Shepp (HC)", "reeds_hc");
    combo_path_planner_->addItem("Straight", "none");
    planner_layout->addWidget(combo_path_planner_);
    v->addLayout(planner_layout);

    // Waypoint spacing (controller-friendly resampling)
    QHBoxLayout* spacing_layout = new QHBoxLayout();
    spacing_layout->addWidget(new QLabel("Waypoint spacing (m)"));
    spin_waypoint_spacing_ = new QDoubleSpinBox();
    spin_waypoint_spacing_->setRange(0.0, 2.0);
    spin_waypoint_spacing_->setSingleStep(0.05);
    spin_waypoint_spacing_->setValue(0.10);  // 10 cm default; set to 0 to disable
    spin_waypoint_spacing_->setToolTip(
        "If > 0, the generated path will be resampled to roughly this spacing.\n"
        "This can reduce jitter from uneven spacing and makes controller tracking smoother.\n"
        "Set to 0 to disable resampling."
    );
    spacing_layout->addWidget(spin_waypoint_spacing_);
    v->addLayout(spacing_layout);
    
    // Axial turns
    chk_axial_turns_ = new QCheckBox("Use axial turns (zero radius)");
    chk_axial_turns_->setToolTip("For robots that can turn in place");
    v->addWidget(chk_axial_turns_);
    
    // Decomposition
    chk_decomposition_ = new QCheckBox("Use decomposition (concave fields)");
    v->addWidget(chk_decomposition_);
    
    QHBoxLayout* decomp_layout = new QHBoxLayout();
    decomp_layout->addWidget(new QLabel("Decomposition type"));
    combo_decomp_type_ = new QComboBox();
    combo_decomp_type_->addItem("Boustrophedon", "boustrophedon");
    combo_decomp_type_->addItem("Trapezoidal", "trapezoidal");
    decomp_layout->addWidget(combo_decomp_type_);
    v->addLayout(decomp_layout);
    
    // ROI controls
    QHBoxLayout* roi_box = new QHBoxLayout();
    btn_roi_ = new QPushButton("Select ROI");
    btn_roi_->setCheckable(true);
    btn_roi_->setIcon(style()->standardIcon(QStyle::SP_DialogYesButton));
    connect(btn_roi_, &QPushButton::clicked, this, &CoverageGUI::toggleROISelection);
    roi_box->addWidget(btn_roi_);
    
    // Rectangle drawing tool (3-click)
    btn_rectangle_ = new QPushButton("📐 Rectangle");
    btn_rectangle_->setCheckable(true);
    btn_rectangle_->setToolTip("Draw a rectangle ROI with 3 clicks:\n"
                               "1. First corner\n"
                               "2. Defines base edge direction\n"
                               "3. Sets width");
    connect(btn_rectangle_, &QPushButton::clicked, this, &CoverageGUI::toggleRectangleMode);
    roi_box->addWidget(btn_rectangle_);
    
    btn_roi_clear_ = new QPushButton("Clear ROI");
    btn_roi_clear_->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
    connect(btn_roi_clear_, &QPushButton::clicked, this, &CoverageGUI::clearROI);
    roi_box->addWidget(btn_roi_clear_);
    v->addLayout(roi_box);
    
    QHBoxLayout* roi_actions = new QHBoxLayout();
    btn_roi_finish_ = new QPushButton("Finish");
    btn_roi_finish_->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    connect(btn_roi_finish_, &QPushButton::clicked, this, &CoverageGUI::finishSelection);
    roi_actions->addWidget(btn_roi_finish_);
    
    btn_roi_undo_ = new QPushButton("Undo Point");
    btn_roi_undo_->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    connect(btn_roi_undo_, &QPushButton::clicked, this, &CoverageGUI::undoSelectionPoint);
    roi_actions->addWidget(btn_roi_undo_);
    v->addLayout(roi_actions);
    
    lbl_roi_ = new QLabel("ROI: none");
    v->addWidget(lbl_roi_);

    // Obstacle controls
    QHBoxLayout* obstacle_box = new QHBoxLayout();
    btn_obstacle_ = new QPushButton("Add Obstacle");
    btn_obstacle_->setCheckable(true);
    btn_obstacle_->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
    connect(btn_obstacle_, &QPushButton::clicked, this, &CoverageGUI::toggleObstacleSelection);
    obstacle_box->addWidget(btn_obstacle_);
    
    btn_obstacle_clear_ = new QPushButton("Clear Obstacles");
    btn_obstacle_clear_->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    connect(btn_obstacle_clear_, &QPushButton::clicked, this, &CoverageGUI::clearObstacles);
    obstacle_box->addWidget(btn_obstacle_clear_);
    v->addLayout(obstacle_box);

    btn_delete_selected_obstacle_ = new QPushButton("Delete Selected Obstacle");
    btn_delete_selected_obstacle_->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    btn_delete_selected_obstacle_->setEnabled(false);
    btn_delete_selected_obstacle_->setToolTip("Delete the currently selected obstacle (click an obstacle in the plot).");
    connect(btn_delete_selected_obstacle_, &QPushButton::clicked, this, &CoverageGUI::deleteSelectedObstacle);
    v->addWidget(btn_delete_selected_obstacle_);

    btn_auto_detect_obstacles_ = new QPushButton("Auto-detect Obstacles");
    btn_auto_detect_obstacles_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    btn_auto_detect_obstacles_->setToolTip(
        "Automatically detect obstacles from the full loaded point cloud\n"
        "(ignores view crop/downsample), using the driven robot trail\n"
        "(AUTO mode; supports holes when hollow).");
    connect(btn_auto_detect_obstacles_, &QPushButton::clicked, this, &CoverageGUI::autoDetectObstacles);
    v->addWidget(btn_auto_detect_obstacles_);
    
    lbl_obstacles_ = new QLabel("Obstacles: 0");
    v->addWidget(lbl_obstacles_);
    
    // Separator
    QFrame* sep = new QFrame();
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    v->addWidget(sep);
    
    // Preset controls
    v->addWidget(buildPresetControls());
    
    // Separator before generation
    QFrame* sep2 = new QFrame();
    sep2->setFrameShape(QFrame::HLine);
    sep2->setFrameShadow(QFrame::Sunken);
    v->addWidget(sep2);

    // Waypoint/DC options
    chk_plan_to_first_ = new QCheckBox("Plan path to first point");
    chk_plan_to_first_->setChecked(true);
    chk_plan_to_first_->setToolTip(
        "If enabled, compute an obstacle-avoiding connector from the robot\n"
        "to the first waypoint (connector waypoints are tagged with DC=0)."
    );
    v->addWidget(chk_plan_to_first_);

    chk_collect_data_ = new QCheckBox("Collect data");
    chk_collect_data_->setChecked(true);
    chk_collect_data_->setToolTip(
        "If enabled, coverage path waypoints are tagged with DC=1 so the\n"
        "autonomy controller will start data collection after reaching them."
    );
    v->addWidget(chk_collect_data_);
    
    // Generation buttons
    QPushButton* btn_swaths = new QPushButton("Generate Swaths");
    btn_swaths->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    connect(btn_swaths, &QPushButton::clicked, this, &CoverageGUI::generateSwaths);
    v->addWidget(btn_swaths);
    
    QPushButton* btn_route = new QPushButton("Generate Route");
    btn_route->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    connect(btn_route, &QPushButton::clicked, this, &CoverageGUI::generateRoute);
    v->addWidget(btn_route);
    
    QPushButton* btn_clear = new QPushButton("Clear Swaths/Path");
    btn_clear->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    connect(btn_clear, &QPushButton::clicked, this, &CoverageGUI::clearCoverage);
    v->addWidget(btn_clear);
    
    QPushButton* btn_path = new QPushButton("Generate Path");
    btn_path->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    connect(btn_path, &QPushButton::clicked, this, &CoverageGUI::generatePath);
    v->addWidget(btn_path);
    
    return widget;
}

QWidget* CoverageGUI::buildCustomPathControls() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    
    QLabel* instructions = new QLabel(
        "Click 'Enable drawing' then click on the map to drop waypoints.\n"
        "Use 'Publish Waypoints' below to send to robot.");
    instructions->setWordWrap(true);
    instructions->setStyleSheet("color: #666; font-size: 10px;");
    layout->addWidget(instructions);
    
    btn_custom_draw_ = new QPushButton("Enable drawing");
    btn_custom_draw_->setCheckable(true);
    btn_custom_draw_->setObjectName("btn_custom_draw");  // For theme-aware styling
    layout->addWidget(btn_custom_draw_);
    
    QHBoxLayout* edit_layout = new QHBoxLayout();
    btn_custom_undo_ = new QPushButton("Undo Last");
    btn_custom_undo_->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    btn_custom_clear_ = new QPushButton("Clear All");
    btn_custom_clear_->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    edit_layout->addWidget(btn_custom_undo_);
    edit_layout->addWidget(btn_custom_clear_);
    layout->addLayout(edit_layout);
    
    list_custom_points_ = new QListWidget();
    list_custom_points_->setMaximumHeight(150);
    layout->addWidget(list_custom_points_);
    
    lbl_custom_status_ = new QLabel("No custom waypoints yet.");
    lbl_custom_status_->setStyleSheet("color: #777; font-size: 10px;");
    layout->addWidget(lbl_custom_status_);
    
    return widget;
}

QGroupBox* CoverageGUI::buildCoverageStatsControls() {
    stats_group_ = new QGroupBox("Coverage Statistics");
    QVBoxLayout* layout = new QVBoxLayout(stats_group_);
    
    // Robot speed for time estimation
    QHBoxLayout* speed_layout = new QHBoxLayout();
    speed_layout->addWidget(new QLabel("Robot speed (m/s):"));
    spin_robot_speed_ = new QDoubleSpinBox();
    spin_robot_speed_->setRange(0.1, 5.0);
    spin_robot_speed_->setSingleStep(0.1);
    spin_robot_speed_->setValue(0.5);
    spin_robot_speed_->setToolTip("Used to estimate mission time");
    speed_layout->addWidget(spin_robot_speed_);
    layout->addLayout(speed_layout);
    
    connect(spin_robot_speed_, qOverload<double>(&QDoubleSpinBox::valueChanged), 
            this, &CoverageGUI::updateCoverageStats);
    
    // Stats labels
    QGridLayout* grid = new QGridLayout();
    grid->setColumnStretch(1, 1);
    
    auto addStatRow = [&](int row, const QString& label, QLabel*& valueLabel) {
        grid->addWidget(new QLabel(label), row, 0);
        valueLabel = new QLabel("-");
        valueLabel->setStyleSheet("font-weight: bold;");
        grid->addWidget(valueLabel, row, 1, Qt::AlignRight);
    };
    
    addStatRow(0, "Path length:", lbl_stats_path_length_);
    addStatRow(1, "Coverage area:", lbl_stats_area_);
    addStatRow(2, "Field area:", lbl_stats_coverage_);
    addStatRow(3, "Swaths:", lbl_stats_swaths_);
    addStatRow(4, "Turns:", lbl_stats_turns_);
    addStatRow(5, "Waypoints:", lbl_stats_waypoints_);
    addStatRow(6, "Est. time:", lbl_stats_time_);
    addStatRow(7, "Live progress:", lbl_stats_live_progress_);
    addStatRow(8, "Distance traveled:", lbl_stats_live_travel_);
    addStatRow(9, "Remaining:", lbl_stats_live_remaining_);
    addStatRow(10, "ETA:", lbl_stats_live_eta_);
    addStatRow(11, "Elapsed:", lbl_stats_live_elapsed_);
    addStatRow(12, "Speed:", lbl_stats_live_speed_);
    
    layout->addLayout(grid);
    
    // Live overlay controls
    QGroupBox* overlay_group = new QGroupBox("Live overlay on plot");
    QVBoxLayout* overlay_layout = new QVBoxLayout(overlay_group);
    chk_live_overlay_ = new QCheckBox("Show live stats overlay (top-right)");
    chk_live_overlay_->setChecked(true);
    overlay_layout->addWidget(chk_live_overlay_);
    
    auto addOverlayCheck = [&](const QString& label, QCheckBox*& box, bool checked) {
        box = new QCheckBox(label);
        box->setChecked(checked);
        overlay_layout->addWidget(box);
    };
    
    addOverlayCheck("Progress", chk_live_show_progress_, true);
    addOverlayCheck("Distance traveled", chk_live_show_travel_, true);
    addOverlayCheck("Remaining", chk_live_show_remaining_, true);
    addOverlayCheck("ETA", chk_live_show_eta_, true);
    addOverlayCheck("Elapsed", chk_live_show_elapsed_, true);
    addOverlayCheck("Speed", chk_live_show_speed_, true);
    
    layout->addWidget(overlay_group);
    
    // Refresh button
    QPushButton* btn_refresh_stats = new QPushButton("Refresh Statistics");
    btn_refresh_stats->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    connect(btn_refresh_stats, &QPushButton::clicked, this, &CoverageGUI::updateCoverageStats);
    layout->addWidget(btn_refresh_stats);
    
    return stats_group_;
}

QGroupBox* CoverageGUI::buildExportControls() {
    QGroupBox* box = new QGroupBox("Export & Navigation");
    QVBoxLayout* v = new QVBoxLayout(box);

    QPushButton* btn_export_path = new QPushButton("Export Path CSV");
    btn_export_path->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    connect(btn_export_path, &QPushButton::clicked, this, &CoverageGUI::exportPathCSV);
    v->addWidget(btn_export_path);

    // Add waypoint publishing buttons (work for both F2C and Custom modes)
    btn_publish_waypoints_ = new QPushButton("⬆ Publish Waypoints to Robot");
    btn_publish_waypoints_->setObjectName("btn_publish");  // For theme-aware styling
    connect(btn_publish_waypoints_, &QPushButton::clicked, this, &CoverageGUI::publishWaypoints);
    v->addWidget(btn_publish_waypoints_);

    btn_start_navigation_ = new QPushButton("▶️ Start Navigation");
    btn_start_navigation_->setObjectName("btn_navigation");  // For theme-aware styling
    btn_start_navigation_->setEnabled(false);  // Initially disabled
    connect(btn_start_navigation_, &QPushButton::clicked, this, &CoverageGUI::startNavigation);
    v->addWidget(btn_start_navigation_);

    btn_go_to_ = new QPushButton("🎯 GO TO (Pick on Map)");
    btn_go_to_->setCheckable(true);
    btn_go_to_->setToolTip(
        "Click to arm GO TO mode, then click a destination point on the map.\n"
        "Planner will build an obstacle-avoiding path from current robot pose.");
    connect(btn_go_to_, &QPushButton::clicked, this, &CoverageGUI::onGoToClicked);
    v->addWidget(btn_go_to_);

    // Reprojection error analysis section
    QFrame* sep = new QFrame();
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    v->addWidget(sep);
    
    QLabel* reproj_title = new QLabel("Path Accuracy Analysis");
    reproj_title->setStyleSheet("font-weight: bold; margin-top: 5px;");
    v->addWidget(reproj_title);
    
    btn_compute_reproj_ = new QPushButton("📊 Compute Reprojection Error");
    btn_compute_reproj_->setToolTip("Compare robot trail to planned path (within 1m)");
    connect(btn_compute_reproj_, &QPushButton::clicked, this, &CoverageGUI::computeReprojectionError);
    v->addWidget(btn_compute_reproj_);
    
    btn_clear_reproj_ = new QPushButton("Clear Reprojection");
    btn_clear_reproj_->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    connect(btn_clear_reproj_, &QPushButton::clicked, this, &CoverageGUI::clearReprojectionError);
    v->addWidget(btn_clear_reproj_);
    
    lbl_reproj_status_ = new QLabel("No reprojection computed");
    lbl_reproj_status_->setStyleSheet("color: #666; font-size: 10px;");
    v->addWidget(lbl_reproj_status_);

    return box;
}

QWidget* CoverageGUI::buildLeftMiniPalette() {
    QWidget* mini = new QWidget();
    mini->setFixedWidth(72);
    QVBoxLayout* v = new QVBoxLayout(mini);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(6);
    
    QLabel* title = new QLabel("Panels");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-weight: bold; font-size: 10px;");
    v->addWidget(title);
    
    auto addChip = [&](const QString& text) {
        QToolButton* btn = new QToolButton();
        btn->setText(text);
        btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        btn->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
        btn->setIconSize(QSize(18, 18));
        btn->setAutoRaise(true);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        connect(btn, &QToolButton::clicked, this, &CoverageGUI::toggleLeftPane);
        v->addWidget(btn);
    };
    
    addChip("Files");
    addChip("Robot");
    addChip("Height");
    addChip("Filter");
    addChip("Hull");
    addChip("Plan");
    addChip("Stats");
    addChip("Export");
    v->addStretch();
    return mini;
}

QWidget* CoverageGUI::buildRightMiniPalette() {
    QWidget* mini = new QWidget();
    mini->setFixedWidth(72);
    QVBoxLayout* v = new QVBoxLayout(mini);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(6);
    
    QLabel* title = new QLabel("Right");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-weight: bold; font-size: 10px;");
    v->addWidget(title);
    
    auto addChip = [&](const QString& text) {
        QToolButton* btn = new QToolButton();
        btn->setText(text);
        btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        btn->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
        btn->setIconSize(QSize(18, 18));
        btn->setAutoRaise(true);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        connect(btn, &QToolButton::clicked, this, &CoverageGUI::toggleRightPane);
        v->addWidget(btn);
    };
    
    addChip("Layers");
    addChip("Video");
    v->addStretch();
    return mini;
}

QWidget* CoverageGUI::buildRightFullPane() {
    QWidget* pane = new QWidget();
    QVBoxLayout* v = new QVBoxLayout(pane);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(6);
    
    // Layers panel (compact 2-column layout)
    QWidget* layers = buildLayerPanel();
    v->addWidget(layers);
    
    // Scan planner panel
    QGroupBox* scan_panel = buildScanPlannerPanel();
    v->addWidget(scan_panel);
    
    // Video panel
    video_panel_widget_ = buildVideoPanelWidget();
    v->addWidget(video_panel_widget_);
    
    // Data transfer panel
    data_transfer_panel_ = buildDataTransferPanel();
    v->addWidget(data_transfer_panel_);
    
    v->addStretch();
    return pane;
}

QWidget* CoverageGUI::buildDataTransferPanel() {
    QGroupBox* box = new QGroupBox("Data Transfer");
    QVBoxLayout* layout = new QVBoxLayout(box);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    
    // Single button opens tabbed dialog (download + upload)
    btn_open_transfer_dialog_ = new QPushButton("📥 Data Transfer");
    btn_open_transfer_dialog_->setMinimumHeight(32);
    btn_open_transfer_dialog_->setToolTip("Download from robot / Upload to AWS S3");
    btn_open_transfer_dialog_->setStyleSheet(
        "QPushButton { font-weight: bold; }");
    connect(btn_open_transfer_dialog_, &QPushButton::clicked, 
            this, &CoverageGUI::openDataTransferDialog);
    layout->addWidget(btn_open_transfer_dialog_);
    
    // Progress widget (hidden by default, shows during active transfers)
    transfer_progress_widget_ = new TransferProgressWidget();
    transfer_progress_widget_->setVisible(false);
    connect(transfer_progress_widget_, &TransferProgressWidget::showDialogRequested,
            this, &CoverageGUI::onShowTransferDialogRequested);
    connect(transfer_progress_widget_, &TransferProgressWidget::cancelRequested,
            this, &CoverageGUI::onCancelTransferRequested);
    layout->addWidget(transfer_progress_widget_);
    
    return box;
}

// =============================================================================
// Workflow Indicator
// =============================================================================

QWidget* CoverageGUI::buildWorkflowIndicator() {
    QWidget* widget = new QWidget();
    widget->setObjectName("workflowIndicator");
    widget->setFixedHeight(50);
    
    QHBoxLayout* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(10, 5, 10, 5);
    layout->setSpacing(0);
    
    QStringList steps = {"Load", "Filter", "Hull", "Plan", "Export"};
    QStringList icons = {"📁", "🔧", "⬡", "🗺️", "🚀"};
    
    workflow_btns_.clear();
    
    for (int i = 0; i < steps.size(); ++i) {
        // Step button
        QPushButton* btn = new QPushButton(QString("%1 %2").arg(icons[i], steps[i]));
        btn->setObjectName(QString("workflow_step_%1").arg(i));
        btn->setCheckable(true);
        btn->setMinimumWidth(80);
        btn->setProperty("step_index", i);
        connect(btn, &QPushButton::clicked, this, [this, i]() {
            onWorkflowStepClicked(i);
        });
        workflow_btns_.push_back(btn);
        layout->addWidget(btn);
        
        // Arrow between steps (except after last)
        if (i < steps.size() - 1) {
            QLabel* arrow = new QLabel(" → ");
            arrow->setStyleSheet("color: #888; font-size: 16px;");
            layout->addWidget(arrow);
        }
    }
    
    layout->addStretch();
    
    // Style the workflow widget
    widget->setStyleSheet(R"(
        #workflowIndicator {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 #f8f9fa, stop:1 #e9ecef);
            border-bottom: 1px solid #dee2e6;
        }
        #workflowIndicator QPushButton {
            border: 2px solid #adb5bd;
            border-radius: 8px;
            padding: 8px 12px;
            background: white;
            font-weight: bold;
        }
        #workflowIndicator QPushButton:checked {
            background: #4a90d9;
            color: white;
            border-color: #357abd;
        }
        #workflowIndicator QPushButton:hover:!checked {
            background: #e9ecef;
        }
        #workflowIndicator QPushButton[completed="true"] {
            background: #d4edda;
            border-color: #28a745;
        }
    )");
    
    return widget;
}

void CoverageGUI::updateWorkflowSteps() {
    // Determine current progress based on data state
    int new_step = 0;
    
    if (pcd_points_ && !pcd_points_->empty()) {
        new_step = 1;  // Loaded
    }
    if (filtered_points_ && !filtered_points_->empty() && 
        (filtered_points_->size() != (pcd_points_ ? pcd_points_->size() : 0))) {
        new_step = 2;  // Filtered
    }
    if (!polygon_.empty()) {
        new_step = 3;  // Hull computed
    }
    if (!path_.empty() || !custom_waypoints_.empty()) {
        new_step = 4;  // Path planned
    }
    if (waypoints_published_) {
        new_step = 5;  // Exported/Published
    }
    
    current_workflow_step_ = new_step;
    
    // Update button states
    for (int i = 0; i < static_cast<int>(workflow_btns_.size()); ++i) {
        bool completed = (i < current_workflow_step_);
        bool current = (i == current_workflow_step_ - 1) || (i == 0 && current_workflow_step_ == 0);
        
        workflow_btns_[i]->setChecked(current);
        workflow_btns_[i]->setProperty("completed", completed);
        workflow_btns_[i]->style()->unpolish(workflow_btns_[i]);
        workflow_btns_[i]->style()->polish(workflow_btns_[i]);
    }
    
    // Update quick status
    if (lbl_quick_status_) {
        QStringList status_texts = {
            "Ready to load point cloud",
            "Point cloud loaded - apply filters",
            "Filtered - compute hull",
            "Hull ready - generate path",
            "Path ready - publish to robot",
            "Published! Ready to start"
        };
        if (current_workflow_step_ < status_texts.size()) {
            lbl_quick_status_->setText(status_texts[current_workflow_step_]);
        }
    }
}

void CoverageGUI::onWorkflowStepClicked(int step) {
    // Navigate to the corresponding toolbox section
    if (toolbox_ && step < toolbox_->count()) {
        toolbox_->setCurrentIndex(step);
    }
    
    // Update visual state
    for (int i = 0; i < static_cast<int>(workflow_btns_.size()); ++i) {
        workflow_btns_[i]->setChecked(i == step);
    }
}

// =============================================================================
// Layer Visibility Panel
// =============================================================================

QWidget* CoverageGUI::buildLayerPanel() {
    QWidget* box = new QWidget();
    box->setFixedWidth(180);  // Slightly wider for 2 columns
    box->setObjectName("layerPanel");
    
    QVBoxLayout* mainLayout = new QVBoxLayout(box);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(2);
    
    // Grid layout for 2-column checkboxes
    QGridLayout* grid = new QGridLayout();
    grid->setSpacing(2);
    grid->setContentsMargins(0, 0, 0, 0);
    
    int row = 0;
    auto addLayerCheckbox = [&](int r, int c, const QString& label, QCheckBox*& checkbox, bool defaultChecked = true) {
        checkbox = new QCheckBox(label);
        checkbox->setChecked(defaultChecked);
        checkbox->setStyleSheet("font-size: 10px;");
        connect(checkbox, &QCheckBox::toggled, this, &CoverageGUI::updateLayerVisibility);
        grid->addWidget(checkbox, r, c);
    };
    
    // Row 0
    addLayerCheckbox(0, 0, "☁️ Points", chk_layer_points_, true);
    addLayerCheckbox(0, 1, "➜ Path", chk_layer_path_, true);
    
    // Row 1
    addLayerCheckbox(1, 0, "⬡ Polygon", chk_layer_polygon_, true);
    addLayerCheckbox(1, 1, "📍 Trail", chk_layer_trail_, true);
    
    // Row 2
    addLayerCheckbox(2, 0, "🟩 ROI", chk_layer_roi_, true);
    addLayerCheckbox(2, 1, "🤖 Robot", chk_layer_robot_, true);
    
    // Row 3
    addLayerCheckbox(3, 0, "⚠️ Obstacles", chk_layer_obstacles_, true);
    addLayerCheckbox(3, 1, "🛰 Segments", chk_layer_scan_segments_, true);
    
    // Row 4 - Swaths spans or single
    addLayerCheckbox(4, 0, "═ Swaths", chk_layer_swaths_, true);
    
    mainLayout->addLayout(grid);
    
    // All on/off buttons
    QHBoxLayout* btn_layout = new QHBoxLayout();
    btn_layout->setSpacing(4);
    QPushButton* btn_all_on = new QPushButton("All");
    QPushButton* btn_all_off = new QPushButton("None");
    btn_all_on->setFixedHeight(22);
    btn_all_off->setFixedHeight(22);
    btn_all_on->setStyleSheet("font-size: 10px;");
    btn_all_off->setStyleSheet("font-size: 10px;");
    
    connect(btn_all_on, &QPushButton::clicked, this, [this]() {
        chk_layer_points_->setChecked(true);
        chk_layer_polygon_->setChecked(true);
        chk_layer_roi_->setChecked(true);
        chk_layer_obstacles_->setChecked(true);
        chk_layer_swaths_->setChecked(true);
        chk_layer_path_->setChecked(true);
        chk_layer_trail_->setChecked(true);
        chk_layer_robot_->setChecked(true);
        chk_layer_scan_segments_->setChecked(true);
    });
    
    connect(btn_all_off, &QPushButton::clicked, this, [this]() {
        chk_layer_points_->setChecked(false);
        chk_layer_polygon_->setChecked(false);
        chk_layer_roi_->setChecked(false);
        chk_layer_obstacles_->setChecked(false);
        chk_layer_swaths_->setChecked(false);
        chk_layer_path_->setChecked(false);
        chk_layer_trail_->setChecked(false);
        chk_layer_robot_->setChecked(false);
        chk_layer_scan_segments_->setChecked(false);
    });
    
    btn_layout->addWidget(btn_all_on);
    btn_layout->addWidget(btn_all_off);
    mainLayout->addLayout(btn_layout);
    
    return box;
}

QGroupBox* CoverageGUI::buildScanPlannerPanel() {
    QGroupBox* box = new QGroupBox("Scan planner");
    QVBoxLayout* v = new QVBoxLayout(box);

    QHBoxLayout* row = new QHBoxLayout();
    row->addWidget(new QLabel("Distance per scan (m):"));
    spin_scan_len_ = new QDoubleSpinBox();
    spin_scan_len_->setRange(10.0, 10000.0);
    spin_scan_len_->setSingleStep(10.0);
    spin_scan_len_->setValue(500.0);
    row->addWidget(spin_scan_len_);
    v->addLayout(row);

    btn_make_segments_ = new QPushButton("⚙️ Split path");
    btn_make_segments_->setObjectName("btn_quick_generate");
    btn_make_segments_->setMinimumHeight(34);

    btn_publish_segments_ = new QPushButton("⬆ Publish selected");
    btn_publish_segments_->setObjectName("btn_publish");
    btn_publish_segments_->setMinimumHeight(34);

    btn_start_segments_ = new QPushButton("▶️ Start selected");
    btn_start_segments_->setObjectName("btn_navigation");
    btn_start_segments_->setMinimumHeight(34);

    v->addWidget(btn_make_segments_);
    v->addWidget(btn_publish_segments_);
    v->addWidget(btn_start_segments_);

    list_scan_segments_ = new QListWidget();
    list_scan_segments_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    v->addWidget(list_scan_segments_, 1);

    lbl_scan_progress_ = new QLabel("Segments: none");
    lbl_scan_progress_->setStyleSheet("color: #666; font-size: 10px;");
    v->addWidget(lbl_scan_progress_);

    return box;
}

void CoverageGUI::updateLayerVisibility() {
    // This function will be called when layer checkboxes change
    // The actual visibility is handled in refreshPlot()
    refreshPlot();
}

// =============================================================================
// Quick Actions Bar
// =============================================================================

QWidget* CoverageGUI::buildQuickActionsBar() {
    QWidget* bar = new QWidget();
    bar->setObjectName("quickActionsBar");
    bar->setFixedHeight(60);
    
    QHBoxLayout* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(15, 8, 15, 8);
    layout->setSpacing(15);
    
    // Status label on left
    lbl_quick_status_ = new QLabel("Ready");
    lbl_quick_status_->setObjectName("quickStatus");
    lbl_quick_status_->setMinimumWidth(200);
    layout->addWidget(lbl_quick_status_);
    
    layout->addStretch();
    
    // Main action buttons
    btn_quick_generate_ = new QPushButton("⚙️ Generate Path");
    btn_quick_generate_->setObjectName("btn_quick_generate");
    btn_quick_generate_->setMinimumWidth(140);
    btn_quick_generate_->setMinimumHeight(36);
    connect(btn_quick_generate_, &QPushButton::clicked, this, &CoverageGUI::generatePath);
    layout->addWidget(btn_quick_generate_);
    
    btn_quick_publish_ = new QPushButton("⬆ Publish");
    btn_quick_publish_->setObjectName("btn_quick_publish");
    btn_quick_publish_->setMinimumWidth(100);
    btn_quick_publish_->setMinimumHeight(36);
    connect(btn_quick_publish_, &QPushButton::clicked, this, &CoverageGUI::publishWaypoints);
    layout->addWidget(btn_quick_publish_);
    
    btn_quick_start_ = new QPushButton("▶️ Start");
    btn_quick_start_->setObjectName("btn_quick_start");
    btn_quick_start_->setMinimumWidth(90);
    btn_quick_start_->setMinimumHeight(36);
    btn_quick_start_->setEnabled(false);
    connect(btn_quick_start_, &QPushButton::clicked, this, &CoverageGUI::startNavigation);
    layout->addWidget(btn_quick_start_);

    btn_quick_home_ = new QPushButton("🏠 Home");
    btn_quick_home_->setObjectName("btn_quick_home");
    btn_quick_home_->setMinimumWidth(90);
    btn_quick_home_->setMinimumHeight(36);
    btn_quick_home_->setToolTip("Plan obstacle-avoiding path to origin (0,0)");
    connect(btn_quick_home_, &QPushButton::clicked, this, &CoverageGUI::planHomePath);
    layout->addWidget(btn_quick_home_);
    
    // Teleop button
    QPushButton* btn_teleop = new QPushButton("🎮 Teleop");
    btn_teleop->setObjectName("btn_teleop");
    btn_teleop->setMinimumWidth(90);
    btn_teleop->setMinimumHeight(36);
    btn_teleop->setToolTip("Open robot teleop controls (keyboard + buttons)");
    connect(btn_teleop, &QPushButton::clicked, this, &CoverageGUI::toggleTeleopWidget);
    layout->addWidget(btn_teleop);
    
    // Style the bar
    bar->setStyleSheet(R"(
        #quickActionsBar {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 #f8f9fa, stop:1 #e9ecef);
            border-top: 1px solid #dee2e6;
        }
        #quickStatus {
            font-size: 12px;
            color: #495057;
            font-weight: bold;
        }
        #btn_quick_generate {
            background-color: #6c757d;
            color: white;
            border: none;
            border-radius: 6px;
            font-weight: bold;
        }
        #btn_quick_generate:hover {
            background-color: #5a6268;
        }
        #btn_quick_publish {
            background-color: #28a745;
            color: white;
            border: none;
            border-radius: 6px;
            font-weight: bold;
        }
        #btn_quick_publish:hover {
            background-color: #218838;
        }
        #btn_quick_start {
            background-color: #007bff;
            color: white;
            border: none;
            border-radius: 6px;
            font-weight: bold;
        }
        #btn_quick_start:hover {
            background-color: #0069d9;
        }
        #btn_quick_start:disabled {
            background-color: #6c757d;
        }
        #btn_quick_home {
            background-color: #17a2b8;
            color: white;
            border: none;
            border-radius: 6px;
            font-weight: bold;
        }
        #btn_quick_home:hover {
            background-color: #138496;
        }
    )");
    
    return bar;
}

void CoverageGUI::setStatus(const QString& text, int timeout_ms) {
    status_bar_->showMessage(text, timeout_ms);
}

void CoverageGUI::beginProgressOperation(const QString& text, bool cancelable, bool indeterminate) {
    progress_cancel_requested_.store(false);
    progress_operation_active_ = true;
    progress_operation_cancelable_ = cancelable;
    progress_active_process_ = nullptr;
    progress_last_percent_ = -2;
    progress_last_text_.clear();
    progress_last_ui_pump_ = std::chrono::steady_clock::now();

    if (progress_bar_) {
        if (indeterminate) {
            progress_bar_->setRange(0, 0);
        } else {
            progress_bar_->setRange(0, 100);
            progress_bar_->setValue(0);
        }
        progress_bar_->setVisible(true);
    }
    if (btn_progress_cancel_) {
        btn_progress_cancel_->setVisible(cancelable);
        btn_progress_cancel_->setEnabled(cancelable);
    }
    if (cancelable && centralWidget()) {
        centralWidget()->setEnabled(false);
    }
    if (!text.isEmpty()) {
        setStatus(text);
        progress_last_text_ = text;
    }
    if (cancelable) {
        pumpProgressUiIfNeeded(true);
    }
}

bool CoverageGUI::endProgressOperation() {
    const bool was_cancelled = progress_cancel_requested_.exchange(false);
    progress_operation_active_ = false;
    progress_operation_cancelable_ = false;
    progress_active_process_ = nullptr;
    progress_last_percent_ = -2;
    progress_last_text_.clear();

    if (progress_bar_) {
        progress_bar_->setVisible(false);
        progress_bar_->setRange(0, 100);
        progress_bar_->setValue(0);
    }
    if (btn_progress_cancel_) {
        btn_progress_cancel_->setVisible(false);
        btn_progress_cancel_->setEnabled(false);
    }
    if (centralWidget()) {
        centralWidget()->setEnabled(true);
    }
    return was_cancelled;
}

void CoverageGUI::showProgress(bool show, const QString& text) {
    progress_bar_->setVisible(show);
    if (!show) {
        progress_bar_->setRange(0, 100);
        progress_bar_->setValue(0);
    }
    if (show && !text.isEmpty()) {
        setStatus(text);
    }
}

void CoverageGUI::updateProgress(int percent, const QString& text) {
    if (percent >= 0) {
        if (progress_bar_->minimum() == 0 && progress_bar_->maximum() == 0) {
            progress_bar_->setRange(0, 100);
        }
        const int clamped_percent = std::clamp(percent, 0, 100);
        if (clamped_percent != progress_last_percent_) {
            progress_bar_->setValue(clamped_percent);
            progress_last_percent_ = clamped_percent;
        }
    }
    if (!text.isEmpty() && text != progress_last_text_) {
        setStatus(text);
        progress_last_text_ = text;
    }
    pumpProgressUiIfNeeded();
}

void CoverageGUI::pumpProgressUiIfNeeded(bool force) {
    if (!progress_operation_active_ || !progress_operation_cancelable_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!force && progress_last_ui_pump_.time_since_epoch().count() != 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - progress_last_ui_pump_);
        if (elapsed < std::chrono::milliseconds(33)) {
            return;
        }
    }

    progress_last_ui_pump_ = now;
    QApplication::processEvents(QEventLoop::AllEvents, 1);
}

bool CoverageGUI::isProgressCancelRequested() const {
    return progress_cancel_requested_.load();
}

bool CoverageGUI::isOperationCancelledMessage(const QString& text) const {
    return text.trimmed().compare(kOperationCancelledMessage, Qt::CaseInsensitive) == 0;
}

void CoverageGUI::onCancelProgressRequested() {
    if (!progress_operation_active_ || !progress_operation_cancelable_) {
        return;
    }
    progress_cancel_requested_.store(true);
    if (btn_progress_cancel_) {
        btn_progress_cancel_->setEnabled(false);
    }
    setStatus("Cancelling current operation...");
    if (progress_active_process_ && progress_active_process_->state() != QProcess::NotRunning) {
        progress_active_process_->terminate();
        if (!progress_active_process_->waitForFinished(250)) {
            progress_active_process_->kill();
            progress_active_process_->waitForFinished(250);
        }
    }
}

CoverageGUI::ProcessWaitResult CoverageGUI::waitForProcessFinishedCancelable(
    QProcess* process, int timeout_ms) {
    if (!process) {
        return ProcessWaitResult::TimedOut;
    }

    progress_active_process_ = process;
    QElapsedTimer timer;
    timer.start();
    while (process->state() != QProcess::NotRunning) {
        if (timeout_ms >= 0 && timer.elapsed() >= timeout_ms) {
            process->terminate();
            if (!process->waitForFinished(500)) {
                process->kill();
                process->waitForFinished(500);
            }
            progress_active_process_ = nullptr;
            return ProcessWaitResult::TimedOut;
        }
        if (isProgressCancelRequested()) {
            process->terminate();
            if (!process->waitForFinished(500)) {
                process->kill();
                process->waitForFinished(500);
            }
            progress_active_process_ = nullptr;
            return ProcessWaitResult::Cancelled;
        }
        process->waitForFinished(100);
        pumpProgressUiIfNeeded();
    }
    progress_active_process_ = nullptr;
    return ProcessWaitResult::Finished;
}

CoverageConfig CoverageGUI::currentConfig() const {
    CoverageConfig cfg;
    cfg.swath_width = spin_swath_->value();
    cfg.headland_width = spin_headland_->value();
    cfg.turn_radius = spin_turn_->value();
    cfg.auto_align = chk_auto_align_->isChecked();
    cfg.align_mode = radio_perp_->isChecked() ? "perp" : "long";
    cfg.route_pattern = combo_route_pattern_->currentData().toString().toStdString();
    cfg.path_planner = combo_path_planner_->currentData().toString().toStdString();
    cfg.use_decomposition = chk_decomposition_->isChecked();
    cfg.decomposition_type = combo_decomp_type_->currentData().toString().toStdString();
    cfg.use_axial_turns = chk_axial_turns_->isChecked();
    cfg.waypoint_spacing = spin_waypoint_spacing_ ? spin_waypoint_spacing_->value() : 0.0;
    return cfg;
}

void CoverageGUI::refreshPlot() {
    // Apply layer visibility settings
    bool show_points = !chk_layer_points_ || chk_layer_points_->isChecked();
    bool show_polygon = !chk_layer_polygon_ || chk_layer_polygon_->isChecked();
    bool show_roi = !chk_layer_roi_ || chk_layer_roi_->isChecked();
    bool show_obstacles = !chk_layer_obstacles_ || chk_layer_obstacles_->isChecked();
    bool show_swaths = !chk_layer_swaths_ || chk_layer_swaths_->isChecked();
    bool show_path = !chk_layer_path_ || chk_layer_path_->isChecked();
    bool show_trail = !chk_layer_trail_ || chk_layer_trail_->isChecked();
    bool show_robot = !chk_layer_robot_ || chk_layer_robot_->isChecked();
    
    // Also check the robot tracking checkbox
    if (chk_show_robot_ && !chk_show_robot_->isChecked()) {
        show_robot = false;
        show_trail = false;
    }
    
    // Set data based on visibility
    plot_->setPoints(show_points ? xy_2d_ : std::vector<Point2D>());
    plot_->setPolygon(show_polygon ? polygon_ : Polygon2D());
    plot_->setROI(show_roi ? roi_polygon_ : Polygon2D());
    plot_->setObstacles(show_obstacles ? obstacles_ : std::vector<Obstacle2D>());
    plot_->setSwaths(show_swaths ? swaths_ : SwathList());
    
    // Set route and path
    plot_->setRoute(show_path ? route_ : PathStateList());
    plot_->setPath(show_path ? path_ : PathStateList());
    plot_->setPathConnectorPrefixCount(show_path ? path_connector_prefix_count_ : 0);
    plot_->setCustomPath(custom_waypoints_, custom_waypoints_visited_);
    plot_->setShowCustomPath(isCustomModeActive() && !custom_waypoints_.empty() && show_path);

    // Scan segments overlay
    bool show_segments = !chk_layer_scan_segments_ || chk_layer_scan_segments_->isChecked();
    std::vector<PathStateList> seg_paths;
    std::vector<QString> seg_labels;
    std::vector<double> seg_lengths;
    std::vector<int> seg_turns;
    if (show_segments) {
        for (const auto& s : scan_segments_) {
            seg_paths.push_back(s.path);
            seg_labels.push_back(s.name);
            seg_lengths.push_back(s.length_m);
            seg_turns.push_back(s.turns);
        }
    }
    plot_->setScanSegments(seg_paths, seg_labels, seg_lengths, seg_turns,
                           show_segments && !seg_paths.empty());
    plot_->setActiveScanSegment(active_scan_segment_idx_);
    
    std::optional<PathState> pose_copy;
    std::vector<Point2D> trail_copy;
    std::chrono::steady_clock::time_point last_update_copy{};
    {
        std::lock_guard<std::mutex> lock(robot_pose_mutex_);
        pose_copy = robot_pose_state_;
        trail_copy = robot_trail_;
        last_update_copy = last_robot_update_;
    }
    
    if (!show_robot) {
        pose_copy.reset();
    }
    if (!show_trail) {
        trail_copy.clear();
    }
    
    plot_->setRobotPose(pose_copy);
    plot_->setRobotTrail(trail_copy);
    
    if (fit_view_pending_) {
        plot_->resetView();
        fit_view_pending_ = false;
    } else {
        plot_->update();
    }
    
    bool pose_fresh = false;
    if (pose_copy.has_value() && last_update_copy.time_since_epoch().count() > 0) {
        auto age = std::chrono::steady_clock::now() - last_update_copy;
        pose_fresh = age < std::chrono::seconds(2);
    }
    updateRobotStatusLabel(pose_fresh);
}

Polygon2D CoverageGUI::effectivePolygon() const {
    if (!roi_polygon_.empty()) {
        return roi_polygon_;  // TODO: Implement proper intersection
    }
    return polygon_;
}

void CoverageGUI::updateDownsampleUI(const QString& method) {
    QString m = method.toLower();
    group_random_->setVisible(m == "random");
    group_voxel_->setVisible(m == "voxel");
    group_stat_->setVisible(m == "statistical");
}

// Slot implementations

void CoverageGUI::loadPointCloud() {
    QString path = QFileDialog::getOpenFileName(
        this, "Select point cloud", "", "Point Clouds (*.pcd *.ply *.xyz)");
    
    if (path.isEmpty()) return;
    
    // Use async loading for better UI responsiveness
    loadPointCloudAsync(path);
}

void CoverageGUI::clearRobotTrail() {
    {
        std::lock_guard<std::mutex> lock(robot_pose_mutex_);
        robot_trail_.clear();
        robot_trail_states_.clear();
        driven_path_snapshot_.clear();
    }
    refreshPlot();
}

void CoverageGUI::loadTrailFromRosbag() {
    QString bag_dir = QFileDialog::getExistingDirectory(
        this, "Select rosbag2 folder", QDir::homePath());
    if (bag_dir.isEmpty()) {
        return;
    }

    QString topic = robot_odom_topic_.trimmed();
    if (topic.isEmpty()) {
        topic = "/Odometry_tilt_corrected_diff";
    }

    const QString temp_csv = "/tmp/bdr_rosbag_trail.csv";
    const QString py_code = R"PY(
import csv
import math
import os
import glob
import shutil
import subprocess
import sys
import tempfile

def eprint(*args):
    print(*args, file=sys.stderr)

try:
    from rosbag2_py import SequentialReader, StorageOptions, ConverterOptions
    from rclpy.serialization import deserialize_message
    from rosidl_runtime_py.utilities import get_message
except Exception as ex:
    eprint(f"Missing ROS Python modules: {ex}")
    sys.exit(10)

bag = sys.argv[1]
topic = sys.argv[2]
out_csv = sys.argv[3]

def has_zstd_segments(bag_dir):
    if not os.path.isdir(bag_dir):
        return False
    try:
        names = os.listdir(bag_dir)
    except Exception:
        return False
    for n in names:
        if n.endswith(".mcap.zstd") or n.endswith(".db3.zstd"):
            return True
    return False

def patch_metadata_for_decompressed_files(metadata_path):
    if not os.path.exists(metadata_path):
        return
    with open(metadata_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    out = []
    in_rel_paths = False
    rel_indent = None
    for ln in lines:
        stripped = ln.lstrip()
        indent = len(ln) - len(stripped)

        if stripped.startswith("compression_mode:"):
            out.append("  compression_mode: NONE\n")
            in_rel_paths = False
            continue
        if stripped.startswith("compression_format:"):
            out.append("  compression_format: ''\n")
            in_rel_paths = False
            continue
        if stripped.startswith("relative_file_paths:"):
            out.append(ln)
            in_rel_paths = True
            rel_indent = indent
            continue

        if in_rel_paths:
            if indent <= rel_indent and stripped and not stripped.startswith("-"):
                in_rel_paths = False
            elif stripped.startswith("- "):
                # Convert e.g. rosbag_0.mcap.zstd -> rosbag_0.mcap
                entry = stripped[2:].strip()
                if entry.endswith(".zstd"):
                    entry = entry[:-5]
                out.append((" " * indent) + "- " + entry + "\n")
                continue

        out.append(ln)

    with open(metadata_path, "w", encoding="utf-8") as f:
        f.writelines(out)

def decompress_with_zstd(src_path, dst_path):
    # Try system zstd first
    cmd = ["zstd", "-d", "-f", src_path, "-o", dst_path]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode == 0 and os.path.exists(dst_path):
        return

    # Fallback to python zstandard module if available
    try:
        import zstandard as zstd
        with open(src_path, "rb") as fin, open(dst_path, "wb") as fout:
            dctx = zstd.ZstdDecompressor()
            dctx.copy_stream(fin, fout)
        return
    except Exception as ex:
        err = (proc.stderr or proc.stdout or "").strip()
        raise RuntimeError(
            f"Failed to decompress '{os.path.basename(src_path)}'. "
            f"Install 'zstd' CLI or python 'zstandard'. "
            f"zstd error: {err}; python error: {ex}"
        )

def prepare_bag_uri(uri):
    # rosbag2_py on this distro cannot directly open FILE-compressed segments.
    # Make a temporary decompressed copy of *.zstd files and patch metadata.
    if not has_zstd_segments(uri):
        return uri, None

    tmp_root = tempfile.mkdtemp(prefix="bdr_rosbag_decompress_")
    tmp_bag = os.path.join(tmp_root, os.path.basename(os.path.normpath(uri)))
    shutil.copytree(uri, tmp_bag)

    zstd_files = glob.glob(os.path.join(tmp_bag, "*.zstd"))
    if not zstd_files:
        return tmp_bag, tmp_root

    for src in zstd_files:
        dst = src[:-5]  # remove ".zstd"
        decompress_with_zstd(src, dst)

    patch_metadata_for_decompressed_files(os.path.join(tmp_bag, "metadata.yaml"))
    return tmp_bag, tmp_root

def open_reader(storage_id):
    reader = SequentialReader()
    reader.open(StorageOptions(uri=bag, storage_id=storage_id), ConverterOptions('', ''))
    return reader

tmp_cleanup = None
try:
    bag, tmp_cleanup = prepare_bag_uri(bag)
except Exception as ex:
    eprint(str(ex))
    sys.exit(14)

reader = None
last_err = None
for sid in ("sqlite3", "mcap", ""):
    try:
        reader = open_reader(sid)
        break
    except Exception as ex:
        last_err = ex

if reader is None:
    eprint(f"Failed to open rosbag: {last_err}")
    sys.exit(11)

topic_types = {t.name: t.type for t in reader.get_all_topics_and_types()}
if topic not in topic_types:
    eprint(f"Topic not found in bag: {topic}")
    sys.exit(12)

msg_type = get_message(topic_types[topic])
rows = []
while reader.has_next():
    tname, data, stamp = reader.read_next()
    if tname != topic:
        continue
    msg = deserialize_message(data, msg_type)
    x = float(msg.pose.pose.position.x)
    y = float(msg.pose.pose.position.y)
    q = msg.pose.pose.orientation
    siny = 2.0 * (q.w * q.z + q.x * q.y)
    cosy = 1.0 - 2.0 * (q.y * q.y + q.z * q.z)
    yaw = math.atan2(siny, cosy)
    rows.append((x, y, yaw, int(stamp)))

if not rows:
    eprint(f"No messages found on topic: {topic}")
    sys.exit(13)

with open(out_csv, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["x", "y", "yaw", "stamp"])
    w.writerows(rows)

if tmp_cleanup:
    try:
        shutil.rmtree(tmp_cleanup, ignore_errors=True)
    except Exception:
        pass
)PY";

    beginProgressOperation("Loading trail from rosbag...", true);
    updateProgress(10, "Loading trail from rosbag...");
    QProcess proc;
    proc.start("python3", QStringList() << "-c" << py_code << bag_dir << topic << temp_csv);
    const ProcessWaitResult bag_wait = waitForProcessFinishedCancelable(&proc, 600000);
    if (bag_wait == ProcessWaitResult::Cancelled) {
        QFile::remove(temp_csv);
        endProgressOperation();
        setStatus("Trail import cancelled", 5000);
        return;
    }
    if (bag_wait == ProcessWaitResult::TimedOut) {
        endProgressOperation();
        QMessageBox::warning(this, "Trail Import Failed", "Timed out while reading/decompressing rosbag.");
        return;
    }
    if (proc.exitCode() != 0) {
        endProgressOperation();
        QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        if (err.isEmpty()) {
            err = "Unknown error reading rosbag.";
        }
        QMessageBox::warning(this, "Trail Import Failed", err);
        return;
    }

    updateProgress(55, "Parsing extracted trail...");
    std::ifstream in(temp_csv.toStdString());
    if (!in.is_open()) {
        endProgressOperation();
        QMessageBox::warning(this, "Trail Import Failed", "Could not read extracted trail CSV.");
        return;
    }

    std::string line;
    std::getline(in, line);  // header
    std::vector<PathState> imported_states;
    imported_states.reserve(5000);
    const qint64 csv_size = QFileInfo(temp_csv).size();

    while (std::getline(in, line)) {
        if ((imported_states.size() & 0x0FFFu) == 0u) {
            const std::streampos pos = in.tellg();
            if (csv_size > 0 && pos >= 0) {
                const double ratio = std::clamp(static_cast<double>(pos) /
                                                    static_cast<double>(csv_size),
                                                0.0, 1.0);
                updateProgress(55 + static_cast<int>(40.0 * ratio), "Parsing extracted trail...");
            } else {
                updateProgress(-1, "Parsing extracted trail...");
            }
            if (isProgressCancelRequested()) {
                QFile::remove(temp_csv);
                endProgressOperation();
                setStatus("Trail import cancelled", 5000);
                return;
            }
        }
        if (line.empty()) {
            continue;
        }
        std::stringstream ss(line);
        std::string sx, sy, syaw, sstamp;
        if (!std::getline(ss, sx, ',')) continue;
        if (!std::getline(ss, sy, ',')) continue;
        if (!std::getline(ss, syaw, ',')) continue;
        std::getline(ss, sstamp, ',');
        try {
            double x = std::stod(sx);
            double y = std::stod(sy);
            double yaw = std::stod(syaw);

            Eigen::Vector4f p(static_cast<float>(x), static_cast<float>(y), 0.0f, 1.0f);
            Eigen::Vector4f pt = alignment_transform_total_ * p;
            const double rot_yaw = std::atan2(
                static_cast<double>(alignment_transform_total_(1, 0)),
                static_cast<double>(alignment_transform_total_(0, 0)));
            const double yaw_t = yaw + rot_yaw;

            PathState st;
            st.point = Point2D(pt.x(), pt.y());
            st.heading = yaw_t;
            st.vx = std::cos(yaw_t);
            st.vy = std::sin(yaw_t);
            imported_states.push_back(st);
        } catch (...) {
            continue;
        }
    }

    if (imported_states.empty()) {
        endProgressOperation();
        QMessageBox::warning(this, "Trail Import Failed", "No valid trail points were parsed.");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(robot_pose_mutex_);
        robot_trail_states_ = imported_states;
        robot_trail_.clear();
        robot_trail_.reserve(imported_states.size());
        for (const auto& st : imported_states) {
            robot_trail_.push_back(st.point);
        }
        driven_path_snapshot_ = robot_trail_states_;
    }

    updateProgress(100, "Trail import complete");
    endProgressOperation();
    refreshPlot();
    setStatus(QString("Loaded %1 trail points from rosbag (%2)")
                  .arg(imported_states.size())
                  .arg(QFileInfo(bag_dir).fileName()),
              6000);
}

void CoverageGUI::fetchLatestMapFromRobot() {
    const bool using_registry = robot_registry_.isLoaded() && !robot_registry_.isEmpty();
    const QString display_robot = using_registry && !active_robot_id_.isEmpty()
                                      ? active_robot_id_
                                      : robot_host_;
    const QString ssh_opts = pinned_known_hosts_file_.isEmpty()
                                 ? "-o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=no"
                                 : QString("-o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=yes "
                                           "-o UserKnownHostsFile=%1 -o GlobalKnownHostsFile=/dev/null")
                                       .arg(pinned_known_hosts_file_);
    beginProgressOperation(QString("Connecting to %1...").arg(display_robot), true);
    updateProgress(5, QString("Connecting to %1...").arg(display_robot));
    
    // Build SSH command to find the latest .pcd file on robot
    QString find_cmd = QString(
        "ssh %1 %2@%3 "
        "\"find %4 -name '*.pcd' -type f -printf '%T@ %p\\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-\""
    ).arg(ssh_opts, robot_user_, robot_host_, robot_data_path_);
    
    QProcess find_process;
    find_process.start("bash", QStringList() << "-c" << find_cmd);

    const ProcessWaitResult find_wait = waitForProcessFinishedCancelable(&find_process, 10000);
    if (find_wait == ProcessWaitResult::Cancelled) {
        endProgressOperation();
        setStatus("Map fetch cancelled", 5000);
        return;
    }
    if (find_wait == ProcessWaitResult::TimedOut) {
        endProgressOperation();
        const QString details = using_registry
                                    ? QString("Could not connect to robot.\nCheck if:\n"
                                              "• Robot is powered on\n"
                                              "• Microhard link is connected\n"
                                              "• SSH keys are configured for user '%1'\n"
                                              "• Selected robot: %2")
                                          .arg(robot_user_, display_robot)
                                    : QString("Could not connect to robot.\nCheck if:\n"
                                              "• Robot is powered on\n"
                                              "• Microhard is connected (%1)\n"
                                              "• SSH keys are configured for %2@%1")
                                          .arg(robot_host_, robot_user_);
        QMessageBox::warning(this, "Connection Failed", details);
        return;
    }
    
    if (find_process.exitCode() != 0) {
        endProgressOperation();
        QString error = QString::fromUtf8(find_process.readAllStandardError());
        QMessageBox::warning(this, "SSH Error", "SSH command failed:\n" + error);
        return;
    }
    
    QString remote_path = QString::fromUtf8(find_process.readAllStandardOutput()).trimmed();
    
    if (remote_path.isEmpty()) {
        endProgressOperation();
        QMessageBox::warning(this, "No Maps Found", 
            "No .pcd map files found on robot at " + robot_data_path_);
        return;
    }
    
    // Create local folder structure: ~/Roofus_maps/December_01_2025/
    QDate today = QDate::currentDate();
    QString day_folder = today.toString("MMMM_dd_yyyy");  // e.g., "December_01_2025"
    QString local_dir = local_map_base_ + "/" + day_folder;
    QDir().mkpath(local_dir);
    
    // Extract filename and create local path
    QFileInfo remote_info(remote_path);
    QString local_path = local_dir + "/" + remote_info.fileName();
    updateProgress(30, "Latest map found: " + remote_info.fileName());
    
    // Check if file already exists locally
    if (QFile::exists(local_path)) {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this, "File Exists", 
            QString("Map already exists locally:\n%1\n\nOverwrite?").arg(local_path),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            endProgressOperation();
            // Offer to load existing file
            reply = QMessageBox::question(this, "Load Existing?", 
                "Load the existing local map instead?",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (reply == QMessageBox::Yes) {
                loadPointCloudAsync(local_path);
            }
            return;
        }
    }

    const QString download_path = local_path + ".part";
    QFile::remove(download_path);
    updateProgress(60, "Downloading: " + remote_info.fileName());
    
    // SCP the file to local machine
    QString scp_cmd = QString(
        "scp %1 %2@%3:\"%4\" \"%5\""
    ).arg(
        pinned_known_hosts_file_.isEmpty()
            ? "-o ConnectTimeout=10 -o BatchMode=yes -o StrictHostKeyChecking=no"
            : QString("-o ConnectTimeout=10 -o BatchMode=yes -o StrictHostKeyChecking=yes "
                      "-o UserKnownHostsFile=%1 -o GlobalKnownHostsFile=/dev/null")
                  .arg(pinned_known_hosts_file_),
        robot_user_, robot_host_, remote_path, download_path);
    
    QProcess scp_process;
    scp_process.start("bash", QStringList() << "-c" << scp_cmd);

    const ProcessWaitResult scp_wait = waitForProcessFinishedCancelable(&scp_process, 180000);
    if (scp_wait == ProcessWaitResult::Cancelled) {
        QFile::remove(download_path);
        endProgressOperation();
        setStatus("Map download cancelled", 5000);
        return;
    }
    if (scp_wait == ProcessWaitResult::TimedOut) {  // 3 min timeout for large files
        QFile::remove(download_path);
        endProgressOperation();
        QMessageBox::warning(this, "Download Failed", 
            "File transfer timed out.\nThe map file may be too large or connection is slow.");
        return;
    }
    
    if (scp_process.exitCode() != 0) {
        QFile::remove(download_path);
        endProgressOperation();
        QString error = QString::fromUtf8(scp_process.readAllStandardError());
        QMessageBox::warning(this, "Download Failed", 
            "Could not download map file:\n" + error);
        return;
    }

    if (QFile::exists(local_path) && !QFile::remove(local_path)) {
        QFile::remove(download_path);
        endProgressOperation();
        QMessageBox::warning(this, "Download Failed",
                             "Downloaded map could not replace the existing local file.");
        return;
    }
    if (!QFile::rename(download_path, local_path)) {
        QFile::remove(download_path);
        endProgressOperation();
        QMessageBox::warning(this, "Download Failed",
                             "Downloaded map could not be finalized locally.");
        return;
    }
    updateProgress(100, "Map download complete");
    endProgressOperation();
    
    // Verify file exists locally
    QFileInfo local_info(local_path);
    if (!local_info.exists() || local_info.size() == 0) {
        QMessageBox::warning(this, "Download Failed", "Map file was not saved correctly.");
        return;
    }
    
    // Show success and offer to load
    QString msg = QString(
        "✅ Map downloaded successfully!\n\n"
        "File: %1\n"
        "Size: %2 MB\n"
        "Saved to: %3\n\n"
        "Load this map now?"
    ).arg(local_info.fileName())
     .arg(local_info.size() / (1024.0 * 1024.0), 0, 'f', 2)
     .arg(local_dir);
    
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "Download Complete", msg,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    
    if (reply == QMessageBox::Yes) {
        loadPointCloudAsync(local_path);
    }
    
    setStatus("Map saved: " + local_path);
}

bool CoverageGUI::fetchLatestMapFileForAlignment(QString* localPathOut, QString* errorOut) {
    const bool using_registry = robot_registry_.isLoaded() && !robot_registry_.isEmpty();
    const QString display_robot = using_registry && !active_robot_id_.isEmpty()
                                      ? active_robot_id_
                                      : robot_host_;
    const QString ssh_opts = pinned_known_hosts_file_.isEmpty()
                                 ? "-o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=no"
                                 : QString("-o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=yes "
                                           "-o UserKnownHostsFile=%1 -o GlobalKnownHostsFile=/dev/null")
                                       .arg(pinned_known_hosts_file_);
    updateProgress(10, QString("Alignment: finding latest map on %1...").arg(display_robot));

    QString find_cmd = QString(
        "ssh %1 %2@%3 "
        "\"find %4 -name '*.pcd' -type f -printf '%T@ %p\\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-\""
    ).arg(ssh_opts, robot_user_, robot_host_, robot_data_path_);

    QProcess find_process;
    find_process.start("bash", QStringList() << "-c" << find_cmd);
    const ProcessWaitResult find_wait = waitForProcessFinishedCancelable(&find_process, 10000);
    if (find_wait == ProcessWaitResult::Cancelled) {
        if (errorOut) {
            *errorOut = kOperationCancelledMessage;
        }
        return false;
    }
    if (find_wait == ProcessWaitResult::TimedOut || find_process.exitCode() != 0) {
        if (errorOut) {
            *errorOut = QString("Could not query latest robot map via SSH.\n%1")
                            .arg(QString::fromUtf8(find_process.readAllStandardError()).trimmed());
        }
        return false;
    }

    QString remote_path = QString::fromUtf8(find_process.readAllStandardOutput()).trimmed();
    if (remote_path.isEmpty()) {
        if (errorOut) {
            *errorOut = "No .pcd map files found on robot under " + robot_data_path_;
        }
        return false;
    }

    QDate today = QDate::currentDate();
    QString day_folder = today.toString("MMMM_dd_yyyy");
    QString local_dir = local_map_base_ + "/" + day_folder;
    QDir().mkpath(local_dir);

    QFileInfo remote_info(remote_path);
    QString local_path = local_dir + "/" + remote_info.fileName();
    bool use_existing = QFileInfo::exists(local_path) && QFileInfo(local_path).size() > 0;

    if (!use_existing) {
        const QString download_path = local_path + ".align.part";
        QFile::remove(download_path);
        updateProgress(25, "Alignment: downloading " + remote_info.fileName());
        QString scp_cmd = QString(
            "scp %1 %2@%3:\"%4\" \"%5\""
        ).arg(
            pinned_known_hosts_file_.isEmpty()
                ? "-o ConnectTimeout=10 -o BatchMode=yes -o StrictHostKeyChecking=no"
                : QString("-o ConnectTimeout=10 -o BatchMode=yes -o StrictHostKeyChecking=yes "
                          "-o UserKnownHostsFile=%1 -o GlobalKnownHostsFile=/dev/null")
                      .arg(pinned_known_hosts_file_),
            robot_user_, robot_host_, remote_path, download_path);

        QProcess scp_process;
        scp_process.start("bash", QStringList() << "-c" << scp_cmd);
        const ProcessWaitResult scp_wait = waitForProcessFinishedCancelable(&scp_process, 180000);
        if (scp_wait == ProcessWaitResult::Cancelled) {
            QFile::remove(download_path);
            if (errorOut) {
                *errorOut = kOperationCancelledMessage;
            }
            return false;
        }
        if (scp_wait == ProcessWaitResult::TimedOut || scp_process.exitCode() != 0) {
            QFile::remove(download_path);
            if (errorOut) {
                *errorOut = QString("Failed to download latest map.\n%1")
                                .arg(QString::fromUtf8(scp_process.readAllStandardError()).trimmed());
            }
            return false;
        }
        if (QFile::exists(local_path) && !QFile::remove(local_path)) {
            QFile::remove(download_path);
            if (errorOut) {
                *errorOut = "Downloaded map could not replace the existing cached file.";
            }
            return false;
        }
        if (!QFile::rename(download_path, local_path)) {
            QFile::remove(download_path);
            if (errorOut) {
                *errorOut = "Downloaded map could not be finalized locally.";
            }
            return false;
        }
    } else {
        updateProgress(25, "Alignment: using cached latest map");
    }

    QFileInfo local_info(local_path);
    if (!local_info.exists() || local_info.size() <= 0) {
        if (errorOut) {
            *errorOut = "Latest map download completed but local file is invalid: " + local_path;
        }
        return false;
    }

    updateProgress(35, "Alignment: latest map ready");
    if (localPathOut) {
        *localPathOut = local_path;
    }
    return true;
}

PointCloudPtr CoverageGUI::downsampleForAlignment(const PointCloudPtr& cloud, double voxelSize) const {
    if (!cloud || cloud->empty() || voxelSize <= 0.0) {
        return cloud;
    }
    PointCloudPtr ds(new PointCloud);
    pcl::VoxelGrid<pcl::PointXYZ> vg;
    vg.setInputCloud(cloud);
    vg.setLeafSize(static_cast<float>(voxelSize),
                   static_cast<float>(voxelSize),
                   static_cast<float>(voxelSize));
    vg.filter(*ds);
    return ds;
}

double CoverageGUI::estimateAlignmentError(
    const PointCloudPtr& source,
    const PointCloudPtr& target,
    const Eigen::Matrix4f& transform) const {
    if (!source || !target || source->empty() || target->empty()) {
        return std::numeric_limits<double>::infinity();
    }

    pcl::KdTreeFLANN<pcl::PointXYZ> tree;
    tree.setInputCloud(target);

    const size_t max_samples = 5000;
    const size_t step = std::max<size_t>(1, source->size() / max_samples);
    std::vector<int> idx(1);
    std::vector<float> d2(1);
    double sum = 0.0;
    size_t count = 0;

    for (size_t i = 0; i < source->size(); i += step) {
        const auto& p = source->points[i];
        Eigen::Vector4f pt(p.x, p.y, p.z, 1.0f);
        Eigen::Vector4f q = transform * pt;
        pcl::PointXYZ query(q.x(), q.y(), q.z());
        if (tree.nearestKSearch(query, 1, idx, d2) > 0) {
            sum += std::sqrt(std::max(0.0f, d2[0]));
            count++;
        }
    }
    if (count == 0) {
        return std::numeric_limits<double>::infinity();
    }
    return sum / static_cast<double>(count);
}

void CoverageGUI::applyAlignmentTransformToLoadedData(const Eigen::Matrix4f& transform) {
    if (!pcd_points_ || pcd_points_->empty()) {
        return;
    }

    pcl::transformPointCloud(*pcd_points_, *pcd_points_, transform);
    if (filtered_points_ && filtered_points_.get() != pcd_points_.get()) {
        pcl::transformPointCloud(*filtered_points_, *filtered_points_, transform);
    } else {
        filtered_points_ = pcd_points_;
    }

    // Clear any derived planning artifacts; they must be recomputed in the aligned frame.
    polygon_.clear();
    roi_polygon_.clear();
    obstacles_.clear();
    clearCoverage();
    custom_waypoints_.clear();
    custom_waypoints_visited_.clear();
    refreshCustomPathUI();
    if (btn_delete_selected_obstacle_) {
        btn_delete_selected_obstacle_->setEnabled(false);
    }
    if (lbl_roi_) {
        lbl_roi_->setText("ROI: none");
    }
    if (lbl_obstacles_) {
        lbl_obstacles_->setText("Obstacles: 0");
    }
    if (lbl_reproj_status_) {
        lbl_reproj_status_->setText("No reprojection computed");
    }

    xy_2d_.clear();
    xy_2d_.reserve(filtered_points_ ? filtered_points_->size() : 0);
    if (filtered_points_) {
        for (const auto& pt : filtered_points_->points) {
            xy_2d_.emplace_back(pt.x, pt.y);
        }
    }

    alignment_transform_total_ = transform * alignment_transform_total_;

    plot_->clearAll();
    scheduleFitToView();
    refreshPlot();
}

void CoverageGUI::alignLoadedMapToLatestRobotMap() {
    if (!pcd_points_ || pcd_points_->empty()) {
        QMessageBox::warning(this, "No Loaded Map",
                             "Load a planning map first, then run alignment.");
        return;
    }

    beginProgressOperation("Alignment: finding latest robot map...", true);
    QString latest_path;
    QString fetch_error;
    if (!fetchLatestMapFileForAlignment(&latest_path, &fetch_error)) {
        const bool cancelled = isOperationCancelledMessage(fetch_error);
        endProgressOperation();
        if (cancelled) {
            setStatus("Map alignment cancelled", 5000);
            return;
        }
        QMessageBox::warning(
            this, "Alignment Fetch Failed",
            "Could not fetch the latest robot map for alignment.\n\n" + fetch_error +
                "\n\nTip: verify robot connectivity, then retry.");
        return;
    }

    try {
        updateProgress(40, "Alignment: loading latest robot map...");
        PointCloudPtr target_cloud = loadPointCloudFile(latest_path.toStdString());
        if (isProgressCancelRequested()) {
            throw std::runtime_error(kOperationCancelledMessage);
        }

        updateProgress(55, "Alignment: preparing point clouds...");
        // Alignment uses raw loaded cloud (pcd_points_), not GUI-cropped/filtered cloud.
        PointCloudPtr eval_source_ds = downsampleForAlignment(pcd_points_, 0.15);
        PointCloudPtr eval_target_ds = downsampleForAlignment(target_cloud, 0.15);
        if (!eval_source_ds || !eval_target_ds || eval_source_ds->size() < 50 || eval_target_ds->size() < 50) {
            endProgressOperation();
            QMessageBox::warning(this, "Alignment Failed",
                                 "Not enough points for alignment after downsampling.");
            return;
        }

        updateProgress(60, "Applying manual initial guess...");
        const double tx_guess = spin_align_tx_ ? spin_align_tx_->value() : 0.0;
        const double ty_guess = spin_align_ty_ ? spin_align_ty_->value() : 0.0;
        const double yaw_guess_deg = spin_align_yaw_deg_ ? spin_align_yaw_deg_->value() : 0.0;
        const double yaw_guess_rad = yaw_guess_deg * M_PI / 180.0;

        Eigen::Matrix4f initial = Eigen::Matrix4f::Identity();
        initial(0, 0) = static_cast<float>(std::cos(yaw_guess_rad));
        initial(0, 1) = static_cast<float>(-std::sin(yaw_guess_rad));
        initial(1, 0) = static_cast<float>(std::sin(yaw_guess_rad));
        initial(1, 1) = static_cast<float>(std::cos(yaw_guess_rad));
        initial(0, 3) = static_cast<float>(tx_guess);
        initial(1, 3) = static_cast<float>(ty_guess);

        QString guess_debug = QString("Manual initial guess: yaw=%1 deg, tx=%2 m, ty=%3 m")
                                  .arg(yaw_guess_deg, 0, 'f', 2)
                                  .arg(tx_guess, 0, 'f', 2)
                                  .arg(ty_guess, 0, 'f', 2);

        Eigen::Matrix4f final_tf = initial;
        QString method = combo_align_mode_ ? combo_align_mode_->currentData().toString() : "guess_icp";
        bool icp_converged = false;
        double icp_score = std::numeric_limits<double>::quiet_NaN();
        QString stage_report;

        const double pre_err = estimateAlignmentError(eval_source_ds, eval_target_ds, Eigen::Matrix4f::Identity());
        const double guess_err = estimateAlignmentError(eval_source_ds, eval_target_ds, initial);

        if (method == "guess_icp") {
            struct IcpStage {
                double voxel;
                double max_corr;
                int max_iter;
            };
            const std::vector<IcpStage> stages = {
                {0.35, 3.0, 80},
                {0.18, 1.8, 60},
                {0.10, 0.9, 50},
            };

            Eigen::Matrix4f current_tf = initial;
            bool all_stages_ok = true;
            for (size_t i = 0; i < stages.size(); ++i) {
                if (isProgressCancelRequested()) {
                    throw std::runtime_error(kOperationCancelledMessage);
                }

                const auto& st = stages[i];
                const int percent = 70 + static_cast<int>((20.0 * static_cast<double>(i)) /
                                                          std::max<size_t>(1, stages.size()));
                updateProgress(percent, QString("Running ICP (%1/%2, voxel=%3 m)...")
                                            .arg(i + 1)
                                            .arg(stages.size())
                                            .arg(st.voxel, 0, 'f', 2));

                PointCloudPtr stage_source = downsampleForAlignment(pcd_points_, st.voxel);
                PointCloudPtr stage_target = downsampleForAlignment(target_cloud, st.voxel);
                if (!stage_source || !stage_target || stage_source->size() < 50 || stage_target->size() < 50) {
                    all_stages_ok = false;
                    stage_report += QString("\nStage %1 skipped (insufficient points at voxel=%2)")
                                        .arg(i + 1)
                                        .arg(st.voxel, 0, 'f', 2);
                    continue;
                }

                pcl::IterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;
                icp.setInputSource(stage_source);
                icp.setInputTarget(stage_target);
                icp.setUseReciprocalCorrespondences(true);
                icp.setMaximumIterations(st.max_iter);
                icp.setMaxCorrespondenceDistance(st.max_corr);
                icp.setRANSACOutlierRejectionThreshold(st.max_corr * 0.25);
                icp.setTransformationEpsilon(1e-9);
                icp.setEuclideanFitnessEpsilon(1e-5);

                PointCloud aligned;
                icp.align(aligned, current_tf);
                if (isProgressCancelRequested()) {
                    throw std::runtime_error(kOperationCancelledMessage);
                }

                const bool ok = icp.hasConverged();
                const double fit = icp.getFitnessScore();
                if (ok && std::isfinite(fit)) {
                    current_tf = icp.getFinalTransformation();
                    stage_report += QString("\nStage %1: converged=yes, fitness=%2")
                                        .arg(i + 1)
                                        .arg(fit, 0, 'f', 5);
                    icp_score = fit;
                } else {
                    all_stages_ok = false;
                    stage_report += QString("\nStage %1: converged=no").arg(i + 1);
                }
            }

            icp_converged = all_stages_ok;
            if (std::isfinite(icp_score)) {
                final_tf = current_tf;
            }
        }

        if (isProgressCancelRequested()) {
            throw std::runtime_error(kOperationCancelledMessage);
        }

        updateProgress(95, "Applying alignment transform...");
        const double final_err = estimateAlignmentError(eval_source_ds, eval_target_ds, final_tf);
        applyAlignmentTransformToLoadedData(final_tf);
        endProgressOperation();

        const double yaw_deg = std::atan2(final_tf(1, 0), final_tf(0, 0)) * 180.0 / M_PI;
        const double tx = final_tf(0, 3);
        const double ty = final_tf(1, 3);
        const QString status = QString("Alignment done (manual guess + ICP): yaw=%1 deg, tx=%2 m, ty=%3 m")
                                   .arg(yaw_deg, 0, 'f', 2)
                                   .arg(tx, 0, 'f', 2)
                                   .arg(ty, 0, 'f', 2);
        if (lbl_alignment_status_) {
            lbl_alignment_status_->setText(status);
            lbl_alignment_status_->setStyleSheet("color: #2e7d32; font-size: 10px;");
        }

        QString details = guess_debug + "\n";
        details += QString("Error (mean NN): before=%1 m, initial=%2 m, final=%3 m")
                       .arg(pre_err, 0, 'f', 3)
                       .arg(guess_err, 0, 'f', 3)
                       .arg(final_err, 0, 'f', 3);
        details += QString("\nICP: converged=%1, fitness=%2")
                       .arg(icp_converged ? "yes" : "no")
                       .arg(std::isfinite(icp_score) ? QString::number(icp_score, 'f', 4) : "n/a");
        if (!stage_report.isEmpty()) {
            details += "\n" + stage_report;
        }
        details += "\n\nNext: recompute hull/obstacles/path in the aligned frame.";

        QMessageBox::information(this, "Map Alignment Complete", details);
        setStatus(status, 7000);
    } catch (const std::exception& e) {
        const QString err = QString::fromUtf8(e.what());
        const bool cancelled = isOperationCancelledMessage(err);
        endProgressOperation();
        if (cancelled) {
            setStatus("Map alignment cancelled", 5000);
            return;
        }
        QMessageBox::warning(this, "Alignment Failed",
                             QString("Alignment failed:\n%1").arg(err));
        return;
    }
}

void CoverageGUI::loadPointCloudFromPath(const QString& path) {
    loadPointCloudAsync(path);
}

void CoverageGUI::applyHeightCrop() {
    if (!pcd_points_ || pcd_points_->empty()) {
        QMessageBox::warning(this, "Warning", "Load a point cloud first.");
        return;
    }

    if (height_crop_watcher_ && height_crop_watcher_->isRunning()) {
        QMessageBox::warning(this, "Busy", "Height crop already in progress. Please wait.");
        return;
    }
    
    double z_min = spin_z_min_->value();
    double z_max = spin_z_max_->value();
    
    beginProgressOperation(QString("Applying height crop [%1, %2]m...").arg(z_min).arg(z_max), true, true);

    PointCloudPtr source_cloud = pcd_points_;
    QFuture<HeightCropResult> future = QtConcurrent::run([this, source_cloud, z_min, z_max]() -> HeightCropResult {
        HeightCropResult result;
        result.z_min = z_min;
        result.z_max = z_max;

        try {
            result.filtered_points = filterByZRange(source_cloud, z_min, z_max);
            result.projected_points.clear();
            result.projected_points.reserve(result.filtered_points ? result.filtered_points->size() : 0);
            size_t i = 0;
            for (const auto& pt : result.filtered_points->points) {
                if ((i++ & 0x7FFFu) == 0u && isProgressCancelRequested()) {
                    result.cancelled = true;
                    return result;
                }
                result.projected_points.emplace_back(pt.x, pt.y);
            }
        } catch (const std::exception& e) {
            const QString err = QString::fromUtf8(e.what());
            if (isOperationCancelledMessage(err)) {
                result.cancelled = true;
            } else {
                result.error = err;
            }
        }

        return result;
    });

    height_crop_watcher_->setFuture(future);
}

void CoverageGUI::onHeightCropFinished() {
    const bool cancelled = progress_cancel_requested_.load();
    endProgressOperation();

    if (!height_crop_watcher_) {
        return;
    }

    const HeightCropResult result = height_crop_watcher_->result();
    if (cancelled || result.cancelled) {
        setStatus("Height crop cancelled", 5000);
        return;
    }
    if (!result.error.isEmpty()) {
        QMessageBox::critical(this, "Error", result.error);
        return;
    }
    if (!result.filtered_points || result.filtered_points->empty()) {
        QMessageBox::critical(this, "Error", "Height crop returned no points.");
        return;
    }

    filtered_points_ = result.filtered_points;
    xy_2d_ = result.projected_points;

    // Clear previous polygon/coverage data
    polygon_.clear();
    swaths_.clear();
    route_.clear();
    path_.clear();

    scheduleFitToView();
    refreshPlot();
    setStatus(QString("Filtered to %1 points (Z: %2 to %3 m)")
                  .arg(filtered_points_->size())
                  .arg(result.z_min, 0, 'f', 2)
                  .arg(result.z_max, 0, 'f', 2),
              4000);
}

void CoverageGUI::showPointCloud3D() {
    // Use filtered cloud if available, otherwise use raw cloud
    PointCloudPtr cloud = filtered_points_ ? filtered_points_ : pcd_points_;
    
    if (!cloud || cloud->empty()) {
        QMessageBox::warning(this, "Warning", 
            "Load a point cloud first.\n\n"
            "Use 'Load PCD / PLY / XYZ' to load a file.");
        return;
    }
    
    setStatus(QString("Preparing 3D viewer with %1 points...").arg(cloud->size()));
    beginProgressOperation("Saving temporary point cloud...", true);
    
    // Save point cloud to a temporary file (avoids VTK/Qt threading conflicts)
    std::string temp_pcd = "/tmp/f2c_viewer_temp.pcd";
    std::string temp_path_csv = "/tmp/f2c_viewer_path.csv";
    
    // Create colored point cloud based on Z height
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr colored_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
    colored_cloud->reserve(cloud->size());
    
    // Calculate bounds
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    
    for (size_t i = 0; i < cloud->points.size(); ++i) {
        if ((i & 0x1FFFu) == 0u) {
            const int percent = cloud->points.empty()
                ? 0
                : 5 + static_cast<int>((15.0 * static_cast<double>(i)) /
                                       static_cast<double>(cloud->points.size()));
            updateProgress(percent, "Scanning point cloud heights...");
            if (isProgressCancelRequested()) {
                endProgressOperation();
                setStatus("3D viewer preparation cancelled", 5000);
                return;
            }
        }
        const auto& pt = cloud->points[i];
        min_z = std::min(min_z, pt.z);
        max_z = std::max(max_z, pt.z);
    }
    
    float z_range = max_z - min_z;
    if (z_range < 0.001f) z_range = 1.0f;
    
    // Apply height-based coloring
    for (size_t i = 0; i < cloud->points.size(); ++i) {
        if ((i & 0x1FFFu) == 0u) {
            const int percent = cloud->points.empty()
                ? 20
                : 20 + static_cast<int>((65.0 * static_cast<double>(i)) /
                                        static_cast<double>(cloud->points.size()));
            updateProgress(percent, "Colorizing point cloud for 3D viewer...");
            if (isProgressCancelRequested()) {
                endProgressOperation();
                setStatus("3D viewer preparation cancelled", 5000);
                return;
            }
        }
        const auto& pt = cloud->points[i];
        pcl::PointXYZRGB colored_pt;
        colored_pt.x = pt.x;
        colored_pt.y = pt.y;
        colored_pt.z = pt.z;
        
        float t = (pt.z - min_z) / z_range;
        t = std::max(0.0f, std::min(1.0f, t));
        
        // Rainbow gradient: blue -> cyan -> green -> yellow -> red
        uint8_t r, g, b;
        if (t < 0.25f) {
            float s = t / 0.25f;
            r = 0; g = static_cast<uint8_t>(255 * s); b = 255;
        } else if (t < 0.5f) {
            float s = (t - 0.25f) / 0.25f;
            r = 0; g = 255; b = static_cast<uint8_t>(255 * (1 - s));
        } else if (t < 0.75f) {
            float s = (t - 0.5f) / 0.25f;
            r = static_cast<uint8_t>(255 * s); g = 255; b = 0;
        } else {
            float s = (t - 0.75f) / 0.25f;
            r = 255; g = static_cast<uint8_t>(255 * (1 - s)); b = 0;
        }
        
        colored_pt.r = r;
        colored_pt.g = g;
        colored_pt.b = b;
        colored_cloud->push_back(colored_pt);
    }
    
    // Save to temp file
    if (pcl::io::savePCDFileBinary(temp_pcd, *colored_cloud) != 0) {
        QMessageBox::critical(this, "Error", "Failed to save temporary point cloud file.");
        endProgressOperation();
        return;
    }
    
    // Save path to CSV if available
    bool has_path = !path_.empty();
    if (has_path) {
        std::ofstream path_out(temp_path_csv);
        path_out << "x,y,z\n";
        for (const auto& state : path_) {
            // Use Z=0 for path points (robot operates on XY plane at origin height)
            path_out << std::fixed << std::setprecision(6) 
                     << state.point.x << "," << state.point.y << ",0.0\n";
        }
        path_out.close();
    }
    
    updateProgress(100, "Temporary point cloud ready");
    endProgressOperation();
    
    // Try different viewer options in order of preference
    std::string viewer_cmd;
    
    // Check for Open3D via Python (preferred - supports path visualization)
    if (system("python3 -c 'import open3d' > /dev/null 2>&1") == 0) {
        // Create a Python script for Open3D visualization with origin and path
        std::string py_script = R"PYTHON(
import open3d as o3d
import numpy as np
import sys
import os

pcd_file = sys.argv[1]
path_file = sys.argv[2] if len(sys.argv) > 2 else None

pcd = o3d.io.read_point_cloud(pcd_file)
print(f"Loaded {len(pcd.points)} points")

vis = o3d.visualization.Visualizer()
vis.create_window(window_name="3D Viewer - Point Cloud + Path", width=1200, height=900)
vis.add_geometry(pcd)

opt = vis.get_render_option()
opt.point_size = 2.0
opt.background_color = np.array([0.08, 0.08, 0.12])
opt.show_coordinate_frame = True

origin_sphere = o3d.geometry.TriangleMesh.create_sphere(radius=0.15)
origin_sphere.translate([0, 0, 0])
origin_sphere.paint_uniform_color([0.9, 0.1, 0.1])
vis.add_geometry(origin_sphere)

axis_length = 1.0
axis_radius = 0.03

x_axis = o3d.geometry.TriangleMesh.create_cylinder(radius=axis_radius, height=axis_length)
x_axis.rotate(o3d.geometry.get_rotation_matrix_from_xyz([0, np.pi/2, 0]), center=[0,0,0])
x_axis.translate([axis_length/2, 0, 0])
x_axis.paint_uniform_color([1, 0.2, 0.2])
vis.add_geometry(x_axis)

y_axis = o3d.geometry.TriangleMesh.create_cylinder(radius=axis_radius, height=axis_length)
y_axis.rotate(o3d.geometry.get_rotation_matrix_from_xyz([-np.pi/2, 0, 0]), center=[0,0,0])
y_axis.translate([0, axis_length/2, 0])
y_axis.paint_uniform_color([0.2, 1, 0.2])
vis.add_geometry(y_axis)

z_axis = o3d.geometry.TriangleMesh.create_cylinder(radius=axis_radius, height=axis_length)
z_axis.translate([0, 0, axis_length/2])
z_axis.paint_uniform_color([0.2, 0.2, 1])
vis.add_geometry(z_axis)

if path_file and os.path.exists(path_file):
    try:
        path_data = np.loadtxt(path_file, delimiter=',', skiprows=1)
        if len(path_data) > 1:
            print(f"Loaded path with {len(path_data)} waypoints")
            
            lines = [[i, i+1] for i in range(len(path_data)-1)]
            colors = [[0, 0.9, 0.3] for _ in lines]
            
            path_line = o3d.geometry.LineSet()
            path_line.points = o3d.utility.Vector3dVector(path_data)
            path_line.lines = o3d.utility.Vector2iVector(lines)
            path_line.colors = o3d.utility.Vector3dVector(colors)
            vis.add_geometry(path_line)
            
            start_sphere = o3d.geometry.TriangleMesh.create_sphere(radius=0.1)
            start_sphere.translate(path_data[0])
            start_sphere.paint_uniform_color([0, 1, 0])
            vis.add_geometry(start_sphere)
            
            end_sphere = o3d.geometry.TriangleMesh.create_sphere(radius=0.1)
            end_sphere.translate(path_data[-1])
            end_sphere.paint_uniform_color([1, 1, 0])
            vis.add_geometry(end_sphere)
            
            print("Path visualization added (green line, green=start, yellow=end)")
    except Exception as e:
        print(f"Could not load path: {e}")

ctr = vis.get_view_control()
ctr.set_zoom(0.7)

print("\nControls:")
print("  Left drag: Rotate | Scroll: Zoom | Shift+drag: Pan")
print("  R: Reset view | Q: Close")
print("\nLegend:")
print("  Red sphere: Origin (0,0,0)")
print("  RGB axes: X(red), Y(green), Z(blue)")
print("  Green line: Coverage path")

vis.run()
vis.destroy_window()
)PYTHON";
        
        std::string py_file = "/tmp/f2c_viewer.py";
        std::ofstream py_out(py_file);
        py_out << py_script;
        py_out.close();
        
        if (has_path) {
            viewer_cmd = "python3 " + py_file + " " + temp_pcd + " " + temp_path_csv + " &";
            setStatus("Opening Open3D viewer with path (close window when done)", 5000);
        } else {
            viewer_cmd = "python3 " + py_file + " " + temp_pcd + " &";
            setStatus("Opening Open3D viewer (close window when done)", 5000);
        }
    }
    // Check for pcl_viewer (fallback - no path support but shows coordinate frame)
    else if (system("which pcl_viewer > /dev/null 2>&1") == 0) {
        // pcl_viewer with coordinate axes
        viewer_cmd = "pcl_viewer " + temp_pcd + " -ps 2 -ax 1.0 &";
        if (has_path) {
            setStatus("Opening pcl_viewer (path not shown - install Open3D for path viz)", 5000);
        } else {
            setStatus("Opening pcl_viewer (press Q to close)", 5000);
        }
    }
    // Check for CloudCompare
    else if (system("which CloudCompare > /dev/null 2>&1") == 0) {
        viewer_cmd = "CloudCompare " + temp_pcd + " &";
        setStatus("Opening CloudCompare", 5000);
    }
    else {
        QMessageBox::warning(this, "No 3D Viewer Found",
            "Could not find a suitable 3D point cloud viewer.\n\n"
            "Please install one of the following:\n"
            "  • Open3D (pip install open3d) - recommended for path visualization\n"
            "  • pcl-tools (sudo apt install pcl-tools)\n"
            "  • CloudCompare (sudo apt install cloudcompare)\n\n"
            "The colored point cloud has been saved to:\n" + 
            QString::fromStdString(temp_pcd));
        return;
    }
    
    // Launch viewer in background
    int result = system(viewer_cmd.c_str());
    if (result != 0) {
        QMessageBox::warning(this, "Warning", 
            "Failed to launch 3D viewer.\n"
            "You can manually open the file:\n" + QString::fromStdString(temp_pcd));
    }
}

void CoverageGUI::applyDownsample() {
    if (!filtered_points_ || filtered_points_->empty()) {
        QMessageBox::warning(this, "Warning", "Apply height crop first.");
        return;
    }
    
    QString method = combo_downsample_->currentText().toLower();
    if (method == "none") {
        setStatus("Downsampling skipped", 3000);
        return;
    }
    
    beginProgressOperation(QString("Applying %1 downsampling...").arg(method), true);
    
    try {
        if (method == "random") {
            filtered_points_ = subsampleRandom(filtered_points_, spin_max_points_->value());
        } else if (method == "voxel") {
            filtered_points_ = downsampleVoxel(filtered_points_, spin_voxel_->value());
        } else if (method == "statistical") {
            filtered_points_ = downsampleStatistical(filtered_points_, 
                                                     spin_mean_k_->value(),
                                                     spin_std_ratio_->value());
        }
        if (isProgressCancelRequested()) {
            throw std::runtime_error(kOperationCancelledMessage);
        }
        endProgressOperation();
        setStatus(QString("Downsampled to %1 points").arg(filtered_points_->size()), 4000);
    } catch (const std::exception& e) {
        const QString err = QString::fromUtf8(e.what());
        const bool cancelled = isOperationCancelledMessage(err);
        endProgressOperation();
        if (cancelled) {
            setStatus("Downsampling cancelled", 5000);
            return;
        }
        QMessageBox::critical(this, "Error", err);
    }
}

void CoverageGUI::computeHull() {
    if (!filtered_points_ || filtered_points_->empty()) {
        QMessageBox::warning(this, "Warning", "Apply filtering first.");
        return;
    }
    
    beginProgressOperation("Computing hull...", true);

    try {
        std::vector<Point2D> projected;
        projected.reserve(filtered_points_->size());
        for (size_t i = 0; i < filtered_points_->size(); ++i) {
            if ((i & 0x1FFFu) == 0u) {
                const int percent = filtered_points_->empty()
                    ? 0
                    : static_cast<int>((15.0 * static_cast<double>(i)) /
                                       static_cast<double>(filtered_points_->size()));
                updateProgress(percent, "Projecting point cloud to 2D...");
                if (isProgressCancelRequested()) {
                    throw std::runtime_error(kOperationCancelledMessage);
                }
            }
            const auto& pt = filtered_points_->points[i];
            projected.emplace_back(pt.x, pt.y);
        }
        xy_2d_ = std::move(projected);

        QString method = combo_hull_method_->currentData().toString();
        polygon_ = computeConcaveHull(xy_2d_, spin_alpha_->value(), method.toStdString());

        endProgressOperation();
        scheduleFitToView();
        refreshPlot();
        setStatus(QString("Hull computed with %1 vertices").arg(polygon_.size()), 4000);
    } catch (const std::exception& e) {
        const QString err = QString::fromUtf8(e.what());
        const bool cancelled = isOperationCancelledMessage(err);
        endProgressOperation();
        if (cancelled) {
            setStatus("Hull computation cancelled", 5000);
            return;
        }
        QMessageBox::critical(this, "Error", err);
    }
}

void CoverageGUI::simplifyPolygon() {
    if (polygon_.empty()) {
        QMessageBox::warning(this, "Warning", "Compute hull first.");
        return;
    }
    
    beginProgressOperation("Simplifying polygon...", true);
    
    try {
        polygon_ = f2c_cpp::simplifyPolygon(polygon_, spin_simplify_->value());
        if (isProgressCancelRequested()) {
            throw std::runtime_error(kOperationCancelledMessage);
        }
        endProgressOperation();
        scheduleFitToView();
        refreshPlot();
        setStatus(QString("Simplified to %1 vertices").arg(polygon_.size()), 4000);
    } catch (const std::exception& e) {
        const QString err = QString::fromUtf8(e.what());
        const bool cancelled = isOperationCancelledMessage(err);
        endProgressOperation();
        if (cancelled) {
            setStatus("Polygon simplification cancelled", 5000);
            return;
        }
        QMessageBox::critical(this, "Error", err);
    }
}

void CoverageGUI::toggleROISelection() {
    if (btn_roi_->isChecked()) {
        if (polygon_.empty()) {
            QMessageBox::warning(this, "Warning", "Compute hull first.");
            btn_roi_->setChecked(false);
            return;
        }
        if (btn_measure_ && btn_measure_->isChecked()) {
            btn_measure_->setChecked(false);
        }
        if (plot_) {
            plot_->setMeasureMode(false);
        }
        if (lbl_measure_) {
            lbl_measure_->setText("Distance: -");
        }
        btn_obstacle_->setChecked(false);
        plot_->startROISelection();
        lbl_roi_->setText("ROI: selecting points...");
    } else {
        plot_->cancelSelection();
    }
}

void CoverageGUI::clearROI() {
    roi_polygon_.clear();
    plot_->clearROI();
    lbl_roi_->setText("ROI: none");
    effective_area_m2_ = 0.0;
    clearCoverage();
    setStatus("ROI cleared", 4000);
    refreshPlot();
}

void CoverageGUI::toggleObstacleSelection() {
    if (btn_obstacle_->isChecked()) {
        if (polygon_.empty()) {
            QMessageBox::warning(this, "Warning", "Compute hull first.");
            btn_obstacle_->setChecked(false);
            return;
        }
        if (btn_measure_ && btn_measure_->isChecked()) {
            btn_measure_->setChecked(false);
        }
        if (plot_) {
            plot_->setMeasureMode(false);
        }
        if (lbl_measure_) {
            lbl_measure_->setText("Distance: -");
        }
        btn_roi_->setChecked(false);
        plot_->startObstacleSelection();
        setStatus("Drawing obstacle polygon...");
    } else {
        plot_->cancelSelection();
    }
}

void CoverageGUI::toggleMeasureMode() {
    if (!plot_ || !btn_measure_) return;
    const bool enabled = btn_measure_->isChecked();
    if (enabled) {
        // Disable conflicting selection modes.
        if (btn_roi_) btn_roi_->setChecked(false);
        if (btn_obstacle_) btn_obstacle_->setChecked(false);
        if (btn_rectangle_ && btn_rectangle_->isChecked()) {
            btn_rectangle_->setChecked(false);
            plot_->cancelRectangleMode();
        }
        if (btn_custom_draw_ && btn_custom_draw_->isChecked()) {
            btn_custom_draw_->setChecked(false);
            custom_draw_enabled_ = false;
            plot_->setCustomDrawMode(false);
        }
        if (plot_->isSelecting()) {
            plot_->cancelSelection();
        }
        plot_->setMeasureMode(true);
        if (lbl_measure_) {
            lbl_measure_->setText("Distance: select first point");
        }
        setStatus("Measure mode enabled: click two points on the map", 4000);
    } else {
        plot_->setMeasureMode(false);
        if (lbl_measure_) {
            lbl_measure_->setText("Distance: -");
        }
        setStatus("Measure mode disabled", 3000);
    }
}

void CoverageGUI::onMeasureDistanceUpdated(double distance_m, bool valid) {
    if (!lbl_measure_) return;
    if (!valid) {
        lbl_measure_->setText("Distance: select second point");
        return;
    }
    lbl_measure_->setText(QString("Distance: %1 m").arg(distance_m, 0, 'f', 3));
    setStatus(QString("Measured distance: %1 m").arg(distance_m, 0, 'f', 3), 3000);
}

void CoverageGUI::autoDetectObstacles() {
    if (auto_detect_obstacles_running_) {
        return;
    }
    // Always use the full loaded point cloud for detection (ignore view filtering/cropping).
    if (!pcd_points_ || pcd_points_->empty()) {
        QMessageBox::warning(this, "Warning", "Load a point cloud first.");
        return;
    }

    // Cancel any in-progress selection modes
    if (btn_roi_) btn_roi_->setChecked(false);
    if (btn_obstacle_) btn_obstacle_->setChecked(false);
    plot_->cancelSelection();

    // Detection should always run on the full point cloud.
    // If ROI is selected, we post-filter the detected obstacle shapes for display only.
    const Polygon2D display_roi = roi_polygon_;

    std::vector<PathState> path_snapshot;
    {
        std::lock_guard<std::mutex> lock(robot_pose_mutex_);
        path_snapshot = !driven_path_snapshot_.empty() ? driven_path_snapshot_ : robot_trail_states_;
    }

    auto_detect_obstacles_running_ = true;
    if (btn_auto_detect_obstacles_) {
        btn_auto_detect_obstacles_->setEnabled(false);
    }

    beginProgressOperation("Auto-detecting obstacles...", true, true);
    setStatus("Auto-detecting obstacles (AUTO mode)...");

    PointCloudPtr cloud = pcd_points_;
    ObstacleDetectionParams params;  // defaults mirror the Python script

    auto future = QtConcurrent::run([cloud, path_snapshot, display_roi, params]() mutable {
        ObstacleDetectionResult result = detectObstaclesAuto(cloud, path_snapshot, nullptr, params);
        if (!result.success) {
            return result;
        }
        if (display_roi.size() >= 3) {
            std::vector<Obstacle2D> filtered;
            filtered.reserve(result.obstacles.size());
            for (const auto& obs : result.obstacles) {
                // Keep any obstacle that intersects the ROI (vertex-in-poly or edge intersection).
                bool keep = false;
                // Quick accept: any obstacle outer vertex inside ROI.
                for (const auto& p : obs.outer) {
                    if (pointInPolygonRayCastLocal(p, display_roi)) {
                        keep = true;
                        break;
                    }
                }
                if (!keep) {
                    // Any ROI vertex inside obstacle shape?
                    for (const auto& p : display_roi) {
                        if (pointInObstacleShapeLocal(p, obs)) {
                            keep = true;
                            break;
                        }
                    }
                }
                if (!keep) {
                    // Any edge intersection between obstacle outer ring and ROI boundary?
                    keep = polygonEdgesIntersectLocal(obs.outer, display_roi);
                }
                if (keep) {
                    filtered.push_back(obs);
                }
            }
            result.obstacles = std::move(filtered);
            int holes = 0;
            for (const auto& o : result.obstacles) {
                holes += static_cast<int>(o.holes.size());
            }
            result.stats.total_holes = holes;
            result.stats.obstacle_shapes = static_cast<int>(result.obstacles.size());
        }
        return result;
    });
    obstacle_detect_watcher_->setFuture(future);
}

void CoverageGUI::clearObstacles() {
    obstacles_.clear();
    plot_->clearObstacles();
    lbl_obstacles_->setText("Obstacles: 0");
    effective_area_m2_ = 0.0;
    clearCoverage();
    setStatus("Obstacles cleared", 4000);
    refreshPlot();
}

void CoverageGUI::onObstacleSelectionChanged(int index) {
    if (btn_delete_selected_obstacle_) {
        bool ok = (index >= 0 && index < static_cast<int>(obstacles_.size()));
        btn_delete_selected_obstacle_->setEnabled(ok);
    }
}

void CoverageGUI::deleteSelectedObstacle() {
    const int idx = plot_ ? plot_->selectedObstacleIndex() : -1;
    if (idx < 0) {
        setStatus("No obstacle selected", 3000);
        return;
    }
    onObstacleDeleteRequested(idx);
    if (plot_) {
        plot_->clearObstacleSelection();
    }
}

void CoverageGUI::onObstacleDeleteRequested(int index) {
    if (index < 0 || index >= static_cast<int>(obstacles_.size())) {
        return;
    }
    obstacles_.erase(obstacles_.begin() + index);
    lbl_obstacles_->setText(QString("Obstacles: %1").arg(obstacles_.size()));
    effective_area_m2_ = 0.0;
    clearCoverage();
    refreshPlot();
    setStatus(QString("Obstacle deleted (remaining: %1)").arg(obstacles_.size()), 4000);
}

void CoverageGUI::onAutoDetectObstaclesFinished() {
    auto_detect_obstacles_running_ = false;
    if (btn_auto_detect_obstacles_) {
        btn_auto_detect_obstacles_->setEnabled(true);
    }

    const bool cancelled = progress_cancel_requested_.load();
    endProgressOperation();

    const ObstacleDetectionResult result = obstacle_detect_watcher_->result();
    if (cancelled || isOperationCancelledMessage(QString::fromStdString(result.error_message))) {
        setStatus("Obstacle detection cancelled", 5000);
        return;
    }
    if (!result.success) {
        QMessageBox::critical(this, "Auto-detect Obstacles",
                              QString("Obstacle detection failed:\n\n%1")
                                  .arg(QString::fromStdString(result.error_message)));
        setStatus("Obstacle detection failed", 5000);
        return;
    }

    obstacles_ = result.obstacles;  // replace existing obstacles
    lbl_obstacles_->setText(QString("Obstacles: %1").arg(obstacles_.size()));
    if (btn_delete_selected_obstacle_) {
        btn_delete_selected_obstacle_->setEnabled(false);
    }
    effective_area_m2_ = 0.0;
    clearCoverage();
    refreshPlot();

    setStatus(QString("Auto-detected %1 obstacle(s) (%2 hole(s))")
                  .arg(obstacles_.size())
                  .arg(result.stats.total_holes),
              6000);
}

void CoverageGUI::undoSelectionPoint() {
    plot_->undoLastPoint();
}

void CoverageGUI::finishSelection() {
    plot_->finishSelection();
}

void CoverageGUI::onROISelected(const Polygon2D& roi) {
    roi_polygon_ = roi;
    btn_roi_->setChecked(false);
    lbl_roi_->setText(QString("ROI: %1 vertices").arg(roi.size()));
    effective_area_m2_ = 0.0;
    clearCoverage();
    setStatus("ROI selected", 4000);
    refreshPlot();
}

void CoverageGUI::onObstacleSelected(const Polygon2D& obstacle) {
    obstacles_.push_back(Obstacle2D{obstacle, {}});
    btn_obstacle_->setChecked(false);
    lbl_obstacles_->setText(QString("Obstacles: %1").arg(obstacles_.size()));
    effective_area_m2_ = 0.0;
    clearCoverage();
    setStatus(QString("Obstacle added (total: %1)").arg(obstacles_.size()), 4000);
    refreshPlot();
}

void CoverageGUI::onSelectionCancelled() {
    btn_roi_->setChecked(false);
    btn_obstacle_->setChecked(false);
    if (roi_polygon_.empty()) {
        lbl_roi_->setText("ROI: none");
    }
    setStatus("Selection cancelled", 3000);
}

void CoverageGUI::buildField() {
    // Just used for validation - actual field is built in generate functions
    if (polygon_.empty()) {
        QMessageBox::warning(this, "Warning", "Compute hull first.");
        return;
    }
    setStatus("Field ready for coverage generation", 4000);
}

void CoverageGUI::generateSwaths() {
    if (polygon_.empty()) {
        QMessageBox::warning(this, "Warning", "Compute hull first.");
        return;
    }
    
    beginProgressOperation("Generating swaths...", true);

    try {
        CoverageConfig cfg = currentConfig();
        const Polygon2D* roi_ptr = roi_polygon_.empty() ? nullptr : &roi_polygon_;
        // Pass obstacles to coverage generation
        const std::vector<Obstacle2D>* obs_ptr = obstacles_.empty() ? nullptr : &obstacles_;
        CoverageResult result = generateCoverage(polygon_, cfg, roi_ptr, obs_ptr);

        endProgressOperation();
        if (!result.success) {
            QMessageBox::critical(this, "Error", QString::fromStdString(result.error_message));
        } else {
            swaths_ = result.swaths;
            effective_area_m2_ = result.effective_area_m2;
            route_.clear();
            path_.clear();
            refreshPlot();
            updateCoverageStats();
            setStatus(QString("Generated %1 swaths").arg(swaths_.size()), 4000);
        }
    } catch (const std::exception& e) {
        const QString err = QString::fromUtf8(e.what());
        const bool cancelled = isOperationCancelledMessage(err);
        endProgressOperation();
        if (cancelled) {
            setStatus("Swath generation cancelled", 5000);
            return;
        }
        QMessageBox::critical(this, "Error", err);
    }
}

void CoverageGUI::generateRoute() {
    if (polygon_.empty()) {
        QMessageBox::warning(this, "Warning", "Compute hull first.");
        return;
    }
    
    beginProgressOperation("Generating route...", true);

    try {
        CoverageConfig cfg = currentConfig();
        const Polygon2D* roi_ptr = roi_polygon_.empty() ? nullptr : &roi_polygon_;
        // Pass obstacles to coverage generation
        const std::vector<Obstacle2D>* obs_ptr = obstacles_.empty() ? nullptr : &obstacles_;
        CoverageResult result = generateCoverage(polygon_, cfg, roi_ptr, obs_ptr);

        endProgressOperation();
        if (!result.success) {
            QMessageBox::critical(this, "Error", QString::fromStdString(result.error_message));
        } else {
            swaths_ = result.swaths;
            route_ = result.route;
            effective_area_m2_ = result.effective_area_m2;
            path_.clear();
            path_connector_prefix_count_ = 0;
            planned_path_is_home_ = false;
            refreshPlot();
            updateCoverageStats();
            setStatus(QString("Generated route with %1 waypoints").arg(route_.size()), 4000);
        }
    } catch (const std::exception& e) {
        const QString err = QString::fromUtf8(e.what());
        const bool cancelled = isOperationCancelledMessage(err);
        endProgressOperation();
        if (cancelled) {
            setStatus("Route generation cancelled", 5000);
            return;
        }
        QMessageBox::critical(this, "Error", err);
    }
}

void CoverageGUI::generatePath() {
    if (polygon_.empty()) {
        QMessageBox::warning(this, "Warning", "Compute hull first.");
        return;
    }
    
    beginProgressOperation("Generating path...", true);

    try {
        CoverageConfig cfg = currentConfig();
        const Polygon2D* roi_ptr = roi_polygon_.empty() ? nullptr : &roi_polygon_;
        // Pass obstacles to coverage generation
        const std::vector<Obstacle2D>* obs_ptr = obstacles_.empty() ? nullptr : &obstacles_;
        CoverageResult result = generateCoverage(polygon_, cfg, roi_ptr, obs_ptr);

        if (!result.success) {
            endProgressOperation();
            QMessageBox::critical(this, "Error", QString::fromStdString(result.error_message));
        } else {
            swaths_ = result.swaths;
            route_ = result.route;
            PathStateList final_path = result.path;
            int connector_prefix_count = 0;
            bool added_connector = false;
            const bool plan_to_first = chk_plan_to_first_ ? chk_plan_to_first_->isChecked() : true;
            if (plan_to_first && !final_path.empty()) {
                std::optional<PathState> pose_copy;
                {
                    std::lock_guard<std::mutex> lock(robot_pose_mutex_);
                    pose_copy = robot_pose_state_;
                }
                if (pose_copy.has_value()) {
                    const Point2D start = pose_copy->point;
                    PathStateList best_connector;
                    size_t connect_idx = 0;
                    bool found_connector = false;
                    for (size_t i = 0; i < final_path.size(); ++i) {
                        if ((i & 0x0Fu) == 0u || i + 1 == final_path.size()) {
                            updateProgress(85 + static_cast<int>(
                                                    (10.0 * static_cast<double>(i)) /
                                                    std::max<size_t>(1, final_path.size())),
                                           QString("Planning connector %1/%2...")
                                               .arg(i + 1)
                                               .arg(final_path.size()));
                        }
                        if (isProgressCancelRequested()) {
                            throw std::runtime_error(kOperationCancelledMessage);
                        }
                        const Point2D goal = final_path[i].point;
                        const double dist = std::hypot(start.x - goal.x, start.y - goal.y);
                        if (dist <= 0.05) {
                            connect_idx = i;
                            found_connector = true;
                            break;
                        }
                        PathStateList connector = computeObstacleAvoidingPath(
                            start, goal, -1.0, kConnectorObstacleClearanceM);
                        if (!connector.empty()) {
                            best_connector = std::move(connector);
                            connect_idx = i;
                            found_connector = true;
                            break;
                        }
                    }
                    if (found_connector && connect_idx < final_path.size()) {
                        PathStateList tail(
                            final_path.begin() + static_cast<std::ptrdiff_t>(connect_idx),
                            final_path.end());
                        if (!best_connector.empty()) {
                            const auto& last = best_connector.back().point;
                            const auto& first = tail.front().point;
                            if (pointsAreDedupedEquivalent(last, first)) {
                                tail.erase(tail.begin());
                            }
                            PathStateList combined = best_connector;
                            combined.insert(combined.end(), tail.begin(), tail.end());
                            std::vector<int> combined_flags(best_connector.size(), 0);
                            combined_flags.insert(combined_flags.end(), tail.size(), 1);
                            dedupePathWithFlags(combined, combined_flags);
                            connector_prefix_count = 0;
                            while (connector_prefix_count < static_cast<int>(combined_flags.size()) &&
                                   combined_flags[static_cast<size_t>(connector_prefix_count)] == 0) {
                                ++connector_prefix_count;
                            }
                            final_path = std::move(combined);
                            added_connector = true;
                        } else {
                            final_path = std::move(tail);
                        }
                    } else {
                        QMessageBox::warning(
                            this, "Connector Failed",
                            QString("Failed to plan a connector with clearance %1 m.\n"
                                    "Showing the coverage path only.")
                                .arg(kConnectorObstacleClearanceM, 0, 'f', 2));
                    }
                } else {
                    QMessageBox::warning(
                        this, "No Robot Pose",
                        "Robot pose not available; cannot plan connector to first waypoint.");
                }
            }
            path_ = final_path;
            path_connector_prefix_count_ = added_connector
                ? std::clamp(connector_prefix_count, 0, static_cast<int>(path_.size()))
                : 0;
            planned_path_is_home_ = false;
            effective_area_m2_ = result.effective_area_m2;
            syncPlannedPathCache();
            refreshPlot();
            updateCoverageStats();  // Update statistics after path generation
            if (added_connector) {
                setStatus(QString("Generated path with %1 states (incl. connector: %2 points)")
                              .arg(path_.size())
                              .arg(path_connector_prefix_count_),
                          5000);
            } else {
                setStatus(QString("Generated path with %1 states").arg(path_.size()), 4000);
            }
            // Auto-refresh scan segments if user has a distance set
            if (spin_scan_len_) {
                generateScanSegments();
            }
            endProgressOperation();
        }
    } catch (const std::exception& e) {
        const QString err = QString::fromUtf8(e.what());
        const bool cancelled = isOperationCancelledMessage(err);
        endProgressOperation();
        if (cancelled) {
            setStatus("Path generation cancelled", 5000);
            return;
        }
        QMessageBox::critical(this, "Error", err);
    }
}

int CoverageGUI::estimateTurns(const PathStateList& seg) const {
    if (seg.size() < 3) return 0;
    int turns = 0;
    const double angle_thresh = 25.0 * M_PI / 180.0;  // 25 degrees
    for (size_t i = 1; i + 1 < seg.size(); ++i) {
        const auto& p0 = seg[i - 1].point;
        const auto& p1 = seg[i].point;
        const auto& p2 = seg[i + 1].point;
        double v1x = p1.x - p0.x;
        double v1y = p1.y - p0.y;
        double v2x = p2.x - p1.x;
        double v2y = p2.y - p1.y;
        double len1 = std::hypot(v1x, v1y);
        double len2 = std::hypot(v2x, v2y);
        if (len1 < 1e-3 || len2 < 1e-3) continue;
        double dot = v1x * v2x + v1y * v2y;
        double det = v1x * v2y - v1y * v2x;
        double angle = std::fabs(std::atan2(det, dot));
        if (angle >= angle_thresh) {
            ++turns;
        }
    }
    return turns;
}

std::vector<int> CoverageGUI::selectedScanSegmentIndices() const {
    std::vector<int> out;
    if (!list_scan_segments_) return out;
    for (int i = 0; i < list_scan_segments_->count(); ++i) {
        if (list_scan_segments_->item(i)->checkState() == Qt::Checked) {
            out.push_back(i);
        }
    }
    return out;
}

PathStateList CoverageGUI::buildPublishPathFromSegments(const std::vector<int>& indices) const {
    PathStateList combined;
    for (int idx : indices) {
        if (idx < 0 || idx >= static_cast<int>(scan_segments_.size())) continue;
        const auto& seg = scan_segments_[idx].path;
        if (seg.empty()) continue;
        size_t start = 0;
        if (!combined.empty()) {
            const auto& prev = combined.back().point;
            if (std::hypot(prev.x - seg.front().point.x, prev.y - seg.front().point.y) < 1e-6) {
                start = 1;  // avoid duplicate joint
            }
        }
        combined.insert(combined.end(), seg.begin() + start, seg.end());
    }
    return dedupePathStates(combined);
}

void CoverageGUI::refreshScanSegmentList() {
    if (!list_scan_segments_) return;
    list_scan_segments_->clear();
    size_t done = 0;
    for (const auto& seg : scan_segments_) {
        auto* item = new QListWidgetItem(
            QString("%1 — %2 m, %3 turns").arg(seg.name).arg(seg.length_m, 0, 'f', 1).arg(seg.turns));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setForeground(seg.completed ? QColor("#2e7d32") : QColor("#006064"));
        list_scan_segments_->addItem(item);
        if (seg.completed) ++done;
    }
    if (lbl_scan_progress_) {
        if (scan_segments_.empty()) {
            lbl_scan_progress_->setText("Segments: none");
        } else {
            lbl_scan_progress_->setText(QString("Segments: %1 total | %2 done")
                .arg(scan_segments_.size()).arg(done));
        }
    }
}

void CoverageGUI::generateScanSegments() {
    PathStateList base;
    if (isCustomModeActive()) {
        if (custom_waypoints_.size() < 2) {
            QMessageBox::warning(this, "No path", "Add custom waypoints first.");
            return;
        }
        base.reserve(custom_waypoints_.size());
        for (size_t i = 0; i < custom_waypoints_.size(); ++i) {
            double h = (i + 1 < custom_waypoints_.size())
                ? std::atan2(custom_waypoints_[i + 1].y - custom_waypoints_[i].y,
                             custom_waypoints_[i + 1].x - custom_waypoints_[i].x)
                : 0.0;
            base.push_back(PathState(custom_waypoints_[i], h));
        }
    } else {
        if (path_.size() < 2) {
            QMessageBox::warning(this, "No path", "Generate a coverage path first.");
            return;
        }
        base = dedupePathStates(path_);
    }

    if (base.size() < 2) return;

    std::vector<double> cum(base.size(), 0.0);
    for (size_t i = 1; i < base.size(); ++i) {
        cum[i] = cum[i - 1] + std::hypot(base[i].point.x - base[i - 1].point.x,
                                         base[i].point.y - base[i - 1].point.y);
    }
    double total = cum.back();
    double seg_len = std::max(1.0, spin_scan_len_ ? spin_scan_len_->value() : 500.0);

    scan_segments_.clear();
    active_scan_segment_idx_ = -1;
    auto addSeg = [&](size_t a, size_t b, const QString& name) {
        ScanSegment s;
        s.name = name;
        s.start_m = cum[a];
        s.end_m = cum[b];
        s.length_m = s.end_m - s.start_m;
        s.path.assign(base.begin() + a, base.begin() + b + 1);
        s.turns = estimateTurns(s.path);
        s.completed = false;
        scan_segments_.push_back(std::move(s));
    };

    size_t start_idx = 0;
    int idx = 1;
    for (double target = seg_len; target < total && start_idx < base.size() - 1; target += seg_len) {
        size_t upper = std::lower_bound(cum.begin() + start_idx + 1, cum.end(), target) - cum.begin();
        if (upper >= base.size()) break;
        size_t lower = upper > 0 ? upper - 1 : upper;
        size_t cut = (target - cum[lower] <= cum[upper] - target) ? lower : upper;
        if (cut <= start_idx) cut = std::min(start_idx + 1, base.size() - 1);
        addSeg(start_idx, cut, QString("#Scan %1").arg(idx++));
        start_idx = cut;
    }
    addSeg(start_idx, base.size() - 1, QString("#Scan %1").arg(idx));

    refreshScanSegmentList();
    refreshPlot();
    setStatus(QString("Generated %1 scan segments").arg(scan_segments_.size()), 4000);
}

void CoverageGUI::updateScanSegmentCompletion(double completed_m) {
    bool changed = false;
    size_t done = 0;
    for (auto& seg : scan_segments_) {
        bool now = completed_m >= seg.end_m - 0.05;
        if (now != seg.completed) {
            seg.completed = now;
            changed = true;
        }
        if (seg.completed) ++done;
    }
    if (changed) {
        refreshScanSegmentList();
    }
    if (lbl_scan_progress_ && !scan_segments_.empty()) {
        lbl_scan_progress_->setText(QString("Segments: %1 total | %2 done")
            .arg(scan_segments_.size()).arg(done));
    }
}

void CoverageGUI::publishSelectedScanSegments() {
    if (!hasValidLoginSession()) {
        QMessageBox::warning(this, "Not Logged In",
                             "Please log into the robot before publishing scan segments.");
        return;
    }
    if (!ros_initialized_ || !waypoint_pub_) {
        QMessageBox::warning(this, "ROS2 Not Available",
                             "ROS2 is not available; cannot publish scan segments.");
        return;
    }
    auto idxs = selectedScanSegmentIndices();
    if (idxs.empty()) {
        QMessageBox::information(this, "No selection", "Select one or more scan segments.");
        return;
    }
    PathStateList publish_path = buildPublishPathFromSegments(idxs);
    if (publish_path.size() < 2) {
        QMessageBox::warning(this, "No path", "Selected segments are empty.");
        return;
    }
    publish_path = dedupePathStates(publish_path);

    std_msgs::msg::Float64MultiArray msg;
    msg.data.reserve(publish_path.size() * 2);
    for (const auto& state : publish_path) {
        msg.data.push_back(state.point.x);
        msg.data.push_back(state.point.y);
    }
    waypoint_pub_->publish(msg);

    waypoints_published_ = true;
    if (btn_start_navigation_) btn_start_navigation_->setEnabled(true);
    if (btn_quick_start_) btn_quick_start_->setEnabled(true);
    setStatus(QString("Published %1 scan(s), %2 points").arg(idxs.size()).arg(publish_path.size()), 4000);
}

void CoverageGUI::startSelectedScanSegments() {
    // Always re-publish the selected scan(s) before starting.
    waypoints_published_ = false;
    publishSelectedScanSegments();
    if (!waypoints_published_) {
        return;
    }
    startNavigation();
}

void CoverageGUI::setActiveScanSegmentFromList(int idx) {
    active_scan_segment_idx_ = idx;
    plot_->setActiveScanSegment(active_scan_segment_idx_);
}

void CoverageGUI::clearCoverage() {
    swaths_.clear();
    route_.clear();
    path_.clear();
    path_connector_prefix_count_ = 0;
    planned_path_is_home_ = false;
    effective_area_m2_ = 0.0;
    plot_->clearSwaths();
    plot_->clearRoute();
    plot_->clearPath();
    scan_segments_.clear();
    active_scan_segment_idx_ = -1;
    refreshScanSegmentList();
    plot_->setScanSegments({}, {}, {}, {}, false);
    syncPlannedPathCache();
    resetLiveStatsUI();
    updateCoverageStats();
    setStatus("Coverage cleared", 4000);
    refreshPlot();
}

void CoverageGUI::exportPathCSV() {
    // Determine which path to export based on mode
    PathStateList export_path;
    std::vector<int> export_flags;
    QString mode_label;
    const bool collect_data = chk_collect_data_ ? chk_collect_data_->isChecked() : true;
    
    if (isCustomModeActive()) {
        if (custom_waypoints_.empty()) {
            QMessageBox::warning(this, "Warning", "No custom waypoints to export.");
            return;
        }
        // Convert custom waypoints to PathStateList for export
        export_path.reserve(custom_waypoints_.size());
        for (const auto& pt : custom_waypoints_) {
            PathState ps;
            ps.point = pt;
            ps.heading = 0;
            ps.vx = 0;
            ps.vy = 0;
            export_path.push_back(ps);
        }
        export_flags.assign(export_path.size(), collect_data ? 1 : 0);
        mode_label = "custom";
    } else {
    if (path_.empty()) {
            QMessageBox::warning(this, "Warning", "Generate a coverage path first.");
        return;
        }
        export_path = dedupePathStates(path_);
        export_flags = buildPlannedDataCollectionFlags(
            export_path.size(), collect_data, planned_path_is_home_, path_connector_prefix_count_);
        mode_label = "planned";
    }

    dedupePathWithFlags(export_path, export_flags);
    
    QString filename = QFileDialog::getSaveFileName(this, "Save Path CSV", "", "CSV (*.csv)");
    if (filename.isEmpty()) return;
    
    if (!filename.toLower().endsWith(".csv")) {
        filename += ".csv";
    }
    
    if (savePathWithFlagsToCSV(export_path, export_flags, filename)) {
        QMessageBox::information(this, "Export", 
                                QString("Saved %1 %2 waypoints").arg(export_path.size()).arg(mode_label));
        setStatus("Path exported", 4000);
    } else {
        QMessageBox::critical(this, "Error", "Failed to save file");
    }
}

void CoverageGUI::setupRobotTrackingSubscription() {
    fastlio_sub_.reset();
    
    if (!ros_node_) {
        updateRobotStatusLabel(false);
        return;
    }

    QString topic_qt = robot_odom_topic_.trimmed();
    if (topic_qt.isEmpty()) {
        updateRobotStatusLabel(false);
        return;
    }
    
    // Use reliable QoS to match odom_tilt_corrector.py publisher (default QoS = reliable)
    // best_effort() was causing QoS mismatch - subscriber couldn't receive from reliable publisher
    auto qos = rclcpp::QoS(rclcpp::KeepLast(50)).reliable();
    std::string topic = topic_qt.toStdString();
    std::cout << "[Coverage Planner] Subscribing to robot odom topic: " << topic << " (reliable QoS)" << std::endl;
    fastlio_sub_ = ros_node_->create_subscription<nav_msgs::msg::Odometry>(
        topic, qos,
        [this](const nav_msgs::msg::Odometry::SharedPtr msg) {
            const auto stamp = std::chrono::steady_clock::now();
            bool emit_live_stats = false;
            std::optional<LiveStatsSnapshot> live_snapshot;
            
            tf2::Quaternion q(
                msg->pose.pose.orientation.x,
                msg->pose.pose.orientation.y,
                msg->pose.pose.orientation.z,
                msg->pose.pose.orientation.w);
            double roll, pitch, yaw;
            tf2::Matrix3x3(q).getRPY(roll, pitch, yaw);
            
            PathState state;
            state.point = {msg->pose.pose.position.x, msg->pose.pose.position.y};
            state.heading = yaw;
            state.vx = std::cos(yaw);
            state.vy = std::sin(yaw);
            
            {
                std::lock_guard<std::mutex> lock(robot_pose_mutex_);
                robot_pose_state_ = state;
                
                if (robot_trail_.empty() ||
                    std::hypot(robot_trail_.back().x - state.point.x,
                               robot_trail_.back().y - state.point.y) > 0.03) {
                    robot_trail_.push_back(state.point);
                    robot_trail_states_.push_back(state);
                    if (robot_trail_max_points_ > 0 &&
                        robot_trail_.size() > robot_trail_max_points_) {
                        const size_t remove_n = robot_trail_.size() - robot_trail_max_points_;
                        robot_trail_.erase(robot_trail_.begin(), robot_trail_.begin() + remove_n);
                        if (robot_trail_states_.size() >= remove_n) {
                            robot_trail_states_.erase(robot_trail_states_.begin(),
                                                      robot_trail_states_.begin() + remove_n);
                        } else {
                            robot_trail_states_.clear();
                        }
                    }
                }
                
                last_robot_update_ = std::chrono::steady_clock::now();
            }
            
            if (auto snapshot = updateLiveStatsFromOdom(state, stamp, emit_live_stats)) {
                live_snapshot = *snapshot;
            }
            
            QMetaObject::invokeMethod(this, [this, emit_live_stats, live_snapshot, stamp]() {
                updateRobotStatusLabel(true);
                updateCustomWaypointStatus();
                
                if (emit_live_stats && live_snapshot.has_value()) {
                    updateLiveStatsUI(*live_snapshot);
                }
                
                // Throttle plot refresh to reduce CPU usage at high odom rates
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    stamp - last_plot_refresh_).count();
                if (elapsed >= kPlotRefreshIntervalMs) {
                    last_plot_refresh_ = stamp;
                    refreshPlot();
                }
            }, Qt::QueuedConnection);
        });
    
    updateRobotStatusLabel(false);
}

void CoverageGUI::updateRobotStatusLabel(bool has_fix) {
    if (!lbl_robot_status_) {
        return;
    }
    
    QString topic = robot_odom_topic_.isEmpty() ? "(topic not set)" : robot_odom_topic_;
    if (has_fix) {
        lbl_robot_status_->setText(QString("Robot: tracking (%1)").arg(topic));
        lbl_robot_status_->setStyleSheet("color: #2e7d32; font-size: 10px;");
    } else {
        lbl_robot_status_->setText(QString("Robot: waiting for %1...").arg(topic));
        lbl_robot_status_->setStyleSheet("color: #a66f00; font-size: 10px;");
    }
}

void CoverageGUI::scheduleFitToView() {
    fit_view_pending_ = true;
}

void CoverageGUI::publishWaypoints() {
    if (!hasValidLoginSession()) {
        QMessageBox::warning(this, "Not Logged In",
                             "Please log into the robot before publishing waypoints.");
        return;
    }
    if (!ros_initialized_ || !waypoint_pub_) {
        QMessageBox::warning(this, "ROS2 Not Available",
                             "ROS2 is not available; cannot publish waypoints.");
        return;
    }
    // Build export path (custom vs planned)
    PathStateList export_path;
    std::vector<int> export_flags;
    QString mode_label;
    const bool collect_data = chk_collect_data_ ? chk_collect_data_->isChecked() : true;
    int planned_connector_prefix_count = 0;

    if (isCustomModeActive()) {
        if (custom_waypoints_.size() < 2) {
            QMessageBox::warning(this, "No Path", "Add at least two custom waypoints before publishing.");
            return;
        }
        export_path.reserve(custom_waypoints_.size());
        for (const auto& pt : custom_waypoints_) {
            PathState ps;
            ps.point = pt;
            ps.heading = 0;
            ps.vx = 0;
            ps.vy = 0;
            export_path.push_back(ps);
        }
        mode_label = "custom";
        export_flags.assign(export_path.size(), collect_data ? 1 : 0);

        // Reset visited status for tracking
        custom_waypoints_visited_.assign(custom_waypoints_.size(), false);
        refreshCustomPathUI();
    } else {
        if (path_.empty()) {
            QMessageBox::warning(this, "No Path", "Generate a coverage path first before publishing waypoints.");
            return;
        }
        export_path = dedupePathStates(path_);
        mode_label = "planned";
        planned_connector_prefix_count = std::max(0, path_connector_prefix_count_);
        export_flags.assign(export_path.size(), collect_data ? 1 : 0);
        if (!planned_path_is_home_ && collect_data && planned_connector_prefix_count > 0 && !export_flags.empty()) {
            const size_t prefix = std::min<size_t>(
                static_cast<size_t>(planned_connector_prefix_count), export_flags.size());
            std::fill(export_flags.begin(), export_flags.begin() + static_cast<std::ptrdiff_t>(prefix), 0);
        }
    }

    dedupePathWithFlags(export_path, export_flags);

    // If we're in planned mode, prepend a connector from current pose to first waypoint
    const bool plan_to_first = chk_plan_to_first_ ? chk_plan_to_first_->isChecked() : true;
    const bool allow_connector_prepend = !planned_path_is_home_ && path_connector_prefix_count_ <= 0;
    if (!isCustomModeActive() && plan_to_first && allow_connector_prepend && !export_path.empty()) {
        std::optional<PathState> pose_copy;
        {
            std::lock_guard<std::mutex> lock(robot_pose_mutex_);
            pose_copy = robot_pose_state_;
        }
        if (pose_copy.has_value() && !polygon_.empty()) {
            const Point2D start = pose_copy->point;
            const Point2D goal = export_path.front().point;
            const double dist = std::hypot(start.x - goal.x, start.y - goal.y);
            if (dist > 0.05) {
                PathStateList connector = computeObstacleAvoidingPath(
                    start, goal, -1.0, kConnectorObstacleClearanceM);
                if (!connector.empty()) {
                    if (!connector.empty() && !export_path.empty()) {
                        const auto& last = connector.back().point;
                        const auto& first = export_path.front().point;
                        if (pointsAreDedupedEquivalent(last, first)) {
                            export_path.erase(export_path.begin());
                            if (!export_flags.empty()) {
                                export_flags.erase(export_flags.begin());
                            }
                        }
                    }
                    std::vector<int> combined_flags(connector.size(), 0);
                    combined_flags.insert(combined_flags.end(), export_flags.begin(), export_flags.end());

                    PathStateList combined = connector;
                    combined.insert(combined.end(), export_path.begin(), export_path.end());
                    export_path = std::move(combined);
                    export_flags = std::move(combined_flags);
                    dedupePathWithFlags(export_path, export_flags);
                    planned_connector_prefix_count = countLeadingConnectorFlags(export_flags);
                }
            }
        }
    }

    if (!isCustomModeActive()) {
        export_flags = buildPlannedDataCollectionFlags(
            export_path.size(), collect_data, planned_path_is_home_, planned_connector_prefix_count);
    }

    std_msgs::msg::Float64MultiArray msg;
    msg.data.reserve(export_path.size() * 3);
    for (size_t i = 0; i < export_path.size(); ++i) {
        msg.data.push_back(export_path[i].point.x);
        msg.data.push_back(export_path[i].point.y);
        int flag = (i < export_flags.size()) ? export_flags[i] : 0;
        msg.data.push_back(static_cast<double>(flag));
    }
    waypoint_pub_->publish(msg);
    waypoints_published_ = true;

    setStatus(QString("📡 Published %1 %2 waypoints").arg(export_path.size()).arg(mode_label), 5000);
    if (btn_start_navigation_) btn_start_navigation_->setEnabled(true);
    if (btn_quick_start_) btn_quick_start_->setEnabled(true);
}

void CoverageGUI::planHomePath() {
    std::optional<PathState> pose_copy;
    {
        std::lock_guard<std::mutex> lock(robot_pose_mutex_);
        pose_copy = robot_pose_state_;
    }
    if (!pose_copy.has_value()) {
        QMessageBox::warning(this, "No Robot Pose", "Robot pose not available yet.");
        return;
    }
    if (polygon_.empty()) {
        QMessageBox::warning(this, "No Map", "Compute hull first (boundary is required).");
        return;
    }

    const Point2D start = pose_copy->point;
    const Point2D goal{0.0, 0.0};
    startTransitPathPlanning(start, goal, /*is_home=*/true);
}

void CoverageGUI::publishCustomPath() {
    // Redirect to unified publish function
    publishWaypoints();
}

void CoverageGUI::onPathModeChanged() {
    bool custom_mode = isCustomModeActive();

    // Show/hide control panels
    if (f2c_controls_widget_) {
        f2c_controls_widget_->setVisible(!custom_mode);
    }
    if (custom_controls_widget_) {
        custom_controls_widget_->setVisible(custom_mode);
    }
    
    // Disable drawing if switching away from custom mode
    setCustomModeActive(custom_mode);
    refreshCustomPathUI();
    syncPlannedPathCache();
    updateCoverageStats();
    if (goto_pick_mode_ && btn_go_to_ && btn_go_to_->isChecked()) {
        goto_pick_mode_ = false;
        {
            QSignalBlocker blocker(btn_go_to_);
            btn_go_to_->setChecked(false);
        }
        setStatus("GO TO point-pick cancelled (mode changed).", 3000);
    }
    refreshPlot();
}

bool CoverageGUI::isCustomModeActive() const {
    return radio_mode_custom_ && radio_mode_custom_->isChecked();
}

void CoverageGUI::setCustomModeActive(bool active) {
    if (!plot_) {
        return;
    }
    
    if (!active && custom_draw_enabled_) {
        custom_draw_enabled_ = false;
        if (btn_custom_draw_) {
            QSignalBlocker blocker(btn_custom_draw_);
            btn_custom_draw_->setChecked(false);
        }
    }
    
    // GO TO pick mode also uses map click capture.
    if (goto_pick_mode_) {
        plot_->setCustomDrawMode(true);
    } else {
        plot_->setCustomDrawMode(active && custom_draw_enabled_);
    }
    plot_->setShowCustomPath(active && !custom_waypoints_.empty());
}

void CoverageGUI::onGoToClicked() {
    if (!btn_go_to_) {
        return;
    }

    const bool enabled = btn_go_to_->isChecked();
    goto_pick_mode_ = enabled;

    if (enabled) {
        if (!plot_) {
            goto_pick_mode_ = false;
            btn_go_to_->setChecked(false);
            return;
        }
        // GO TO and custom-draw both consume map clicks; disable custom draw while armed.
        if (custom_draw_enabled_) {
            custom_draw_enabled_ = false;
            if (btn_custom_draw_) {
                QSignalBlocker blocker(btn_custom_draw_);
                btn_custom_draw_->setChecked(false);
            }
        }
        plot_->setCustomDrawMode(true);
        setStatus("GO TO armed: click destination on map.", 5000);
    } else {
        goto_pick_mode_ = false;
        setCustomModeActive(isCustomModeActive());
        setStatus("GO TO cancelled.", 3000);
    }
}

void CoverageGUI::onPlotCustomWaypoint(const Point2D& point) {
    if (goto_pick_mode_) {
        goto_pick_mode_ = false;
        if (btn_go_to_) {
            QSignalBlocker blocker(btn_go_to_);
            btn_go_to_->setChecked(false);
        }
        setCustomModeActive(isCustomModeActive());

        std::optional<PathState> pose_copy;
        {
            std::lock_guard<std::mutex> lock(robot_pose_mutex_);
            pose_copy = robot_pose_state_;
        }
        if (!pose_copy.has_value()) {
            QMessageBox::warning(this, "No Robot Pose", "Robot pose not available yet.");
            return;
        }
        if (polygon_.empty()) {
            QMessageBox::warning(this, "No Map", "Compute hull first (boundary is required).");
            return;
        }

        const Point2D start = pose_copy->point;
        const Point2D goal = point;
        startTransitPathPlanning(start, goal, /*is_home=*/false);
        return;
    }

    if (!isCustomModeActive() || !custom_draw_enabled_) {
        return;
    }
    custom_waypoints_.push_back(point);
    custom_waypoints_visited_.push_back(false);
    refreshCustomPathUI();
    if (isCustomModeActive()) {
        syncPlannedPathCache();
        updateCoverageStats();
    }
}

void CoverageGUI::refreshCustomPathUI() {
    custom_waypoints_visited_.resize(custom_waypoints_.size(), false);
    
    if (list_custom_points_) {
        list_custom_points_->clear();
        for (size_t i = 0; i < custom_waypoints_.size(); ++i) {
            const auto& pt = custom_waypoints_[i];
            bool visited = custom_waypoints_visited_[i];
            QString text = QString("#%1 (%2, %3) %4")
                .arg(i + 1)
                .arg(pt.x, 0, 'f', 2)
                .arg(pt.y, 0, 'f', 2)
                .arg(visited ? "✓ reached" : "→ pending");
            auto* item = new QListWidgetItem(text);
            item->setForeground(visited ? QColor("#2e7d32") : QColor("#006064"));
            list_custom_points_->addItem(item);
        }
    }
    
    size_t next_idx = custom_waypoints_.size();
    for (size_t i = 0; i < custom_waypoints_.size(); ++i) {
        if (!custom_waypoints_visited_[i]) {
            next_idx = i;
            break;
        }
    }

    if (lbl_custom_status_) {
        if (custom_waypoints_.empty()) {
            lbl_custom_status_->setText("No custom waypoints yet.");
        } else if (next_idx >= custom_waypoints_.size()) {
            lbl_custom_status_->setText(QString("Waypoints: %1 (all reached)").arg(custom_waypoints_.size()));
        } else {
            lbl_custom_status_->setText(
                QString("Waypoints: %1 | Next target: #%2")
                .arg(custom_waypoints_.size())
                .arg(next_idx + 1));
        }
    }
    
    if (btn_custom_undo_) {
        btn_custom_undo_->setEnabled(!custom_waypoints_.empty());
    }
    if (btn_custom_clear_) {
        btn_custom_clear_->setEnabled(!custom_waypoints_.empty());
    }
    
    plot_->setCustomPath(custom_waypoints_, custom_waypoints_visited_);
    plot_->setShowCustomPath(isCustomModeActive() && !custom_waypoints_.empty());
}

void CoverageGUI::undoCustomWaypoint() {
    if (custom_waypoints_.empty()) {
        return;
    }
    custom_waypoints_.pop_back();
    if (!custom_waypoints_visited_.empty()) {
        custom_waypoints_visited_.resize(custom_waypoints_.size());
    }
    refreshCustomPathUI();
    if (isCustomModeActive()) {
        syncPlannedPathCache();
        updateCoverageStats();
    }
}

void CoverageGUI::clearCustomWaypoints() {
    if (custom_waypoints_.empty()) {
        return;
    }
    custom_waypoints_.clear();
    custom_waypoints_visited_.clear();
    scan_segments_.clear();
    refreshScanSegmentList();
    plot_->setScanSegments({}, {}, {}, {}, false);
    refreshCustomPathUI();
    if (isCustomModeActive()) {
        syncPlannedPathCache();
        resetLiveStatsUI();
        updateCoverageStats();
    }
}

void CoverageGUI::updateCustomWaypointStatus() {
    if (custom_waypoints_.empty()) {
        return;
    }
    
    std::optional<PathState> pose_copy;
    {
        std::lock_guard<std::mutex> lock(robot_pose_mutex_);
        pose_copy = robot_pose_state_;
    }
    
    if (!pose_copy.has_value()) {
        return;
    }
    
    bool updated = false;
    for (size_t i = 0; i < custom_waypoints_.size(); ++i) {
        if (i >= custom_waypoints_visited_.size()) {
            custom_waypoints_visited_.resize(custom_waypoints_.size(), false);
        }
        if (custom_waypoints_visited_[i]) {
            continue;
        }
        
        const auto& target = custom_waypoints_[i];
        double dist = std::hypot(pose_copy->point.x - target.x,
                                 pose_copy->point.y - target.y);
        if (dist <= custom_waypoint_reach_tol_) {
            custom_waypoints_visited_[i] = true;
            updated = true;
            continue;
        }
        break;  // Waypoints are sequential
    }
    
    if (updated) {
        refreshCustomPathUI();
    }
}

void CoverageGUI::startNavigation() {
    if (!hasValidLoginSession()) {
        QMessageBox::warning(this, "Not Logged In",
                             "Please log into the robot before starting navigation.");
        return;
    }
    if (!waypoints_published_) {
        QMessageBox::warning(this, "Waypoints Not Published", "Please publish waypoints first.");
        return;
    }
    if (!ros_initialized_ || !waypoint_pub_) {
        QMessageBox::warning(this, "ROS2 Not Available",
                             "ROS2 is not available; cannot start navigation.");
        return;
    }

    std_msgs::msg::Float64MultiArray start_msg;
    start_msg.data = {0.0};  // Start signal for controllers listening on /f2c_waypoints
    waypoint_pub_->publish(start_msg);

    setStatus("🚀 Navigation started!", 3000);
    std::cout << "[Coverage Planner] Start signal published to /f2c_waypoints" << std::endl;
    
    // Start scan session tracking (GPS accumulation + stats recording)
    QString sectionName = QString("Section_%1")
        .arg(QDateTime::currentDateTime().toString("HHmmss"));
    startScanSession(sectionName);
}

// Helper: find closest point on a line segment to a given point
static Point2D closestPointOnSegment(const Point2D& p, const Point2D& a, const Point2D& b) {
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double len_sq = dx * dx + dy * dy;
    
    if (len_sq < 1e-10) {
        // Degenerate segment
        return a;
    }
    
    // Project p onto line, clamped to segment [0,1]
    double t = std::max(0.0, std::min(1.0,
        ((p.x - a.x) * dx + (p.y - a.y) * dy) / len_sq));
    
    return Point2D{a.x + t * dx, a.y + t * dy};
}

// Helper: find closest point on a polyline path to a given point
static std::pair<Point2D, double> closestPointOnPath(const Point2D& p, const std::vector<Point2D>& path) {
    if (path.empty()) {
        return {p, std::numeric_limits<double>::max()};
    }
    if (path.size() == 1) {
        double dist = std::hypot(p.x - path[0].x, p.y - path[0].y);
        return {path[0], dist};
    }
    
    Point2D best_point = path[0];
    double best_dist = std::numeric_limits<double>::max();
    
    for (size_t i = 1; i < path.size(); ++i) {
        Point2D closest = closestPointOnSegment(p, path[i-1], path[i]);
        double dist = std::hypot(p.x - closest.x, p.y - closest.y);
        if (dist < best_dist) {
            best_dist = dist;
            best_point = closest;
        }
    }
    
    return {best_point, best_dist};
}

void CoverageGUI::computeReprojectionError() {
    // Determine which waypoints to use based on current mode
    std::vector<Point2D> path_points;
    if (isCustomModeActive()) {
        path_points = custom_waypoints_;
    } else {
        // Use F2C path waypoints
        for (const auto& state : path_) {
            path_points.push_back(state.point);
        }
    }
    
    if (path_points.size() < 2) {
        QMessageBox::warning(this, "No Path", 
            "Need at least 2 waypoints. Generate or draw a path first.");
        return;
    }
    
    // Get current robot trail
    std::vector<Point2D> full_trail;
    {
        std::lock_guard<std::mutex> lock(robot_pose_mutex_);
        full_trail = robot_trail_;
    }
    
    if (full_trail.size() < 2) {
        QMessageBox::warning(this, "No Trail", 
            "Robot trail is empty. Move the robot first.");
        return;
    }
    
    // Parameters
    const double sample_interval = 0.05;  // 5cm sampling along trail
    const double max_association_dist = 1.0;  // 1m threshold
    const double start_threshold = 0.05;  // 5cm threshold to detect path start
    
    // Find where trail first enters the path region (within 5cm of first waypoint)
    // This excludes the approach path before the robot starts following the planned route
    const Point2D& first_waypoint = path_points.front();
    size_t trail_start_idx = 0;
    bool found_start = false;
    
    for (size_t i = 0; i < full_trail.size(); ++i) {
        double dist_to_start = std::hypot(full_trail[i].x - first_waypoint.x,
                                          full_trail[i].y - first_waypoint.y);
        if (dist_to_start <= start_threshold) {
            trail_start_idx = i;
            found_start = true;
            break;
        }
    }
    
    if (!found_start) {
        QMessageBox::information(this, "Path Not Started", 
            QString("Robot trail never came within %1 cm of the first waypoint.\n"
                    "Make sure the robot has started following the planned path.")
                .arg(start_threshold * 100, 0, 'f', 0));
        return;
    }
    
    // Use only the portion of trail from path start onward
    std::vector<Point2D> trail(full_trail.begin() + trail_start_idx, full_trail.end());
    
    if (trail.size() < 2) {
        QMessageBox::warning(this, "Insufficient Trail", 
            "Not enough trail data after reaching the first waypoint.");
        return;
    }
    
    reproj_lines_.clear();
    
    // Resample the trail at 5cm intervals
    std::vector<Point2D> sampled_trail;
    sampled_trail.push_back(trail[0]);
    double accumulated_dist = 0;
    
    for (size_t i = 1; i < trail.size(); ++i) {
        double seg_dist = std::hypot(trail[i].x - trail[i-1].x, 
                                      trail[i].y - trail[i-1].y);
        accumulated_dist += seg_dist;
        
        // Add sample points at each 5cm interval
        while (accumulated_dist >= sample_interval) {
            // Interpolate position at sample point
            double overshoot = accumulated_dist - sample_interval;
            double ratio = (seg_dist > 1e-6) ? (seg_dist - overshoot) / seg_dist : 1.0;
            
            Point2D sample;
            sample.x = trail[i-1].x + ratio * (trail[i].x - trail[i-1].x);
            sample.y = trail[i-1].y + ratio * (trail[i].y - trail[i-1].y);
            sampled_trail.push_back(sample);
            
            accumulated_dist -= sample_interval;
        }
    }
    
    // For each sampled trail point, find closest point on planned path
    int sample_index = 0;
    for (const auto& trail_pt : sampled_trail) {
        auto [closest_path_pt, dist] = closestPointOnPath(trail_pt, path_points);
        
        // Only include if within threshold
        if (dist <= max_association_dist) {
            ReprojectionLine line;
            line.waypoint = closest_path_pt;  // Point on planned path
            line.traversed = trail_pt;         // Point on robot trail
            line.error_m = dist;
            line.waypoint_index = sample_index;
            reproj_lines_.push_back(line);
        }
        sample_index++;
    }
    
    if (reproj_lines_.empty()) {
        QMessageBox::information(this, "No Match", 
            "No trail points found within 1m of the planned path.\n"
            "Make sure the robot has traversed near the path.");
        return;
    }
    
    // Compute statistics
    double total_error = 0;
    double max_error = 0;
    for (const auto& line : reproj_lines_) {
        total_error += line.error_m;
        max_error = std::max(max_error, line.error_m);
    }
    double avg_error = total_error / reproj_lines_.size();
    
    // Update plot
    plot_->setReprojectionLines(reproj_lines_);
    
    // Update status
    double trail_length_m = 0;
    for (size_t i = 1; i < trail.size(); ++i) {
        trail_length_m += std::hypot(trail[i].x - trail[i-1].x, trail[i].y - trail[i-1].y);
    }
    
    QString status = QString("Reprojection: %1 samples over %2m, avg=%3 cm, max=%4 cm")
        .arg(reproj_lines_.size())
        .arg(trail_length_m, 0, 'f', 1)
        .arg(avg_error * 100, 0, 'f', 1)
        .arg(max_error * 100, 0, 'f', 1);
    if (lbl_reproj_status_) {
        lbl_reproj_status_->setText(status);
        lbl_reproj_status_->setStyleSheet("color: #1565c0; font-size: 10px;");
    }
    setStatus(status, 5000);
    
    std::cout << "[Coverage Planner] Reprojection error computed: " << reproj_lines_.size() 
              << " samples over " << trail_length_m << "m, avg=" << (avg_error * 100) 
              << " cm, max=" << (max_error * 100) << " cm" << std::endl;
}

void CoverageGUI::clearReprojectionError() {
    reproj_lines_.clear();
    plot_->clearReprojectionLines();
    if (lbl_reproj_status_) {
        lbl_reproj_status_->setText("No reprojection computed");
        lbl_reproj_status_->setStyleSheet("color: #666; font-size: 10px;");
    }
    setStatus("Reprojection error cleared", 3000);
}

// =============================================================================
// Async Point Cloud Loading
// =============================================================================

void CoverageGUI::loadPointCloudAsync(const QString& path) {
    if (pcd_watcher_->isRunning()) {
        QMessageBox::warning(this, "Busy", "Already loading a point cloud. Please wait.");
        return;
    }
    
    pending_load_path_ = path;
    beginProgressOperation("Loading point cloud...", true, true);
    setStatus("Loading " + QFileInfo(path).fileName() + " (async)...");
    
    // Run loading in background thread
    QFuture<PointCloudPtr> future = QtConcurrent::run([path]() -> PointCloudPtr {
        try {
            return loadPointCloudFile(path.toStdString());
        } catch (const std::exception& e) {
            std::cerr << "Async load error: " << e.what() << std::endl;
            return nullptr;
        }
    });
    
    pcd_watcher_->setFuture(future);
}

void CoverageGUI::onPointCloudLoaded() {
    const bool cancelled = progress_cancel_requested_.load();
    endProgressOperation();
    
    PointCloudPtr result = pcd_watcher_->result();
    if (cancelled) {
        pending_load_path_.clear();
        setStatus("Point cloud load cancelled", 5000);
        return;
    }
    
    if (!result || result->empty()) {
        QMessageBox::critical(this, "Error", "Failed to load point cloud: " + pending_load_path_);
        pending_load_path_.clear();
        return;
    }
    
    pcd_points_ = result;
    filtered_points_ = pcd_points_;
    loaded_file_ = pending_load_path_;
    lbl_file_->setText(QFileInfo(pending_load_path_).fileName());
    
    // Clear old data
    polygon_.clear();
    roi_polygon_.clear();
    obstacles_.clear();
    swaths_.clear();
    route_.clear();
    path_.clear();
    alignment_transform_total_ = Eigen::Matrix4f::Identity();

    // Snapshot driven path at map-load time (used for obstacle detection)
    {
        std::lock_guard<std::mutex> lock(robot_pose_mutex_);
        driven_path_snapshot_ = robot_trail_states_;
    }
    
    lbl_roi_->setText("ROI: none");
    lbl_obstacles_->setText("Obstacles: 0");
    if (lbl_alignment_status_) {
        lbl_alignment_status_->setText("Alignment: not run");
        lbl_alignment_status_->setStyleSheet("color: #666; font-size: 10px;");
    }
    
    plot_->clearAll();
    
    // Project points to 2D immediately for visualization
    xy_2d_.clear();
    xy_2d_.reserve(pcd_points_->size());
    for (const auto& pt : pcd_points_->points) {
        xy_2d_.emplace_back(pt.x, pt.y);
    }
    
    scheduleFitToView();
    refreshPlot();
    setStatus(QString("Loaded %1 points from %2")
              .arg(pcd_points_->size())
              .arg(QFileInfo(pending_load_path_).fileName()), 4000);
    
    pending_load_path_.clear();
}

// =============================================================================
// Dark Mode
// =============================================================================

void CoverageGUI::toggleDarkMode() {
    dark_mode_ = btn_dark_mode_ ? btn_dark_mode_->isChecked() : !dark_mode_;
    
    QSettings settings("PilotControl", "BDRCoveragePlanner");
    settings.setValue("dark_mode", dark_mode_);
    
    applyTheme();
}

void CoverageGUI::applyTheme() {
    QPalette palette;
    QString styleSheet;
    
    if (dark_mode_) {
        // Dark theme colors
        palette.setColor(QPalette::Window, QColor(45, 45, 48));
        palette.setColor(QPalette::WindowText, QColor(220, 220, 220));
        palette.setColor(QPalette::Base, QColor(30, 30, 32));
        palette.setColor(QPalette::AlternateBase, QColor(45, 45, 48));
        palette.setColor(QPalette::ToolTipBase, QColor(60, 60, 65));
        palette.setColor(QPalette::ToolTipText, QColor(220, 220, 220));
        palette.setColor(QPalette::Text, QColor(220, 220, 220));
        palette.setColor(QPalette::Button, QColor(55, 55, 58));
        palette.setColor(QPalette::ButtonText, QColor(220, 220, 220));
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Link, QColor(42, 130, 218));
        palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        
        styleSheet = R"(
            QGroupBox {
                border: 1px solid #555;
                border-radius: 4px;
                margin-top: 8px;
                padding-top: 8px;
                color: #ddd;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 5px;
                color: #aaa;
            }
            QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
                background-color: #2d2d30;
                border: 1px solid #555;
                border-radius: 3px;
                padding: 3px;
                color: #ddd;
            }
            QPushButton {
                background-color: #3d3d40;
                border: 1px solid #555;
                border-radius: 4px;
                padding: 5px 10px;
                color: #ddd;
            }
            QPushButton:hover {
                background-color: #4d4d50;
            }
            QPushButton:pressed {
                background-color: #2d2d30;
            }
            QPushButton:checked {
                background-color: #2a82da;
                color: white;
            }
            QListWidget {
                background-color: #2d2d30;
                border: 1px solid #555;
                color: #ddd;
            }
            QScrollBar:vertical {
                background: #2d2d30;
                width: 12px;
            }
            QScrollBar::handle:vertical {
                background: #555;
                border-radius: 6px;
                min-height: 20px;
            }
            QStatusBar {
                background-color: #2d2d30;
                color: #aaa;
            }
            /* Special action buttons - dark mode */
            #btn_fetch {
                background-color: #1e5a6e;
                color: #fff;
                font-weight: bold;
            }
            #btn_fetch:hover {
                background-color: #267a8e;
            }
            #btn_custom_draw:checked {
                background-color: #2e7d32;
                color: white;
            }
            #btn_publish {
                background-color: #2e7d32;
                color: white;
                font-weight: bold;
            }
            #btn_publish:hover {
                background-color: #388e3c;
            }
            #btn_navigation {
                background-color: #1565c0;
                color: white;
                font-weight: bold;
            }
            #btn_navigation:hover {
                background-color: #1976d2;
            }
            /* Layer panel - dark mode */
            #layerPanel {
                background-color: #2d2d30;
            }
        )";
        
        if (btn_dark_mode_) {
            btn_dark_mode_->setText("☀️ Light Mode");
        }
    } else {
        // Light theme (default Qt palette)
        palette = QApplication::style()->standardPalette();
        styleSheet = R"(
            /* Special action buttons - light mode */
            #btn_fetch {
                background-color: #e3f2fd;
                color: #1565c0;
                font-weight: bold;
                border: 1px solid #90caf9;
            }
            #btn_fetch:hover {
                background-color: #bbdefb;
            }
            #btn_custom_draw:checked {
                background-color: #81C784;
                color: white;
            }
            #btn_publish {
                background-color: #4CAF50;
                color: white;
                font-weight: bold;
            }
            #btn_publish:hover {
                background-color: #66bb6a;
            }
            #btn_navigation {
                background-color: #2196F3;
                color: white;
                font-weight: bold;
            }
            #btn_navigation:hover {
                background-color: #42a5f5;
            }
        )";
        
        if (btn_dark_mode_) {
            btn_dark_mode_->setText("🌙 Dark Mode");
        }
    }
    
    QApplication::setPalette(palette);
    qApp->setStyleSheet(styleSheet);
    
    // Update plot widget
    if (plot_) {
        plot_->setDarkMode(dark_mode_);
    }
}

void CoverageGUI::resetLiveStatsUI() {
    auto setLabel = [](QLabel* lbl) {
        if (lbl) {
            lbl->setText("-");
        }
    };
    setLabel(lbl_stats_live_progress_);
    setLabel(lbl_stats_live_travel_);
    setLabel(lbl_stats_live_remaining_);
    setLabel(lbl_stats_live_eta_);
    setLabel(lbl_stats_live_elapsed_);
    setLabel(lbl_stats_live_speed_);
    last_live_snapshot_.reset();
    if (plot_) {
        plot_->setLiveOverlay(false, {});
    }
}

void CoverageGUI::syncPlannedPathCache() {
    std::vector<Point2D> path_points;
    if (isCustomModeActive()) {
        path_points = custom_waypoints_;
    } else {
        PathStateList deduped = dedupePathStates(path_);
        path_points.reserve(deduped.size());
        for (const auto& state : deduped) {
            path_points.push_back(state.point);
        }
    }
    rebuildPlannedPathCache(path_points);
    if (path_points.size() < 2) {
        resetLiveStatsUI();
    }
    rebuildLiveOverlay();
}

void CoverageGUI::rebuildPlannedPathCache(const std::vector<Point2D>& path_points) {
    std::lock_guard<std::mutex> lock(live_stats_mutex_);
    planned_path_points_ = path_points;
    planned_cumulative_dist_.assign(path_points.size(), 0.0);
    planned_path_length_m_ = 0.0;
    planned_segment_hint_ = 0;
    live_completed_m_ = 0.0;
    live_traveled_m_ = 0.0;
    live_filtered_speed_mps_ = 0.0;
    live_progress_active_ = false;
    mission_timer_active_ = false;
    last_odom_point_live_.reset();
    mission_start_time_ = std::chrono::steady_clock::now();
    last_live_ui_update_ = mission_start_time_;
    
    if (path_points.size() >= 2) {
        for (size_t i = 1; i < path_points.size(); ++i) {
            double seg_len = std::hypot(path_points[i].x - path_points[i-1].x,
                                        path_points[i].y - path_points[i-1].y);
            planned_cumulative_dist_[i] = planned_cumulative_dist_[i-1] + seg_len;
        }
        planned_path_length_m_ = planned_cumulative_dist_.back();
    }
}

double CoverageGUI::projectAlongPlannedPath(const Point2D& point, size_t& segment_hint) const {
    if (planned_path_points_.size() < 2 || planned_cumulative_dist_.size() != planned_path_points_.size()) {
        return 0.0;
    }
    
    const size_t n = planned_path_points_.size();
    size_t best_seg = 0;
    double best_dist = std::numeric_limits<double>::max();
    double best_t = 0.0;
    
    auto searchRange = [&](size_t start, size_t end) {
        for (size_t i = start; i + 1 < end; ++i) {
            const auto& a = planned_path_points_[i];
            const auto& b = planned_path_points_[i + 1];
            double dx = b.x - a.x;
            double dy = b.y - a.y;
            double len_sq = dx * dx + dy * dy;
            if (len_sq < 1e-9) {
                continue;
            }
            
            double t = std::max(0.0, std::min(1.0,
                ((point.x - a.x) * dx + (point.y - a.y) * dy) / len_sq));
            double proj_x = a.x + t * dx;
            double proj_y = a.y + t * dy;
            double dist = std::hypot(point.x - proj_x, point.y - proj_y);
            
            if (dist < best_dist) {
                best_dist = dist;
                best_seg = i;
                best_t = t;
            }
        }
    };
    
    size_t start_seg = (segment_hint > 5) ? (segment_hint - 5) : 0;
    size_t end_seg = std::min(n - 1, segment_hint + 6);
    searchRange(start_seg, end_seg);
    
    // Fallback to full search if far from hint (e.g., teleport or reset)
    if (best_dist > 1.5) {
        searchRange(0, n - 1);
    }
    
    segment_hint = best_seg;
    double seg_len = planned_cumulative_dist_[best_seg + 1] - planned_cumulative_dist_[best_seg];
    return planned_cumulative_dist_[best_seg] + best_t * seg_len;
}

std::optional<CoverageGUI::LiveStatsSnapshot> CoverageGUI::updateLiveStatsFromOdom(
    const PathState& state,
    std::chrono::steady_clock::time_point stamp,
    bool& should_emit_ui) {
    
    std::lock_guard<std::mutex> lock(live_stats_mutex_);
    should_emit_ui = false;
    
    if (planned_path_points_.size() < 2 || planned_path_length_m_ <= 0.0) {
        return std::nullopt;
    }
    
    if (last_odom_point_live_) {
        double seg = std::hypot(state.point.x - last_odom_point_live_->x,
                                state.point.y - last_odom_point_live_->y);
        live_traveled_m_ += seg;
        
        double dt = std::chrono::duration<double>(stamp - last_odom_time_live_).count();
        if (dt > 1e-3) {
            double inst_speed = seg / dt;
            double alpha = 0.25;
            live_filtered_speed_mps_ = (live_filtered_speed_mps_ <= 0.0)
                ? inst_speed
                : alpha * inst_speed + (1.0 - alpha) * live_filtered_speed_mps_;
        }
    }
    
    last_odom_point_live_ = state.point;
    last_odom_time_live_ = stamp;
    
    double dist_to_start = std::hypot(
        state.point.x - planned_path_points_.front().x,
        state.point.y - planned_path_points_.front().y);
    
    if (!live_progress_active_ && dist_to_start <= kLiveStartGateM) {
        live_progress_active_ = true;
        mission_timer_active_ = true;
        mission_start_time_ = stamp;
        live_completed_m_ = 0.0;
        live_traveled_m_ = 0.0;
        live_filtered_speed_mps_ = 0.0;
    }
    
    if (live_progress_active_) {
        double along = projectAlongPlannedPath(state.point, planned_segment_hint_);
        live_completed_m_ = std::max(live_completed_m_, along);
    }
    
    LiveStatsSnapshot snapshot;
    snapshot.planned_length_m = planned_path_length_m_;
    snapshot.completed_m = live_completed_m_;
    snapshot.remaining_m = std::max(0.0, planned_path_length_m_ - live_completed_m_);
    snapshot.coverage_pct = planned_path_length_m_ > 0.0
        ? std::clamp((live_completed_m_ / planned_path_length_m_) * 100.0, 0.0, 100.0)
        : 0.0;
    snapshot.traveled_m = live_traveled_m_;
    snapshot.speed_mps = live_filtered_speed_mps_;
    snapshot.elapsed_sec = mission_timer_active_
        ? std::chrono::duration<double>(stamp - mission_start_time_).count()
        : 0.0;
    snapshot.eta_sec = (live_filtered_speed_mps_ > kEtaMinSpeed && snapshot.remaining_m > 0.0 && live_progress_active_)
        ? snapshot.remaining_m / live_filtered_speed_mps_
        : 0.0;
    snapshot.active = live_progress_active_;
    
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        stamp - last_live_ui_update_).count();
    if (elapsed_ms >= kLiveUiIntervalMs) {
        last_live_ui_update_ = stamp;
        should_emit_ui = true;
    }
    
    return snapshot;
}

void CoverageGUI::updateLiveStatsUI(const LiveStatsSnapshot& snapshot) {
    auto formatLen = [](double val) -> QString {
        return (val > 0.0) ? QString("%1 m").arg(val, 0, 'f', 1) : "-";
    };
    
    auto formatTime = [](double seconds) -> QString {
        if (seconds <= 0.0) return "-";
        int mins = static_cast<int>(seconds / 60.0);
        int secs = static_cast<int>(seconds) % 60;
        return QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
    };
    
    if (lbl_stats_live_progress_) {
        if (!snapshot.active || snapshot.planned_length_m <= 0.0) {
            lbl_stats_live_progress_->setText("-");
        } else {
            lbl_stats_live_progress_->setText(
                QString("%1% (%2 / %3 m)")
                .arg(snapshot.coverage_pct, 0, 'f', 0)
                .arg(snapshot.completed_m, 0, 'f', 1)
                .arg(snapshot.planned_length_m, 0, 'f', 1));
        }
    }
    
    if (lbl_stats_live_travel_) {
        lbl_stats_live_travel_->setText(formatLen(snapshot.traveled_m));
    }
    if (lbl_stats_live_remaining_) {
        lbl_stats_live_remaining_->setText(formatLen(snapshot.remaining_m));
    }
    if (lbl_stats_live_eta_) {
        lbl_stats_live_eta_->setText(formatTime(snapshot.eta_sec));
    }
    if (lbl_stats_live_elapsed_) {
        lbl_stats_live_elapsed_->setText(formatTime(snapshot.elapsed_sec));
    }
    if (lbl_stats_live_speed_) {
        lbl_stats_live_speed_->setText(
            snapshot.speed_mps > 0.0
            ? QString("%1 m/s").arg(snapshot.speed_mps, 0, 'f', 2)
            : "-");
    }
    
    updateScanSegmentCompletion(snapshot.completed_m);
    
    last_live_snapshot_ = snapshot;
    rebuildLiveOverlay();
}

std::vector<QString> CoverageGUI::buildLiveOverlayLines(const LiveStatsSnapshot& snapshot) const {
    std::vector<QString> lines;
    
    auto formatLen = [](double val) -> QString {
        return (val > 0.0) ? QString("%1 m").arg(val, 0, 'f', 1) : "-";
    };
    auto formatTime = [](double seconds) -> QString {
        if (seconds <= 0.0) return "-";
        int mins = static_cast<int>(seconds / 60.0);
        int secs = static_cast<int>(seconds) % 60;
        return QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
    };
    auto formatPct = [](double pct) -> QString {
        return pct > 0.0 ? QString("%1%").arg(pct, 0, 'f', 0) : "-";
    };
    
    if (chk_live_show_progress_ && chk_live_show_progress_->isChecked()) {
        lines.push_back(QString("Progress: %1 / %2 (%3)")
            .arg(formatLen(snapshot.completed_m))
            .arg(formatLen(snapshot.planned_length_m))
            .arg(formatPct(snapshot.coverage_pct)));
    }
    if (chk_live_show_travel_ && chk_live_show_travel_->isChecked()) {
        lines.push_back(QString("Traveled: %1").arg(formatLen(snapshot.traveled_m)));
    }
    if (chk_live_show_remaining_ && chk_live_show_remaining_->isChecked()) {
        lines.push_back(QString("Remaining: %1").arg(formatLen(snapshot.remaining_m)));
    }
    if (chk_live_show_eta_ && chk_live_show_eta_->isChecked()) {
        lines.push_back(QString("ETA: %1").arg(formatTime(snapshot.eta_sec)));
    }
    if (chk_live_show_elapsed_ && chk_live_show_elapsed_->isChecked()) {
        lines.push_back(QString("Elapsed: %1").arg(formatTime(snapshot.elapsed_sec)));
    }
    if (chk_live_show_speed_ && chk_live_show_speed_->isChecked()) {
        lines.push_back(snapshot.speed_mps > 0.0
            ? QString("Speed: %1 m/s").arg(snapshot.speed_mps, 0, 'f', 2)
            : QString("Speed: -"));
    }
    return lines;
}

void CoverageGUI::rebuildLiveOverlay() {
    if (!plot_) {
        return;
    }
    bool enabled = chk_live_overlay_ && chk_live_overlay_->isChecked();
    if (!enabled || !last_live_snapshot_.has_value()) {
        plot_->setLiveOverlay(false, {});
        return;
    }
    auto lines = buildLiveOverlayLines(*last_live_snapshot_);
    plot_->setLiveOverlay(enabled && !lines.empty(), lines);
}

PathStateList CoverageGUI::computeObstacleAvoidingPath(
    const Point2D& start,
    const Point2D& goal,
    double spacing_override,
    double clearance_override) const {
    if (polygon_.empty()) {
        return {};
    }
    // Connector/home planning should not be clipped by ROI; use full boundary.
    const Polygon2D* roi_ptr = nullptr;
    const std::vector<Obstacle2D>* obs_ptr = obstacles_.empty() ? nullptr : &obstacles_;
    const double spacing = (spacing_override >= 0.0)
        ? spacing_override
        : (spin_waypoint_spacing_ ? spin_waypoint_spacing_->value() : 0.0);
    const double clearance = (clearance_override >= 0.0)
        ? clearance_override
        : (spin_headland_ ? spin_headland_->value() : 0.0);
    return planObstacleAvoidingPath(start, goal, polygon_, roi_ptr, obs_ptr, spacing, clearance);
}

void CoverageGUI::startTransitPathPlanning(const Point2D& start, const Point2D& goal, bool is_home) {
    if (!transit_plan_watcher_) {
        return;
    }
    if (transit_plan_watcher_->isRunning()) {
        setStatus("Path planning already in progress. Please wait...", 3000);
        return;
    }

    transit_plan_kind_ = is_home ? TransitPlanKind::Home : TransitPlanKind::GoTo;
    transit_plan_goal_ = goal;

    const Polygon2D boundary = polygon_;
    const std::vector<Obstacle2D> obstacles_copy = obstacles_;
    const double spacing = spin_waypoint_spacing_ ? spin_waypoint_spacing_->value() : 0.0;
    const double clearance = kConnectorObstacleClearanceM;

    beginProgressOperation(is_home ? "Planning home path..." : "Planning GO TO path...", true, true);
    if (btn_quick_home_) btn_quick_home_->setEnabled(false);
    if (btn_go_to_) btn_go_to_->setEnabled(false);

    auto future = QtConcurrent::run([this, start, goal, boundary, obstacles_copy, spacing, clearance]() -> PathStateList {
        const std::vector<Obstacle2D>* obs_ptr = obstacles_copy.empty() ? nullptr : &obstacles_copy;
        try {
            return planObstacleAvoidingPath(start, goal, boundary, nullptr, obs_ptr, spacing, clearance);
        } catch (const std::exception& e) {
            if (!isOperationCancelledMessage(QString::fromUtf8(e.what()))) {
                std::cerr << "Transit planning error: " << e.what() << std::endl;
            }
            return PathStateList();
        }
    });
    transit_plan_watcher_->setFuture(future);
}

void CoverageGUI::onTransitPathPlanningFinished() {
    const bool cancelled = progress_cancel_requested_.load();
    endProgressOperation();
    if (btn_quick_home_) btn_quick_home_->setEnabled(true);
    if (btn_go_to_) btn_go_to_->setEnabled(true);

    if (!transit_plan_watcher_) {
        transit_plan_kind_ = TransitPlanKind::None;
        return;
    }

    PathStateList planned_path = transit_plan_watcher_->result();
    const TransitPlanKind kind = transit_plan_kind_;
    transit_plan_kind_ = TransitPlanKind::None;

    if (planned_path.empty()) {
        if (cancelled) {
            setStatus(kind == TransitPlanKind::Home
                          ? "Home path planning cancelled"
                          : "GO TO path planning cancelled",
                      5000);
            return;
        }
        if (kind == TransitPlanKind::Home) {
            QMessageBox::warning(
                this, "Home Path",
                QString("Failed to generate a valid path to origin with clearance %1 m.")
                    .arg(kConnectorObstacleClearanceM, 0, 'f', 2));
        } else if (kind == TransitPlanKind::GoTo) {
            QMessageBox::warning(
                this, "GO TO Path",
                QString("Failed to generate a valid path to clicked point with clearance %1 m.")
                    .arg(kConnectorObstacleClearanceM, 0, 'f', 2));
        }
        return;
    }

    clearCoverage();
    path_ = std::move(planned_path);
    path_connector_prefix_count_ = 0;
    // Treat both HOME and GO TO as transit paths for data-collection flags.
    planned_path_is_home_ = true;
    syncPlannedPathCache();
    updateCoverageStats();
    refreshPlot();

    if (kind == TransitPlanKind::Home) {
        setStatus(QString("Home path generated (%1 points, clearance %2 m)")
                      .arg(path_.size())
                      .arg(kConnectorObstacleClearanceM, 0, 'f', 2),
                  6000);
    } else if (kind == TransitPlanKind::GoTo) {
        setStatus(QString("GO TO path generated (%1 points) to (%2, %3)")
                      .arg(path_.size())
                      .arg(transit_plan_goal_.x, 0, 'f', 2)
                      .arg(transit_plan_goal_.y, 0, 'f', 2),
                  6000);
    }
}

// =============================================================================
// Coverage Statistics
// =============================================================================

CoverageStats CoverageGUI::computeStats() const {
    CoverageStats stats;
    
    // Get path to analyze (F2C or custom)
    std::vector<Point2D> path_points;
    if (isCustomModeActive()) {
        path_points = custom_waypoints_;
    } else {
        PathStateList deduped = dedupePathStates(path_);
        for (const auto& state : deduped) {
            path_points.push_back(state.point);
        }
    }
    
    // Compute path length
    if (path_points.size() >= 2) {
        for (size_t i = 1; i < path_points.size(); ++i) {
            stats.path_length_m += std::hypot(
                path_points[i].x - path_points[i-1].x,
                path_points[i].y - path_points[i-1].y);
        }
    }
    
    // Count waypoints
    stats.num_waypoints = static_cast<int>(path_points.size());
    
    // Count swaths and turns
    stats.num_swaths = static_cast<int>(swaths_.size());
    stats.num_turns = std::max(0, stats.num_swaths - 1);
    
    // Compute polygon/ROI area
    if (!isCustomModeActive() && effective_area_m2_ > 0.0) {
        // Use backend-computed effective area: (boundary ∩ ROI) − obstacles
        stats.polygon_area_m2 = effective_area_m2_;
    } else {
    const Polygon2D& field_poly = roi_polygon_.empty() ? polygon_ : roi_polygon_;
    if (!field_poly.empty()) {
        stats.polygon_area_m2 = polygonArea(field_poly);
        }
    }
    
    // Estimate coverage area (swath width × path length)
    double swath_w = spin_swath_ ? spin_swath_->value() : 1.0;
    stats.coverage_area_m2 = stats.path_length_m * swath_w;
    
    // Compute coverage percentage
    if (stats.polygon_area_m2 > 0) {
        stats.coverage_percent = std::min(100.0, 
            (stats.coverage_area_m2 / stats.polygon_area_m2) * 100.0);
    }
    
    // Estimate time based on robot speed
    double speed = spin_robot_speed_ ? spin_robot_speed_->value() : 0.5;
    if (speed > 0) {
        stats.estimated_time_min = (stats.path_length_m / speed) / 60.0;
    }
    
    // Compute overlap (if coverage > field area, there's overlap)
    if (stats.polygon_area_m2 > 0 && stats.coverage_area_m2 > stats.polygon_area_m2) {
        stats.overlap_percent = ((stats.coverage_area_m2 - stats.polygon_area_m2) / 
                                 stats.polygon_area_m2) * 100.0;
    }
    
    return stats;
}

void CoverageGUI::updateCoverageStats() {
    current_stats_ = computeStats();
    
    auto formatValue = [](double val, const QString& unit, int decimals = 1) -> QString {
        if (val <= 0) return "-";
        return QString("%1 %2").arg(val, 0, 'f', decimals).arg(unit);
    };
    
    if (lbl_stats_path_length_) {
        lbl_stats_path_length_->setText(formatValue(current_stats_.path_length_m, "m"));
    }
    if (lbl_stats_area_) {
        lbl_stats_area_->setText(formatValue(current_stats_.coverage_area_m2, "m²"));
    }
    if (lbl_stats_coverage_) {
        QString coverage = formatValue(current_stats_.polygon_area_m2, "m²");
        if (current_stats_.coverage_percent > 0) {
            coverage += QString(" (%1%)").arg(current_stats_.coverage_percent, 0, 'f', 0);
        }
        lbl_stats_coverage_->setText(coverage);
    }
    if (lbl_stats_swaths_) {
        lbl_stats_swaths_->setText(current_stats_.num_swaths > 0 ? 
            QString::number(current_stats_.num_swaths) : "-");
    }
    if (lbl_stats_turns_) {
        lbl_stats_turns_->setText(current_stats_.num_turns > 0 ? 
            QString::number(current_stats_.num_turns) : "-");
    }
    if (lbl_stats_waypoints_) {
        lbl_stats_waypoints_->setText(current_stats_.num_waypoints > 0 ? 
            QString::number(current_stats_.num_waypoints) : "-");
    }
    if (lbl_stats_time_) {
        if (current_stats_.estimated_time_min > 0) {
            int mins = static_cast<int>(current_stats_.estimated_time_min);
            int secs = static_cast<int>((current_stats_.estimated_time_min - mins) * 60);
            lbl_stats_time_->setText(QString("%1:%2")
                .arg(mins).arg(secs, 2, 10, QChar('0')));
        } else {
            lbl_stats_time_->setText("-");
        }
    }
}

// =============================================================================
// Rectangle Drawing Tool
// =============================================================================

void CoverageGUI::toggleRectangleMode() {
    if (btn_rectangle_->isChecked()) {
        // Cancel any other selection modes
        if (btn_roi_) btn_roi_->setChecked(false);
        if (btn_obstacle_) btn_obstacle_->setChecked(false);
        if (btn_measure_ && btn_measure_->isChecked()) {
            btn_measure_->setChecked(false);
            plot_->setMeasureMode(false);
            if (lbl_measure_) {
                lbl_measure_->setText("Distance: -");
            }
        }
        plot_->cancelSelection();
        
        plot_->startRectangleMode();
        setStatus("Rectangle mode: Click first corner");
    } else {
        plot_->cancelRectangleMode();
        setStatus("Rectangle mode cancelled");
    }
}

void CoverageGUI::onRectangleCompleted(const Polygon2D& rect) {
    // Use rectangle as ROI
    roi_polygon_ = rect;
    plot_->setROI(roi_polygon_);
    effective_area_m2_ = 0.0;
    clearCoverage();
    
    if (btn_rectangle_) {
        btn_rectangle_->setChecked(false);
    }
    
    // Compute area for display
    double area = polygonArea(rect);
    lbl_roi_->setText(QString("ROI: rectangle (%1 m²)").arg(area, 0, 'f', 1));
    
    setStatus(QString("Rectangle ROI set (area: %1 m²)").arg(area, 0, 'f', 1), 4000);
    updateCoverageStats();
    refreshPlot();
}

void CoverageGUI::tryReconnectROS2() {
    // Already connected - stop the timer
    if (ros_initialized_) {
        ros_reconnect_timer_->stop();
        return;
    }
    
    std::cout << "[Coverage Planner] Attempting ROS2 reconnection..." << std::endl;
    
    try {
        // Try to initialize ROS2
        ros_node_ = rclcpp::Node::make_shared("bdr_coverage_gui");
        waypoint_pub_ = ros_node_->create_publisher<std_msgs::msg::Float64MultiArray>(
            "/f2c_waypoints", 10);
        setupRobotTrackingSubscription();

        // Start ROS2 spinning in background thread
        ros_thread_ = std::thread([this]() {
            rclcpp::spin(ros_node_);
        });
        
        ros_initialized_ = true;
        
        // Success! Stop the reconnection timer
        ros_reconnect_timer_->stop();
        
        QString bridge_str = zenoh_bridge_detected_ ? "Zenoh bridge active" : "Zenoh bridge not detected";
        setStatus(QString("ROS2 connected (local DDS, %1)").arg(bridge_str), 5000);
        std::cout << "[Coverage Planner] ROS2 reconnection successful!" << std::endl;
        
    } catch (const std::exception& e) {
        // Still not available - timer will try again
        setStatus("ROS2 unavailable - retrying...", 4500);
    }
}

void CoverageGUI::reinitializeROS2() {
    setStatus("Reinitializing ROS2 (CycloneDDS loopback + Zenoh bridge)...");
    
    // Stop reconnection timer if running
    ros_reconnect_timer_->stop();
    
    // Shutdown existing ROS2 connection if any
    if (ros_initialized_) {
        std::cout << "[Coverage Planner] Shutting down ROS2 for reinit..." << std::endl;
        fastlio_sub_.reset();
        
        // Stop the spin thread by shutting down the node's context
        if (ros_node_) {
            rclcpp::shutdown();
            
            // Wait for thread to finish
            if (ros_thread_.joinable()) {
                ros_thread_.join();
            }
            
            // Reset pointers
            waypoint_pub_.reset();
            ros_node_.reset();
        }
        
        ros_initialized_ = false;
        waypoints_published_ = false;
        
        // Reinitialize rclcpp
        // Note: rclcpp::init should be called again after shutdown
        int argc = 0;
        char** argv = nullptr;
        rclcpp::init(argc, argv);
    }
    
    // Ensure CycloneDDS config is set
    if (QFile::exists(dds_config_path_)) {
        qputenv("CYCLONEDDS_URI", dds_config_path_.toUtf8());
    }
    
    try {
        ros_node_ = rclcpp::Node::make_shared("bdr_coverage_gui");
        waypoint_pub_ = ros_node_->create_publisher<std_msgs::msg::Float64MultiArray>(
            "/f2c_waypoints", 10);
        setupRobotTrackingSubscription();

        ros_thread_ = std::thread([this]() {
            rclcpp::spin(ros_node_);
        });
        
        ros_initialized_ = true;
        QString bridge_str = zenoh_bridge_detected_ ? "Zenoh bridge active" : "Zenoh bridge not detected";
        setStatus(QString("ROS2 connected (local DDS, %1)").arg(bridge_str), 5000);
        std::cout << "[Coverage Planner] ROS2 reinitialized (CycloneDDS loopback)" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "[Coverage Planner] ROS2 reinit failed: " << e.what() << std::endl;
        setStatus("ROS2 unavailable - retrying...");
        ros_reconnect_timer_->start();
    }
}

void CoverageGUI::checkZenohBridgeStatus() {
    // Check if the Zenoh bridge daemon (zenohd) is running
    // The bridge is managed by laptop_teleop.launch.py, not by this application
    QProcess proc;
    proc.start("pgrep", QStringList() << "-x" << "zenohd");
    proc.waitForFinished(1000);
    
    bool was_detected = zenoh_bridge_detected_;
    zenoh_bridge_detected_ = (proc.exitCode() == 0);
    
    // Update UI label
    if (lbl_zenoh_status_) {
        if (zenoh_bridge_detected_) {
            lbl_zenoh_status_->setText("Zenoh Bridge: running (Microhard RF) ✓");
            lbl_zenoh_status_->setStyleSheet("color: green; font-size: 10px;");
        } else {
            lbl_zenoh_status_->setText("Zenoh Bridge: not running (start laptop_teleop.launch.py)");
            lbl_zenoh_status_->setStyleSheet("color: orange; font-size: 10px;");
        }
    }
    
    // Log state transitions
    if (zenoh_bridge_detected_ && !was_detected) {
        std::cout << "[Coverage Planner] Zenoh bridge detected - robot communication available" << std::endl;
        setStatus("Zenoh bridge connected - robot communication active", 5000);
    } else if (!zenoh_bridge_detected_ && was_detected) {
        std::cerr << "[Coverage Planner] Zenoh bridge lost - robot communication unavailable" << std::endl;
        std::cerr << "[Coverage Planner] Start with: ros2 launch pilot_control laptop_teleop.launch.py" << std::endl;
        setStatus("⚠ Zenoh bridge disconnected - robot comms unavailable", 8000);
    }
}

// =============================================================================
// Preset Management
// =============================================================================

QWidget* CoverageGUI::buildPresetControls() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    
    QLabel* title = new QLabel("Presets");
    title->setStyleSheet("font-weight: bold;");
    layout->addWidget(title);
    
    // Initialize preset manager if needed
    if (!preset_manager_) {
        preset_manager_ = new PresetManager(this);
        connect(preset_manager_, &PresetManager::presetsChanged, this, &CoverageGUI::refreshPresetList);
        connect(preset_manager_, &PresetManager::error, this, [this](const QString& msg) {
            QMessageBox::warning(this, "Preset Error", msg);
        });
    }
    
    // Preset dropdown
    QHBoxLayout* comboLayout = new QHBoxLayout();
    combo_preset_ = new QComboBox();
    combo_preset_->setMinimumWidth(120);
    combo_preset_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(combo_preset_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CoverageGUI::onPresetSelected);
    comboLayout->addWidget(combo_preset_);
    
    // Save button
    btn_save_preset_ = new QPushButton("💾");
    btn_save_preset_->setFixedSize(28, 28);
    btn_save_preset_->setToolTip("Save current settings to selected preset");
    connect(btn_save_preset_, &QPushButton::clicked, this, &CoverageGUI::saveCurrentPreset);
    comboLayout->addWidget(btn_save_preset_);
    
    layout->addLayout(comboLayout);
    
    // Action buttons
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(4);
    
    btn_new_preset_ = new QPushButton("+ New");
    btn_new_preset_->setToolTip("Create a new preset with current settings");
    connect(btn_new_preset_, &QPushButton::clicked, this, &CoverageGUI::createNewPreset);
    btnLayout->addWidget(btn_new_preset_);
    
    btn_manage_presets_ = new QPushButton("⚙ Manage");
    btn_manage_presets_->setToolTip("Open preset manager (rename, delete, import/export)");
    connect(btn_manage_presets_, &QPushButton::clicked, this, &CoverageGUI::openPresetManager);
    btnLayout->addWidget(btn_manage_presets_);
    
    layout->addLayout(btnLayout);
    
    // Populate preset list
    refreshPresetList();
    
    return widget;
}

void CoverageGUI::refreshPresetList() {
    if (!combo_preset_ || !preset_manager_) return;
    
    QString current = combo_preset_->currentText();
    
    combo_preset_->blockSignals(true);
    combo_preset_->clear();
    
    // Add "Default" as first item
    combo_preset_->addItem("(Default)", "");
    
    // Add saved presets
    QStringList presets = preset_manager_->availablePresets();
    for (const QString& name : presets) {
        combo_preset_->addItem(name, name);
    }
    
    // Restore selection
    int idx = combo_preset_->findText(current);
    if (idx >= 0) {
        combo_preset_->setCurrentIndex(idx);
    }
    
    combo_preset_->blockSignals(false);
    
    // Enable/disable save button based on selection
    bool hasSelection = combo_preset_->currentIndex() > 0;
    btn_save_preset_->setEnabled(hasSelection);
}

void CoverageGUI::onPresetSelected(int index) {
    if (index <= 0) {
        // Default selected - don't load anything, just enable "New" only
        btn_save_preset_->setEnabled(false);
        return;
    }
    
    btn_save_preset_->setEnabled(true);
    
    QString name = combo_preset_->currentData().toString();
    if (!name.isEmpty()) {
        loadPreset(name);
    }
}

void CoverageGUI::loadPreset(const QString& name) {
    if (!preset_manager_) return;
    
    PlanningPreset preset = preset_manager_->loadPreset(name);
    if (preset.isValid()) {
        applyPreset(preset);
        setStatus(QString("Loaded preset: %1").arg(name));
    }
}

void CoverageGUI::saveCurrentPreset() {
    if (!preset_manager_ || !combo_preset_) return;
    
    QString name = combo_preset_->currentData().toString();
    if (name.isEmpty()) {
        // No preset selected, prompt for new
        createNewPreset();
        return;
    }
    
    // Confirm overwrite
    int result = QMessageBox::question(this, "Save Preset",
        QString("Overwrite preset '%1' with current settings?").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    
    if (result != QMessageBox::Yes) return;
    
    PlanningPreset preset = gatherCurrentSettings();
    preset.name = name;
    
    // Preserve original creation date
    PlanningPreset existing = preset_manager_->loadPreset(name);
    if (existing.isValid()) {
        preset.created = existing.created;
    }
    
    if (preset_manager_->savePreset(preset)) {
        setStatus(QString("Saved preset: %1").arg(name));
    }
}

void CoverageGUI::createNewPreset() {
    if (!preset_manager_) return;
    
    QStringList existing = preset_manager_->availablePresets();
    NewPresetDialog dialog(existing, this);
    
    if (dialog.exec() == QDialog::Accepted) {
        QString name = dialog.presetName();
        
        PlanningPreset preset = gatherCurrentSettings();
        preset.name = name;
        preset.created = QDateTime::currentDateTime();
        
        if (preset_manager_->savePreset(preset)) {
            // Select the new preset
            int idx = combo_preset_->findText(name);
            if (idx >= 0) {
                combo_preset_->setCurrentIndex(idx);
            }
            setStatus(QString("Created preset: %1").arg(name));
        }
    }
}

void CoverageGUI::openPresetManager() {
    if (!preset_manager_) return;
    
    PresetManagerDialog dialog(preset_manager_, this);
    connect(&dialog, &PresetManagerDialog::presetLoadRequested,
            this, &CoverageGUI::loadPreset);
    
    dialog.exec();
    
    // Refresh combo in case presets were deleted/renamed
    refreshPresetList();
}

PlanningPreset CoverageGUI::gatherCurrentSettings() const {
    PlanningPreset preset;
    
    // Filtering
    if (spin_z_min_) preset.z_min = spin_z_min_->value();
    if (spin_z_max_) preset.z_max = spin_z_max_->value();
    if (combo_downsample_) preset.downsample_method = combo_downsample_->currentText().toLower();
    if (spin_max_points_) preset.max_points = spin_max_points_->value();
    if (spin_voxel_) preset.voxel_size = spin_voxel_->value();
    if (spin_mean_k_) preset.mean_k = spin_mean_k_->value();
    if (spin_std_ratio_) preset.std_ratio = spin_std_ratio_->value();
    
    // Hull
    if (combo_hull_method_) preset.hull_method = combo_hull_method_->currentData().toString();
    if (spin_alpha_) preset.alpha = spin_alpha_->value();
    if (spin_simplify_) preset.simplify_tolerance = spin_simplify_->value();
    
    // Coverage
    if (spin_swath_) preset.swath_width = spin_swath_->value();
    if (spin_headland_) preset.headland_width = spin_headland_->value();
    if (spin_turn_) preset.turn_radius = spin_turn_->value();
    if (chk_auto_align_) preset.auto_align = chk_auto_align_->isChecked();
    if (radio_long_) preset.direction = radio_long_->isChecked() ? "longest" : "perpendicular";
    if (combo_route_pattern_) preset.route_pattern = combo_route_pattern_->currentData().toString();
    if (combo_path_planner_) preset.path_planner = combo_path_planner_->currentData().toString();
    if (chk_decomposition_) preset.decomposition = chk_decomposition_->isChecked();
    if (combo_decomp_type_) preset.decomp_type = combo_decomp_type_->currentData().toString();
    if (chk_axial_turns_) preset.axial_turns = chk_axial_turns_->isChecked();
    if (spin_waypoint_spacing_) preset.waypoint_spacing = spin_waypoint_spacing_->value();
    
    // Execution
    if (spin_robot_speed_) preset.robot_speed = spin_robot_speed_->value();
    
    return preset;
}

void CoverageGUI::toggleTeleopWidget() {
    // Create teleop dock lazily if it doesn't exist yet
    if (!teleop_dock_) {
        teleop_dock_ = new TeleopDockWidget(ros_node_, this);
        teleop_dock_->setFloating(true);
        addDockWidget(Qt::RightDockWidgetArea, teleop_dock_);
        connect(teleop_dock_->teleopWidget(), &TeleopWidget::statusMessage,
                this, &CoverageGUI::onTeleopStatusMessage);
    }
    
    if (teleop_dock_->isVisible()) {
        teleop_dock_->hide();
    } else {
        teleop_dock_->show();
        teleop_dock_->raise();
        teleop_dock_->teleopWidget()->setFocus();
    }
}

void CoverageGUI::onTeleopStatusMessage(const QString& message) {
    setStatus(message);
}

// =============================================================================
// Cloud Upload
// =============================================================================

void CoverageGUI::onCloudUploadActive(bool active) {
    if (active) {
        setStatus("Cloud upload in progress...");
        if (btn_open_transfer_dialog_) {
            btn_open_transfer_dialog_->setText("📥 Data Transfer (uploading...)");
        }
    } else {
        setStatus("Cloud upload completed");
        if (btn_open_transfer_dialog_) {
            btn_open_transfer_dialog_->setText("📥 Data Transfer");
        }
    }
}

// =============================================================================
// Scan Session Tracking
// =============================================================================

void CoverageGUI::startScanSession(const QString& sectionName) {
    if (!scan_session_tracker_) return;
    
    double swathWidth = spin_swath_ ? spin_swath_->value() : 1.0;
    
    // Determine pattern type from UI
    QString patternType = "boustrophedon";
    if (combo_route_pattern_) {
        patternType = combo_route_pattern_->currentText();
    }
    
    const QString rid = session_robot_id_.isEmpty() ? active_robot_id_ : session_robot_id_;
    scan_session_tracker_->startSession(sectionName, rid, activeRobotSlug(),
                                        current_stats_, swathWidth, patternType);
}

void CoverageGUI::endScanSession() {
    if (!scan_session_tracker_ || !scan_session_tracker_->isSessionActive()) return;
    
    // Gather actual execution stats
    double durationSec = 0.0;
    double avgSpeed = 0.0;
    double totalTraveled = 0.0;
    
    if (mission_start_time_ != std::chrono::steady_clock::time_point{}) {
        auto now = std::chrono::steady_clock::now();
        durationSec = std::chrono::duration<double>(now - mission_start_time_).count();
    }
    
    totalTraveled = live_traveled_m_;
    
    if (durationSec > 0) {
        avgSpeed = totalTraveled / durationSec;
    }
    
    scan_session_tracker_->endSession(durationSec, avgSpeed, totalTraveled);
}

void CoverageGUI::applyPreset(const PlanningPreset& preset) {
    // Block signals during bulk update
    QList<QWidget*> widgets = {
        spin_z_min_, spin_z_max_, combo_downsample_, spin_max_points_,
        spin_voxel_, spin_mean_k_, spin_std_ratio_, combo_hull_method_,
        spin_alpha_, spin_simplify_, spin_swath_, spin_headland_,
        spin_turn_, chk_auto_align_, radio_long_, radio_perp_,
        combo_route_pattern_, combo_path_planner_, chk_decomposition_,
        combo_decomp_type_, chk_axial_turns_, spin_waypoint_spacing_,
        spin_robot_speed_
    };
    
    for (QWidget* w : widgets) {
        if (w) w->blockSignals(true);
    }
    
    // Filtering
    if (spin_z_min_) spin_z_min_->setValue(preset.z_min);
    if (spin_z_max_) spin_z_max_->setValue(preset.z_max);
    if (combo_downsample_) {
        int idx = combo_downsample_->findText(preset.downsample_method, Qt::MatchFixedString);
        if (idx >= 0) combo_downsample_->setCurrentIndex(idx);
    }
    if (spin_max_points_) spin_max_points_->setValue(preset.max_points);
    if (spin_voxel_) spin_voxel_->setValue(preset.voxel_size);
    if (spin_mean_k_) spin_mean_k_->setValue(preset.mean_k);
    if (spin_std_ratio_) spin_std_ratio_->setValue(preset.std_ratio);
    
    // Hull
    if (combo_hull_method_) {
        int idx = combo_hull_method_->findData(preset.hull_method);
        if (idx >= 0) combo_hull_method_->setCurrentIndex(idx);
    }
    if (spin_alpha_) spin_alpha_->setValue(preset.alpha);
    if (spin_simplify_) spin_simplify_->setValue(preset.simplify_tolerance);
    
    // Coverage
    if (spin_swath_) spin_swath_->setValue(preset.swath_width);
    if (spin_headland_) spin_headland_->setValue(preset.headland_width);
    if (spin_turn_) spin_turn_->setValue(preset.turn_radius);
    if (chk_auto_align_) chk_auto_align_->setChecked(preset.auto_align);
    if (radio_long_ && radio_perp_) {
        if (preset.direction == "longest") {
            radio_long_->setChecked(true);
        } else {
            radio_perp_->setChecked(true);
        }
    }
    if (combo_route_pattern_) {
        int idx = combo_route_pattern_->findData(preset.route_pattern);
        if (idx >= 0) combo_route_pattern_->setCurrentIndex(idx);
    }
    if (combo_path_planner_) {
        int idx = combo_path_planner_->findData(preset.path_planner);
        if (idx >= 0) combo_path_planner_->setCurrentIndex(idx);
    }
    if (chk_decomposition_) chk_decomposition_->setChecked(preset.decomposition);
    if (combo_decomp_type_) {
        int idx = combo_decomp_type_->findData(preset.decomp_type);
        if (idx >= 0) combo_decomp_type_->setCurrentIndex(idx);
    }
    if (chk_axial_turns_) chk_axial_turns_->setChecked(preset.axial_turns);
    if (spin_waypoint_spacing_) spin_waypoint_spacing_->setValue(preset.waypoint_spacing);
    
    // Execution
    if (spin_robot_speed_) spin_robot_speed_->setValue(preset.robot_speed);
    
    // Unblock signals
    for (QWidget* w : widgets) {
        if (w) w->blockSignals(false);
    }
    
    // Update UI that depends on these values
    if (combo_downsample_) {
        updateDownsampleUI(combo_downsample_->currentText());
    }
}

// =============================================================================
// Robot login/session (in-memory only)
// =============================================================================

bool CoverageGUI::hasValidLoginSession() const {
    if (session_token_.isEmpty()) return false;
    if (session_robot_id_.isEmpty()) return false;
    if (!session_has_expiry_) return true;
    if (!session_expires_at_.isValid()) return false;
    return QDateTime::currentDateTimeUtc() < session_expires_at_.toUTC();
}

void CoverageGUI::clearLoginSession(const QString& reason) {
    session_robot_id_.clear();
    session_token_.clear();
    session_expires_at_ = QDateTime();
    session_has_expiry_ = false;
    session_scopes_.clear();

    if (login_countdown_timer_) {
        login_countdown_timer_->stop();
    }

    if (!reason.isEmpty()) {
        setStatus(reason, 4000);
    }

    updateLoginUi();
}

void CoverageGUI::updatePrivilegedUiState() {
    const bool authed = hasValidLoginSession();

    if (btn_publish_waypoints_) btn_publish_waypoints_->setEnabled(authed);
    if (btn_quick_publish_) btn_quick_publish_->setEnabled(authed);
    if (btn_publish_segments_) btn_publish_segments_->setEnabled(authed);
    if (btn_start_segments_) btn_start_segments_->setEnabled(authed);

    if (!authed) {
        if (btn_start_navigation_) btn_start_navigation_->setEnabled(false);
        if (btn_quick_start_) btn_quick_start_->setEnabled(false);
    } else {
        // Keep start buttons gated by whether waypoints have been published.
        if (waypoints_published_) {
            if (btn_start_navigation_) btn_start_navigation_->setEnabled(true);
            if (btn_quick_start_) btn_quick_start_->setEnabled(true);
        }
    }
}

void CoverageGUI::updateLoginUi() {
    const bool authed = hasValidLoginSession();

    if (btn_robot_login_) {
        btn_robot_login_->setText(authed ? "Logout" : "Login");
    }
    if (combo_robot_id_) {
        combo_robot_id_->setEnabled(!authed);
    }
    if (txt_access_code_) {
        txt_access_code_->setEnabled(!authed);
        if (authed) {
            txt_access_code_->clear();
        }
    }

    if (lbl_login_status_) {
        if (authed) {
            lbl_login_status_->setText(QString("Logged in (%1)").arg(session_robot_id_));
            lbl_login_status_->setStyleSheet("color: #2e7d32; font-size: 10px;");
        } else {
            lbl_login_status_->setText("Not logged in");
            lbl_login_status_->setStyleSheet("color: #666; font-size: 10px;");
        }
    }

    if (lbl_login_expiry_) {
        if (authed) {
            if (session_has_expiry_) {
                qint64 secs = QDateTime::currentDateTimeUtc().secsTo(session_expires_at_.toUTC());
                if (secs < 0) secs = 0;
                const int mm = static_cast<int>(secs / 60);
                const int ss = static_cast<int>(secs % 60);
                lbl_login_expiry_->setText(QString("Session expires in %1:%2")
                                               .arg(mm, 2, 10, QChar('0'))
                                               .arg(ss, 2, 10, QChar('0')));
            } else {
                lbl_login_expiry_->setText("Session does not expire");
            }
        } else {
            lbl_login_expiry_->clear();
        }
    }

    updatePrivilegedUiState();
}

bool CoverageGUI::loginToRobotOverSsh(const QString& robotId, const QString& pin, QString* errorOut) {
    if (robot_host_.isEmpty() || robot_user_.isEmpty()) {
        if (errorOut) *errorOut = "Robot connection profile is not set.";
        return false;
    }

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);

    QStringList args;
    args << "-o" << "ConnectTimeout=8"
         << "-o" << "BatchMode=yes";

    if (!pinned_known_hosts_file_.isEmpty()) {
        args << "-o" << "StrictHostKeyChecking=yes"
             << "-o" << QString("UserKnownHostsFile=%1").arg(pinned_known_hosts_file_)
             << "-o" << "GlobalKnownHostsFile=/dev/null";
    } else {
        args << "-o" << "StrictHostKeyChecking=no";
    }

    args << QString("%1@%2").arg(robot_user_, robot_host_)
         << "pilot_control_auth"
         << "login"
         << "--robot-id" << robotId
         << "--pin-stdin"
         << "--json";

    proc.start("ssh", args);
    if (!proc.waitForStarted(3000)) {
        if (errorOut) *errorOut = "Failed to start ssh process.";
        return false;
    }

    // Send PIN via stdin (avoid exposing it in process list)
    proc.write(pin.toUtf8());
    proc.write("\n");
    proc.closeWriteChannel();

    if (!proc.waitForFinished(12000)) {
        proc.kill();
        proc.waitForFinished(3000);
        if (errorOut) *errorOut = "Login timed out.";
        return false;
    }

    const QByteArray out = proc.readAllStandardOutput();
    const QByteArray err = proc.readAllStandardError();

    if (proc.exitCode() != 0) {
        QString msg = QString::fromUtf8(err).trimmed();
        if (msg.isEmpty()) msg = QString::fromUtf8(out).trimmed();
        if (msg.isEmpty()) msg = QString("ssh exited with code %1").arg(proc.exitCode());
        if (errorOut) *errorOut = msg;
        return false;
    }

    QJsonParseError jerr{};
    QJsonDocument doc = QJsonDocument::fromJson(out, &jerr);
    if (doc.isNull() || !doc.isObject()) {
        if (errorOut) {
            *errorOut = QString("Invalid JSON from robot auth (offset %1): %2")
                            .arg(jerr.offset)
                            .arg(jerr.errorString());
        }
        return false;
    }

    const QJsonObject obj = doc.object();
    const QString out_robot_id = obj.value("robot_id").toString().trimmed();
    const QString token = obj.value("token").toString().trimmed();
    const QString expires_at = obj.value("expires_at").toString().trimmed();
    const bool expires_never = obj.value("expires_never").toBool(false);

    if (token.isEmpty()) {
        if (errorOut) *errorOut = "Robot auth response missing token.";
        return false;
    }

    QDateTime exp;
    bool has_expiry = false;
    if (!expires_never && !expires_at.isEmpty()) {
        exp = QDateTime::fromString(expires_at, Qt::ISODate);
        if (!exp.isValid()) {
            // Some producers emit ISO8601 with milliseconds; try that too.
            exp = QDateTime::fromString(expires_at, Qt::ISODateWithMs);
        }
        if (!exp.isValid()) {
            if (errorOut) *errorOut = "Robot auth response has invalid expires_at timestamp.";
            return false;
        }
        has_expiry = true;
    }

    QStringList scopes;
    if (obj.value("scopes").isArray()) {
        const QJsonArray arr = obj.value("scopes").toArray();
        for (const auto& v : arr) {
            if (v.isString()) scopes << v.toString();
        }
    }

    // Save session (in-memory only)
    session_robot_id_ = out_robot_id.isEmpty() ? robotId : out_robot_id;
    session_token_ = token;
    session_has_expiry_ = has_expiry;
    session_expires_at_ = has_expiry ? exp.toUTC() : QDateTime();
    session_scopes_ = scopes;

    return true;
}

bool CoverageGUI::uploadMissionCsvToRobot(const PathStateList& exportPath, QString* remoteCsvPathOut, QString* errorOut) {
    if (robot_host_.isEmpty() || robot_user_.isEmpty()) {
        if (errorOut) *errorOut = "Robot connection profile is not set.";
        return false;
    }
    if (exportPath.size() < 2) {
        if (errorOut) *errorOut = "Export path is empty.";
        return false;
    }

    // Save to a unique local temp CSV
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
    const QString localCsv = QDir::tempPath() + QString("/pilot_control_waypoints_%1_%2.csv")
                                 .arg(activeRobotSlug(), stamp);
    if (!savePathToCSV(exportPath, localCsv.toStdString())) {
        if (errorOut) *errorOut = "Failed to write local CSV.";
        return false;
    }

    // Choose remote upload directory
    QString remoteDir;
    if (active_robot_.has_value() && !active_robot_->default_remote_upload_dir.trimmed().isEmpty()) {
        remoteDir = active_robot_->default_remote_upload_dir.trimmed();
    } else {
        remoteDir = robot_data_path_.trimmed() + "/waypoints";
    }
    if (!remoteDir.startsWith('/')) {
        remoteDir = "/" + remoteDir;
    }

    const QString remoteCsv = remoteDir + QString("/waypoints_%1_%2.csv").arg(activeRobotSlug(), stamp);

    // Build SSH/SCP options (use pinning if present)
    QStringList sshArgs;
    sshArgs << "-o" << "ConnectTimeout=8"
            << "-o" << "BatchMode=yes";
    if (!pinned_known_hosts_file_.isEmpty()) {
        sshArgs << "-o" << "StrictHostKeyChecking=yes"
                << "-o" << QString("UserKnownHostsFile=%1").arg(pinned_known_hosts_file_)
                << "-o" << "GlobalKnownHostsFile=/dev/null";
    } else {
        sshArgs << "-o" << "StrictHostKeyChecking=no";
    }
    const QString userHost = QString("%1@%2").arg(robot_user_, robot_host_);

    // Ensure remote dir exists
    {
        QProcess mkdirProc;
        mkdirProc.setProcessChannelMode(QProcess::SeparateChannels);
        mkdirProc.start("ssh", sshArgs + QStringList() << userHost << "mkdir" << "-p" << remoteDir);
        if (!mkdirProc.waitForFinished(12000) || mkdirProc.exitCode() != 0) {
            const QString err = QString::fromUtf8(mkdirProc.readAllStandardError()).trimmed();
            if (errorOut) *errorOut = err.isEmpty() ? "Failed to create remote upload directory." : err;
            return false;
        }
    }

    // Upload CSV
    {
        QProcess scpProc;
        scpProc.setProcessChannelMode(QProcess::SeparateChannels);

        QStringList scpArgs;
        scpArgs << "-o" << "ConnectTimeout=10"
                << "-o" << "BatchMode=yes";
        if (!pinned_known_hosts_file_.isEmpty()) {
            scpArgs << "-o" << "StrictHostKeyChecking=yes"
                    << "-o" << QString("UserKnownHostsFile=%1").arg(pinned_known_hosts_file_)
                    << "-o" << "GlobalKnownHostsFile=/dev/null";
        } else {
            scpArgs << "-o" << "StrictHostKeyChecking=no";
        }

        const QString remoteTarget = QString("%1:%2").arg(userHost, remoteCsv);
        scpProc.start("scp", scpArgs + QStringList() << localCsv << remoteTarget);

        if (!scpProc.waitForFinished(180000) || scpProc.exitCode() != 0) {
            const QString err = QString::fromUtf8(scpProc.readAllStandardError()).trimmed();
            if (errorOut) *errorOut = err.isEmpty() ? "CSV upload failed." : err;
            return false;
        }
    }

    if (remoteCsvPathOut) *remoteCsvPathOut = remoteCsv;
    return true;
}

void CoverageGUI::onRobotIdChanged(const QString& robotId) {
    if (hasValidLoginSession()) {
        // Robot selection is disabled while logged in, but guard anyway.
        return;
    }

    const QString rid = robotId.trimmed();
    if (rid.isEmpty()) return;

    // Only apply when it matches a registry entry (avoid switching mid-typing).
    if (robot_registry_.isLoaded() && !robot_registry_.isEmpty()) {
        if (robot_registry_.findById(rid).has_value()) {
            setActiveRobotId(rid, true);
            applyActiveRobotProfile(true);
        }
    }
}

void CoverageGUI::onRobotLoginClicked() {
    if (hasValidLoginSession()) {
        clearLoginSession("Logged out.");
        return;
    }

    const QString robotId = combo_robot_id_ ? combo_robot_id_->currentText().trimmed() : QString();
    const QString pin = txt_access_code_ ? txt_access_code_->text().trimmed() : QString();

    if (robotId.isEmpty()) {
        QMessageBox::warning(this, "Robot ID Required", "Select a robot ID.");
        return;
    }
    if (pin.size() != 6) {
        QMessageBox::warning(this, "Invalid Access Code", "Access code must be exactly 6 digits.");
        return;
    }

    if (!robot_registry_.isLoaded() || robot_registry_.isEmpty()) {
        const QString detail = !robot_registry_error_.trimmed().isEmpty()
                                   ? robot_registry_error_.trimmed()
                                   : (robot_registry_.isEmpty()
                                          ? QString("robots.json did not contain any robot entries.")
                                          : QString("robots.json could not be located."));
        QMessageBox::warning(this, "Robot Registry Unavailable",
                             QString("Unable to load robot profiles for login.\n\n%1").arg(detail));
        return;
    }

    // Resolve robot profile (robot_id -> host/user/path) without exposing IP
    if (!setActiveRobotId(robotId, true)) {
        QMessageBox::warning(this, "Unknown Robot", "Robot ID was not found in robots.json.");
        return;
    }
    applyActiveRobotProfile(true);

    const QString resolved_robot_id = active_robot_id_.isEmpty() ? robotId : active_robot_id_;

    if (btn_robot_login_) btn_robot_login_->setEnabled(false);
    setStatus(QString("Logging into %1...").arg(resolved_robot_id), 0);

    QString err;
    const bool ok = loginToRobotOverSsh(resolved_robot_id, pin, &err);

    if (btn_robot_login_) btn_robot_login_->setEnabled(true);

    if (!ok) {
        clearLoginSession("");
        QMessageBox::warning(this, "Login Failed", err);
        return;
    }

    if (login_countdown_timer_ && session_has_expiry_) login_countdown_timer_->start();
    updateLoginUi();
    setStatus(QString("✅ Logged in to %1").arg(session_robot_id_), 4000);
}

void CoverageGUI::onLoginCountdownTick() {
    if (!session_has_expiry_) {
        if (login_countdown_timer_) login_countdown_timer_->stop();
        return;
    }
    if (!hasValidLoginSession()) {
        clearLoginSession("Session expired. Logged out.");
        return;
    }
    updateLoginUi();
}

// =============================================================================
// Robot registry / active robot selection
// =============================================================================

void CoverageGUI::loadRobotRegistry() {
    robot_registry_error_.clear();
    QString err;
    if (!robot_registry_.load(&err)) {
        robot_registry_error_ = err;
        qWarning() << "[CoverageGUI] Robot registry not loaded:" << err;
        return;
    }
    qDebug() << "[CoverageGUI] Loaded robot registry from" << robot_registry_.sourcePath()
             << "robots:" << robot_registry_.robotIds();
}

QString CoverageGUI::activeRobotSlug() const {
    const QString slug = active_robot_slug_.trimmed();
    if (!slug.isEmpty()) return slug;

    const QString rid = active_robot_id_.trimmed();
    if (!rid.isEmpty()) return RobotRegistry::slugifyRobotId(rid);

    return "robot";
}

QString CoverageGUI::ensurePinnedKnownHostsFile(QString* errorOut) {
    pinned_known_hosts_file_.clear();

    if (!active_robot_.has_value()) {
        return QString();
    }

    const QString entry = active_robot_->known_hosts_entry.trimmed();
    if (entry.isEmpty()) {
        return QString();
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath() + "/.config/PilotControl/BDRCoveragePlanner";
    }

    if (!QDir().mkpath(dir)) {
        if (errorOut) *errorOut = QString("Failed to create config directory: %1").arg(dir);
        return QString();
    }

    const QString path = dir + "/known_hosts_" + activeRobotSlug();

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = QString("Failed to write known_hosts file: %1").arg(path);
        return QString();
    }

    QByteArray data = entry.toUtf8();
    f.write(data);
    if (!entry.endsWith('\n')) {
        f.write("\n");
    }
    f.close();

    pinned_known_hosts_file_ = path;
    return path;
}

bool CoverageGUI::setActiveRobotId(const QString& robotId, bool persist) {
    const QString rid = robotId.trimmed();
    if (rid.isEmpty()) return false;

    auto robot = robot_registry_.findById(rid);
    if (!robot.has_value()) {
        qWarning() << "[CoverageGUI] Robot id not found in registry:" << rid;
        return false;
    }

    const QString prev_robot_id = active_robot_id_;
    active_robot_ = *robot;
    active_robot_id_ = active_robot_->robot_id;
    active_robot_slug_ = active_robot_->robot_id_slug.trimmed().isEmpty()
                             ? RobotRegistry::slugifyRobotId(active_robot_id_)
                             : active_robot_->robot_id_slug.trimmed();

    if (combo_robot_id_) {
        QSignalBlocker blocker(combo_robot_id_);
        const int idx = combo_robot_id_->findText(active_robot_id_, Qt::MatchFixedString);
        if (idx >= 0) {
            combo_robot_id_->setCurrentIndex(idx);
        } else {
            combo_robot_id_->setEditText(active_robot_id_);
        }
    }

    if (persist) {
        QSettings settings("PilotControl", "BDRCoveragePlanner");
        settings.setValue("robot_id", active_robot_id_);
    }

    if (prev_robot_id != active_robot_id_) {
        // Avoid cross-robot command/path reuse
        waypoints_published_ = false;
        last_uploaded_csv_remote_path_.clear();
    }

    applyActiveRobotProfile(true);
    return true;
}

void CoverageGUI::applyActiveRobotProfile(bool updateUi) {
    if (active_robot_.has_value()) {
        robot_host_ = active_robot_->host;
        robot_user_ = active_robot_->ssh_user;
        robot_data_path_ = active_robot_->robot_data_path;
    }

    // Always include slug to avoid cross-robot collisions on disk.
    local_map_base_ = QDir::homePath() + "/Roofus_maps/" + activeRobotSlug();

    // Prepare per-robot pinned known_hosts file (if present in registry) for SSH operations
    QString khErr;
    ensurePinnedKnownHostsFile(&khErr);
    if (!khErr.isEmpty()) {
        qWarning() << "[CoverageGUI] Host-key pinning disabled:" << khErr;
        pinned_known_hosts_file_.clear();
    }
    TransferManager::instance().setKnownHostsFile(pinned_known_hosts_file_);

    if (!updateUi) return;

    if (lbl_active_robot_) {
        lbl_active_robot_->setText(active_robot_id_.isEmpty() ? "(not set)" : active_robot_id_);
    }

    // Keep the legacy IP field in sync (and non-editable when using registry).
    if (txt_robot_ip_) {
        txt_robot_ip_->setText(robot_host_);
        const bool using_registry = robot_registry_.isLoaded() && !robot_registry_.isEmpty();
        txt_robot_ip_->setReadOnly(using_registry);
        txt_robot_ip_->setEnabled(!using_registry);
        if (using_registry) {
            txt_robot_ip_->setToolTip("Managed by robots.json (static IP hidden from operator)");
        }
    }

    // Update any already-created transfer/upload dialogs
    const QString robotLocalDataRoot = QDir::homePath() + "/robot_data/" + activeRobotSlug();
    if (data_transfer_dialog_) {
        data_transfer_dialog_->setRobotHost(robot_host_);
        data_transfer_dialog_->setRobotUser(robot_user_);
        data_transfer_dialog_->setDataPath(robot_data_path_);
        data_transfer_dialog_->setRobotDisplayName(active_robot_id_);
        data_transfer_dialog_->setRobotIdSlug(activeRobotSlug());
        data_transfer_dialog_->setDefaultDestination(robotLocalDataRoot);
    }
    if (cloud_upload_dialog_) {
        cloud_upload_dialog_->setLocalDataPath(robotLocalDataRoot);
    }
}

} // namespace f2c_cpp

