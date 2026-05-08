/**
 * @file coverage_gui.hpp
 * @brief Qt6 GUI for coverage planning
 * 
 * This is the C++ equivalent of f2c_gui.py
 */

#pragma once

#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QProgressBar>
#include <QStatusBar>
#include <QScrollArea>
#include <QListWidget>
#include <QSplitter>
#include <QFileDialog>
#include "components/bdr_message_box.hpp"
#include <QProcess>
#include <QDir>
#include <QDate>
#include <QSettings>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPaintEvent>
#include <QTimer>
#include <QAction>
#include <QShortcut>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QToolBox>
#include <QButtonGroup>
#include <QNetworkInterface>
#include <QString>
#include <QSplitter>
#include <QStackedWidget>
#include <QToolButton>
#include <QAbstractItemView>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/float64_multi_array.hpp>
#include <std_msgs/msg/string.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <thread>
#include <optional>
#include <mutex>
#include <chrono>
#include <atomic>

// GStreamer for video streaming
#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include "coverage_pipeline.hpp"
#include "obstacle_detector.hpp"
#include "transfer_manager.hpp"
#include "data_transfer_dialog.hpp"
#include "preset_manager.hpp"
// preset_dialog.hpp deleted in the preset UX rework; legacy openPresetManager
// in coverage_gui.cpp is now a no-op stub (kept for reference, never called).
#include "teleop_widget.hpp"
#include "cloud_upload_dialog.hpp"
#include "scan_session_tracker.hpp"

namespace f2c_cpp {

// =============================================================================
// Reprojection Error Line (waypoint vs traversed)
// =============================================================================

struct ReprojectionLine {
    Point2D waypoint;      // The target waypoint
    Point2D traversed;     // Closest traversed point
    double error_m;        // Error in meters
    int waypoint_index;    // Index in the path
};

// =============================================================================
// Coverage Statistics
// =============================================================================

struct CoverageStats {
    double path_length_m = 0;       // Total path length in meters
    double coverage_area_m2 = 0;    // Covered area in square meters
    double polygon_area_m2 = 0;     // Field/ROI area
    double coverage_percent = 0;    // Coverage percentage
    int num_swaths = 0;             // Number of swaths
    int num_turns = 0;              // Number of turns
    int num_waypoints = 0;          // Number of waypoints
    double estimated_time_min = 0;  // Estimated time at given speed
    double overlap_percent = 0;     // Swath overlap percentage
    
    bool isValid() const { return path_length_m > 0; }
};

// =============================================================================
// Video Stream Widget (GStreamer embedded)
// =============================================================================

class VideoStreamWidget : public QWidget {
    Q_OBJECT

public:
    explicit VideoStreamWidget(QWidget* parent = nullptr);
    ~VideoStreamWidget();
    
    void startStream(int port);
    void stopStream();
    bool isPlaying() const { return playing_; }
    int currentPort() const { return current_port_; }

signals:
    void streamError(const QString& msg);
    void firstFrameReady();
    void streamStarted();
    void streamStopped();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void pollBus();
    void handleFirstFrameReady(qulonglong generation);

private:
    GstElement* pipeline_ = nullptr;
    int current_port_ = 5600;
    bool playing_ = false;
    bool auto_start_on_show_ = false;
    QTimer* bus_poll_timer_ = nullptr;
    std::atomic_ullong stream_generation_{0};
    bool first_frame_emitted_ = false;

    void setupPipeline(int port);
    void destroyPipeline();
    static GstBusSyncReply busSyncHandler(GstBus* bus, GstMessage* msg, gpointer data);
    static gboolean busCallback(GstBus* bus, GstMessage* msg, gpointer data);
    static GstPadProbeReturn firstFrameProbe(GstPad* pad, GstPadProbeInfo* info, gpointer data);
};

// =============================================================================
// Custom Plot Widget
// =============================================================================

class PlotWidget : public QWidget {
    Q_OBJECT

public:
    explicit PlotWidget(QWidget* parent = nullptr);

    // Data setters
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
    
    // Rectangle drawing mode
    void startRectangleMode();
    void cancelRectangleMode();
    bool isDrawingRectangle() const { return drawing_rectangle_; }
    
    // Dark mode
    void setDarkMode(bool enabled);
    bool isDarkMode() const { return dark_mode_; }
    void setPlannerPreviewMode(bool enabled);
    
    // Clear functions
    void clearAll();
    void clearPoints();
    void clearPolygon();
    void clearROI();
    void clearObstacles();
    void clearSwaths();
    void clearRoute();
    void clearPath();
    
    // View control
    void resetView();
    void zoomIn();
    void zoomOut();
    
    // ROI/Obstacle selection
    void startROISelection();
    void startObstacleSelection();
    void finishSelection();
    void cancelSelection();
    void undoLastPoint();
    
    bool isSelecting() const { return selecting_; }
    Polygon2D getSelectedPolygon() const;
    int selectedObstacleIndex() const { return selected_obstacle_idx_; }  // -1 = none
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
    // Data
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
    
    // Reprojection error visualization
    std::vector<ReprojectionLine> reproj_lines_;
    int hovered_reproj_index_ = -1;  // -1 = none hovered
    bool show_live_overlay_ = false;
    std::vector<QString> live_overlay_lines_;
    
    // Rectangle drawing mode (3-click: corner1, corner2 defines edge, corner3 defines width)
    bool drawing_rectangle_ = false;
    std::vector<Point2D> rect_points_;  // 0-2 points during drawing
    
    // Dark mode
    bool dark_mode_ = false;
    bool planner_preview_mode_ = false;
    
    // View transform
    double scale_ = 1.0;
    double offset_x_ = 0.0;
    double offset_y_ = 0.0;
    double data_min_x_ = 0, data_max_x_ = 1;
    double data_min_y_ = 0, data_max_y_ = 1;
    
    // Selection state
    bool selecting_ = false;
    bool selecting_roi_ = false;  // true = ROI, false = obstacle
    std::vector<Point2D> selection_points_;
    QPointF cursor_pos_;

    // Obstacle selection
    int selected_obstacle_idx_ = -1;  // -1 = none
    
    // Panning
    bool panning_ = false;
    QPoint pan_start_;
    double pan_offset_x_, pan_offset_y_;
    
    // Helpers
    QPointF worldToScreen(const Point2D& p) const;
    Point2D screenToWorld(const QPointF& p) const;
    void updateDataBounds();
    void fitToData();
    double distanceToLineSegment(const QPointF& mouse, const QPointF& p1, const QPointF& p2) const;
};

// =============================================================================
// Main GUI Window
// =============================================================================

class CoverageGUI : public QMainWindow {
    Q_OBJECT

public:
    explicit CoverageGUI(QWidget* parent = nullptr);
    ~CoverageGUI() override;
    void setDrivenPathSnapshotCutoffMs(qint64 cutoff_ms);

private slots:
    // File operations
    void loadPointCloud();
    void fetchLatestMapFromRobot();
    void loadPointCloudFromPath(const QString& path);
    
    // Processing
    void applyHeightCrop();
    void applyDownsample();
    void computeHull();
    void simplifyPolygon();
    
    // 3D Visualization
    void showPointCloud3D();
    
    // ROI / Obstacles
    void toggleROISelection();
    void clearROI();
    void toggleObstacleSelection();
    void autoDetectObstacles();
    void deleteSelectedObstacle();
    void clearObstacles();
    void undoSelectionPoint();
    void finishSelection();
    
    // Coverage
    void buildField();
    void generateSwaths();
    void generateRoute();
    void generatePath();
    void clearCoverage();
    
    // Export
    void exportPathCSV();
    void publishWaypoints();
    void startNavigation();
    void clearRobotTrail();
    void onPathModeChanged();
    
    // Callbacks
    void onROISelected(const Polygon2D& roi);
    void onObstacleSelected(const Polygon2D& obstacle);
    void onSelectionCancelled();
    void onObstacleDeleteRequested(int index);
    void onObstacleSelectionChanged(int index);
    void onAutoDetectObstaclesFinished();
    
    // UI updates
    void updateDownsampleUI(const QString& method);
    
    // ROS2 reconnection
    void tryReconnectROS2();
    
    // Zenoh bridge status monitoring
    void checkZenohBridgeStatus();
    
    // Reprojection error analysis
    void computeReprojectionError();
    void clearReprojectionError();
    
    // Rectangle drawing
    void toggleRectangleMode();
    void onRectangleCompleted(const Polygon2D& rect);
    
    // Dark mode
    void toggleDarkMode();
    void applyTheme();
    
    // Coverage statistics
    void updateCoverageStats();
    CoverageStats computeStats() const;
    
    // Workflow steps
    void updateWorkflowSteps();
    void onWorkflowStepClicked(int step);
    
    // Layer visibility
    void updateLayerVisibility();
    
    // Video streaming
    QWidget* buildVideoPanelWidget();
    void toggleVideoPanel();
    void onCameraToggled(bool right_selected);
    void playVideoStream();
    void stopVideoStream();
    void onCameraStatusReceived(const std_msgs::msg::String::SharedPtr msg);
    
    // Data transfer (tabbed: download + upload)
    void openDataTransferDialog();
    void onTransferActive(bool active);
    void onTransferProgress(int percent, double speedMBps);
    void onShowTransferDialogRequested();
    void onCancelTransferRequested();
    
    // Preset management
    void onPresetSelected(int index);
    void saveCurrentPreset();
    void createNewPreset();
    void openPresetManager();
    void loadPreset(const QString& name);
    void refreshPresetList();
    PlanningPreset gatherCurrentSettings() const;
    void applyPreset(const PlanningPreset& preset);
    
    // Teleop widget
    void toggleTeleopWidget();
    void onTeleopStatusMessage(const QString& message);
    
    // Cloud upload (integrated in tabbed dialog)
    void onCloudUploadActive(bool active);
    
    // Scan session tracking
    void startScanSession(const QString& sectionName);
    void endScanSession();

private:
    struct LiveStatsSnapshot;

    void setupUI();
    void setupConnections();
    void setupRobotTrackingSubscription();
    
    // UI building helpers
    QGroupBox* buildFileControls();
    QGroupBox* buildHeightControls();
    QGroupBox* buildDownsampleControls();
    QGroupBox* buildHullControls();
    QGroupBox* buildSimplifyControls();
    QGroupBox* buildCoverageControls();
    QWidget* buildPresetControls();
    QGroupBox* buildExportControls();
    QGroupBox* buildRobotTrackingControls();
    QGroupBox* buildPathPlanningControls();
    QWidget* buildF2CControls();
    QWidget* buildCustomPathControls();
    QGroupBox* buildCoverageStatsControls();
    QWidget* buildWorkflowIndicator();
    QWidget* buildLayerPanel();
    QWidget* buildQuickActionsBar();
    QGroupBox* buildScanPlannerPanel();
    QWidget* buildDataTransferPanel();
    
    // Async point cloud loading
    void loadPointCloudAsync(const QString& path);
    void onPointCloudLoaded();
    
    // Status/Progress
    void setStatus(const QString& text, int timeout_ms = 0);
    void showProgress(bool show, const QString& text = "");
    void updateProgress(int percent, const QString& text = "");
    
    // Get current configuration
    CoverageConfig currentConfig() const;
    
    // Refresh plot
    void refreshPlot();
    
    // Apply effective polygon (with ROI/obstacles)
    Polygon2D effectivePolygon() const;
    void updateRobotStatusLabel(bool has_fix);
    std::vector<PathState> buildDrivenPathSnapshotLocked() const;
    static double computePathLengthMeters(const std::vector<PathState>& states);
    void scheduleFitToView();
    void refreshCustomPathUI();
    void onPlotCustomWaypoint(const Point2D& point);
    void undoCustomWaypoint();
    void clearCustomWaypoints();
    void publishCustomPath();
    void updateCustomWaypointStatus();
    void setCustomModeActive(bool active);
    bool isCustomModeActive() const;
    void syncPlannedPathCache();
    void rebuildPlannedPathCache(const std::vector<Point2D>& path_points);
    std::optional<LiveStatsSnapshot> updateLiveStatsFromOdom(
        const PathState& state, std::chrono::steady_clock::time_point stamp, bool& should_emit_ui);
    double projectAlongPlannedPath(const Point2D& point, size_t& segment_hint) const;
    void resetLiveStatsUI();
    void updateLiveStatsUI(const LiveStatsSnapshot& snapshot);
    void rebuildLiveOverlay();
    std::vector<QString> buildLiveOverlayLines(const LiveStatsSnapshot& snapshot) const;
    void updateScanSegmentCompletion(double completed_m);
    void generateScanSegments();
    void refreshScanSegmentList();
    std::vector<int> selectedScanSegmentIndices() const;
    PathStateList buildPublishPathFromSegments(const std::vector<int>& indices) const;
    int estimateTurns(const PathStateList& seg) const;
    void publishSelectedScanSegments();
    void startSelectedScanSegments();
    void setActiveScanSegmentFromList(int idx);
    
    // UI helpers for collapsible panes
    QWidget* buildLeftMiniPalette();
    QWidget* buildRightMiniPalette();
    QWidget* buildRightFullPane();
    void toggleLeftPane();
    void toggleRightPane();
    void updateCollapseButtons();

private:
    struct LiveStatsSnapshot {
        double planned_length_m = 0.0;
        double completed_m = 0.0;
        double remaining_m = 0.0;
        double coverage_pct = 0.0;
        double traveled_m = 0.0;
        double speed_mps = 0.0;
        double eta_sec = 0.0;
        double elapsed_sec = 0.0;
        bool active = false;
    };
    
    struct ScanSegment {
        QString name;
        PathStateList path;
        double start_m = 0.0;
        double end_m = 0.0;
        double length_m = 0.0;
        int turns = 0;
        bool completed = false;
    };

    // Main widgets
    PlotWidget* plot_;
    QStatusBar* status_bar_;
    QProgressBar* progress_bar_;
    
    // File controls
    QLabel* lbl_file_;
    QLineEdit* txt_robot_ip_;
    QCheckBox* chk_show_robot_;
    QPushButton* btn_clear_robot_trail_;
    QLabel* lbl_robot_status_;
    QLineEdit* txt_robot_topic_;
    QDoubleSpinBox* spin_robot_marker_size_;
    
    // Path mode selector (F2C vs Custom)
    QRadioButton* radio_mode_f2c_ = nullptr;
    QRadioButton* radio_mode_custom_ = nullptr;
    QWidget* f2c_controls_widget_ = nullptr;
    QWidget* custom_controls_widget_ = nullptr;
    QPushButton* btn_custom_draw_ = nullptr;
    QPushButton* btn_custom_undo_ = nullptr;
    QPushButton* btn_custom_clear_ = nullptr;
    QListWidget* list_custom_points_ = nullptr;
    QLabel* lbl_custom_status_ = nullptr;
    QPushButton* btn_publish_waypoints_ = nullptr;
    QPushButton* btn_start_navigation_ = nullptr;
    QDoubleSpinBox* spin_scan_len_ = nullptr;
    QListWidget* list_scan_segments_ = nullptr;
    QPushButton* btn_make_segments_ = nullptr;
    QPushButton* btn_publish_segments_ = nullptr;
    QPushButton* btn_start_segments_ = nullptr;
    QLabel* lbl_scan_progress_ = nullptr;
    
    // DDS / Zenoh bridge status controls
    QLabel* lbl_dds_status_;
    QLabel* lbl_zenoh_status_;
    
    // Height controls (Z range filtering relative to robot origin Z=0)
    QDoubleSpinBox* spin_z_min_;   // Minimum Z value (can be negative)
    QDoubleSpinBox* spin_z_max_;   // Maximum Z value
    
    // Downsample controls
    QComboBox* combo_downsample_;
    QGroupBox* group_random_;
    QGroupBox* group_voxel_;
    QGroupBox* group_stat_;
    QSpinBox* spin_max_points_;
    QDoubleSpinBox* spin_voxel_;
    QSpinBox* spin_mean_k_;
    QDoubleSpinBox* spin_std_ratio_;
    
    // Hull controls
    QComboBox* combo_hull_method_;
    QDoubleSpinBox* spin_alpha_;
    
    // Simplify controls
    QDoubleSpinBox* spin_simplify_;
    
    // Coverage controls
    QDoubleSpinBox* spin_swath_;
    QDoubleSpinBox* spin_headland_;
    QDoubleSpinBox* spin_turn_;
    QCheckBox* chk_auto_align_;
    QRadioButton* radio_long_;
    QRadioButton* radio_perp_;
    QComboBox* combo_route_pattern_;
    QComboBox* combo_path_planner_;
    QCheckBox* chk_decomposition_;
    QComboBox* combo_decomp_type_;
    QCheckBox* chk_axial_turns_;
    QDoubleSpinBox* spin_waypoint_spacing_ = nullptr;
    
    // Preset controls
    PresetManager* preset_manager_ = nullptr;
    QComboBox* combo_preset_ = nullptr;
    QPushButton* btn_save_preset_ = nullptr;
    QPushButton* btn_new_preset_ = nullptr;
    QPushButton* btn_manage_presets_ = nullptr;
    
    // ROI controls
    QPushButton* btn_roi_;
    QPushButton* btn_roi_clear_;
    QPushButton* btn_roi_finish_;
    QPushButton* btn_roi_undo_;
    QLabel* lbl_roi_;
    
    // Obstacle controls
    QPushButton* btn_obstacle_;
    QPushButton* btn_auto_detect_obstacles_ = nullptr;
    QPushButton* btn_delete_selected_obstacle_ = nullptr;
    QPushButton* btn_obstacle_clear_;
    QLabel* lbl_obstacles_;
    
    // State
    PointCloudPtr pcd_points_;
    PointCloudPtr filtered_points_;
    std::vector<Point2D> xy_2d_;
    Polygon2D polygon_;
    Polygon2D roi_polygon_;
    std::vector<Obstacle2D> obstacles_;
    QFutureWatcher<ObstacleDetectionResult>* obstacle_detect_watcher_ = nullptr;
    bool auto_detect_obstacles_running_ = false;
    SwathList swaths_;
    PathStateList route_;
    PathStateList path_;
    double effective_area_m2_ = 0.0;  // From backend: (boundary ∩ ROI) − obstacles
    QString loaded_file_;

    // ROS2 integration for waypoint publishing
    rclcpp::Node::SharedPtr ros_node_;
    rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr waypoint_pub_;
    rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr fastlio_sub_;
    std::thread ros_thread_;
    bool waypoints_published_;
    bool ros_initialized_;
    
    // ROS2 reconnection timer (only active when disconnected)
    QTimer* ros_reconnect_timer_;
    
    // Robot connection settings for map fetching
    QString robot_host_ = "192.168.168.101";
    QString robot_user_ = "roofus";
    QString robot_data_path_ = "/R_DATA";
    QString local_map_base_;  // Set to ~/Roofus_maps in constructor
    
    // CycloneDDS config (loopback-only for local node communication)
    QString dds_config_path_;       // ~/cyclone_loopback.xml
    
    // Zenoh bridge status tracking (bridge managed by laptop_teleop.launch.py)
    QTimer* zenoh_check_timer_;
    bool zenoh_bridge_detected_;
    
    // Helper to shutdown and reinitialize ROS2
    void reinitializeROS2();
    
    // Robot tracking state
    mutable std::mutex robot_pose_mutex_;
    std::optional<PathState> robot_pose_state_;
    std::vector<Point2D> robot_trail_;
    std::vector<PathState> robot_trail_states_;
    std::vector<qint64> robot_trail_state_timestamps_ms_;
    std::vector<PathState> driven_path_snapshot_;
    std::optional<qint64> driven_path_snapshot_cutoff_ms_;
    size_t robot_trail_max_points_ = 5000;
    static constexpr double kAutoDetectMinDrivenPathLengthM = 5.0;
    std::chrono::steady_clock::time_point last_robot_update_;
    QString robot_odom_topic_ = "/Odometry_tilt_corrected_diff";
    double robot_marker_size_m_ = 0.6;
    bool fit_view_pending_ = false;
    bool custom_draw_enabled_ = false;
    std::vector<Point2D> custom_waypoints_;
    std::vector<bool> custom_waypoints_visited_;
    double custom_waypoint_reach_tol_ = 0.10;
    
    // Throttle plot refresh to avoid excessive repaints from high-frequency odom
    std::chrono::steady_clock::time_point last_plot_refresh_;
    static constexpr int kPlotRefreshIntervalMs = 50;  // ~20 Hz max
    static constexpr int kLiveUiIntervalMs = 200;      // 5 Hz UI updates for live stats
    static constexpr double kLiveStartGateM = 0.05;    // Require 5 cm to arm live stats
    static constexpr double kEtaMinSpeed = 0.05;       // Min speed to show ETA (m/s)
    
    // Reprojection error analysis
    QPushButton* btn_compute_reproj_ = nullptr;
    QPushButton* btn_clear_reproj_ = nullptr;
    QLabel* lbl_reproj_status_ = nullptr;
    std::vector<ReprojectionLine> reproj_lines_;
    
    // Collapsible layout
    QSplitter* main_splitter_ = nullptr;
    QStackedWidget* left_stack_ = nullptr;
    QWidget* left_full_ = nullptr;
    QWidget* left_mini_ = nullptr;
    QStackedWidget* right_stack_ = nullptr;
    QWidget* right_full_ = nullptr;
    QWidget* right_mini_ = nullptr;
    QToolButton* btn_collapse_left_ = nullptr;
    QToolButton* btn_collapse_right_ = nullptr;
    int left_saved_width_ = 390;
    int right_saved_width_ = 260;
    bool left_collapsed_ = false;
    bool right_collapsed_ = false;
    
    // Rectangle drawing tool
    QPushButton* btn_rectangle_ = nullptr;
    
    // Dark mode
    bool dark_mode_ = false;
    QPushButton* btn_dark_mode_ = nullptr;
    
    // Coverage statistics widgets
    QGroupBox* stats_group_ = nullptr;
    QLabel* lbl_stats_path_length_ = nullptr;
    QLabel* lbl_stats_area_ = nullptr;
    QLabel* lbl_stats_coverage_ = nullptr;
    QLabel* lbl_stats_swaths_ = nullptr;
    QLabel* lbl_stats_turns_ = nullptr;
    QLabel* lbl_stats_waypoints_ = nullptr;
    QLabel* lbl_stats_time_ = nullptr;
    QLabel* lbl_stats_live_progress_ = nullptr;
    QLabel* lbl_stats_live_travel_ = nullptr;
    QLabel* lbl_stats_live_remaining_ = nullptr;
    QLabel* lbl_stats_live_eta_ = nullptr;
    QLabel* lbl_stats_live_elapsed_ = nullptr;
    QLabel* lbl_stats_live_speed_ = nullptr;
    QCheckBox* chk_live_overlay_ = nullptr;
    QCheckBox* chk_live_show_progress_ = nullptr;
    QCheckBox* chk_live_show_travel_ = nullptr;
    QCheckBox* chk_live_show_remaining_ = nullptr;
    QCheckBox* chk_live_show_eta_ = nullptr;
    QCheckBox* chk_live_show_elapsed_ = nullptr;
    QCheckBox* chk_live_show_speed_ = nullptr;
    QDoubleSpinBox* spin_robot_speed_ = nullptr;
    CoverageStats current_stats_;
    
    // Async loading
    QFutureWatcher<PointCloudPtr>* pcd_watcher_ = nullptr;
    QString pending_load_path_;
    
    // Workflow steps indicator
    QWidget* workflow_widget_ = nullptr;
    std::vector<QPushButton*> workflow_btns_;
    int current_workflow_step_ = 0;
    
    // Layer visibility panel
    QCheckBox* chk_layer_points_ = nullptr;
    QCheckBox* chk_layer_polygon_ = nullptr;
    QCheckBox* chk_layer_roi_ = nullptr;
    QCheckBox* chk_layer_obstacles_ = nullptr;
    QCheckBox* chk_layer_swaths_ = nullptr;
    QCheckBox* chk_layer_path_ = nullptr;
    QCheckBox* chk_layer_trail_ = nullptr;
    QCheckBox* chk_layer_robot_ = nullptr;
    QCheckBox* chk_layer_scan_segments_ = nullptr;
    
    // Quick actions bar
    QWidget* quick_actions_bar_ = nullptr;
    QPushButton* btn_quick_generate_ = nullptr;
    QPushButton* btn_quick_publish_ = nullptr;
    QPushButton* btn_quick_start_ = nullptr;
    QLabel* lbl_quick_status_ = nullptr;
    
    // Collapsible sections (QToolBox)
    QToolBox* toolbox_ = nullptr;
    
    // Video streaming panel
    QWidget* video_panel_widget_ = nullptr;
    VideoStreamWidget* video_widget_ = nullptr;
    QPushButton* btn_view_fov_ = nullptr;
    QRadioButton* radio_cam_left_ = nullptr;
    QRadioButton* radio_cam_right_ = nullptr;
    QSpinBox* spin_video_port_ = nullptr;
    QPushButton* btn_video_play_ = nullptr;
    QPushButton* btn_video_stop_ = nullptr;
    QLabel* lbl_video_status_ = nullptr;
    
    // Data transfer panel
    QWidget* data_transfer_panel_ = nullptr;
    QPushButton* btn_open_transfer_dialog_ = nullptr;
    TransferProgressWidget* transfer_progress_widget_ = nullptr;
    DataTransferDialog* data_transfer_dialog_ = nullptr;
    CloudUploadDialog* cloud_upload_dialog_ = nullptr;
    QDialog* data_transfer_window_ = nullptr;     // Tabbed window (download + upload)
    QTabWidget* data_transfer_tabs_ = nullptr;
    
    // Teleop widget (floating/dockable)
    TeleopDockWidget* teleop_dock_ = nullptr;
    QAction* action_show_teleop_ = nullptr;
    
    // Scan session tracker (GPS + stats persistence)
    ScanSessionTracker* scan_session_tracker_ = nullptr;
    
    // ROS2 camera selection publisher
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr camera_select_pub_;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr camera_status_sub_;
    QString current_streaming_camera_ = "left";
    
    // ROS2 stream target configuration
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr stream_target_pub_;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr stream_status_sub_;
    QString current_stream_target_;
    bool stream_target_confirmed_ = false;
    
    // Live coverage tracking
    std::vector<Point2D> planned_path_points_;
    std::vector<double> planned_cumulative_dist_;
    double planned_path_length_m_ = 0.0;
    size_t planned_segment_hint_ = 0;
    double live_completed_m_ = 0.0;
    double live_traveled_m_ = 0.0;
    double live_filtered_speed_mps_ = 0.0;
    bool live_progress_active_ = false;
    bool mission_timer_active_ = false;
    std::optional<Point2D> last_odom_point_live_;
    std::chrono::steady_clock::time_point last_odom_time_live_;
    std::chrono::steady_clock::time_point mission_start_time_;
    std::chrono::steady_clock::time_point last_live_ui_update_;
    mutable std::mutex live_stats_mutex_;
    std::optional<LiveStatsSnapshot> last_live_snapshot_;
    std::vector<ScanSegment> scan_segments_;
    int active_scan_segment_idx_ = -1;
    
    // Auto-detect local IP for streaming
    QString detectLocalIP() const;
    void publishStreamTarget();
    void onStreamStatusReceived(const std_msgs::msg::String::SharedPtr msg);
};

} // namespace f2c_cpp

