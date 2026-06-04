#pragma once

#include <QDateTime>
#include <QFutureWatcher>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <optional>
#include <vector>

#include "coverage_pipeline.hpp"
#include "obstacle_detector.hpp"
#include "preset_manager.hpp"

class QLabel;
class QComboBox;
class QDateTime;
class QEvent;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QObject;
class QHideEvent;
class QProgressBar;
class QPushButton;
class QKeyEvent;
class QShowEvent;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace f2c_cpp {

class PlotWidget;
class PlannerTrackSlider;
class FPVCameraView;

class PlannerScreen : public QWidget {
    Q_OBJECT

public:
    enum class ValueTone {
        Good,
        Warning,
        Muted,
        Error
    };

    explicit PlannerScreen(QWidget* parent = nullptr);

    void setDarkMode(bool dark_mode);
    void setRobotId(const QString& robot_id);
    void setMapPath(const QString& map_path);
    void setLiveRobotTelemetry(const std::optional<PathState>& pose,
                               const std::vector<Point2D>& trail);
    // Latest robot ground speed (m/s, unsigned). Fed from AppShellWindow's
    // odometry pipe. Used by effectiveScanSpeedMps() while a scan is running.
    void setLiveRobotSpeedMps(double speed_mps);
    void setTopSignalState(const QString& text, ValueTone tone);
    // Top-bar battery pill — see ExplorationScreen::setTopBatteryState
    // for semantics; same dashboard-mirroring behavior, identical
    // thresholds (≤12 % red, ≤25 % amber, otherwise green; "—%"
    // muted when stale).
    void setTopBatteryState(double pct, bool stale);
    void setTopRecPillState(const QString& text, ValueTone tone);
    // BOT pill: drives the LinkHealthMonitor state to a top-bar chip
    // identical in shape to the Signal / REC pills. AppShellWindow
    // pushes this on every linkStateChanged transition. Tone semantics
    // match the existing pills: Good=green LIVE, Warning=amber
    // RECONNECTING, Error=red OFFLINE, Muted=grey IDLE.
    void setBotLinkPillState(const QString& text, ValueTone tone);
    // Disconnect surface for Stage 4 (Scan execution).  Driven by the
    // layered LinkHealthMonitor / RobotReachabilityProbe model:
    //
    //   connected=true  -> Healthy.  No grey-out, no banner, buttons
    //                      enabled.  `reachable` ignored.
    //   connected=false, reachable=true  -> Reconnecting.  Grey-out
    //                      live telemetry + button lockout, but NO
    //                      banner and NO map halo (visually soft —
    //                      the host is still on the network so we
    //                      avoid the "panic OFFLINE" treatment).
    //                      Tooltip says "Robot reconnecting…".
    //   connected=false, reachable=false -> True OFFLINE.  Grey-out,
    //                      banner, halo, button lockout with "Robot
    //                      offline — wait for reconnect." tooltip.
    //
    // AppShell calls this on every LinkHealthMonitor transition.
    void setLinkConnectionState(bool connected, bool reachable, qint64 since_ms);
    void setTopLockChipState(const QString& text, ValueTone tone);
    void setTopMotorsChipState(const QString& text, ValueTone tone);

    // Wall-clock ms timestamp of the most recently delivered RTP video
    // frame on the Stage 5 scan camera tile.  Returns 0 when the stream
    // isn't playing (placeholder visible) so AppShell's
    // LinkHealthMonitor stamping logic can ignore it.  Mirrors
    // ExplorationScreen::lastFpvFrameWallMs() — same FPV proof-of-life
    // signal, but for the Stage 5 instance of the camera widget.
    //
    // Why this matters: RTP/UDP video keeps flowing through brief
    // Microhard radio fades (it's stateless), while Zenoh ROS topics
    // can hang for 30+ s on a stale TCP socket.  Without this signal,
    // LinkHealthMonitor on Stage 5 would only see ROS topic age and
    // false-positive into RECONNECTING even when the bot is verifiably
    // alive on camera.
    qint64 lastScanFpvFrameWallMs() const;

    void notifyScanSegmentCompleted();
    void notifyScanSegmentSaved();
    // Called by AppShell after /dc/cancel_scan has returned (or hit the
    // safety ceiling). Wipes per-mission Stage-4 runtime state, clears the
    // planned path / segments / coverage cache so the operator must re-plan,
    // and navigates back to the Map Processing step. `success` is logged
    // only — the UI always proceeds with the reset, since by the time we
    // reach this point the controller has already torn down DC.
    void notifyScanCancelled(bool success);

    // Called by AppShell after the post-Completed Discard Scan flow
    // finishes (success after retry, or terminal failure). Unlike Cancel,
    // Discard does NOT navigate away — the operator stays on Stage 5 with
    // the planned path + last stats still visible, the segment list cleared,
    // and the action button locked into a terminal "Discarded" state. The
    // operator advances by pressing Complete Mission.
    void notifyScanDiscarded(bool success);

    // BDR_REWIRE: dev-only hook used by AppShellWindow when the env var
    // BDR_DEV_START_AT_SCAN=1 is set, so we can boot directly into the
    // Stage-4 (Scan) screen for screenshot / UI iteration without driving
    // the full planner pipeline. Remove (or guard behind a build flag) once
    // the production flow is in place.
    void devForceJumpToScanStep();

signals:
    void backRequested();
    void nextStageRequested();
    void publishScanSegmentsRequested(const std::vector<double>& xy_pairs);
    void startScanSegmentsRequested(const QString& progression_mode);
    // Stage 4 (Scan execution) signals
    // Carries the current scan-speed slider value (m/s). AppShellWindow
    // forwards it to mpc_accel_autonomous_controller's `max_linear_velocity` param via
    // sendControllerMaxLinearVelocity() before resuming autonomy. Signal
    // payload (vs. shared cache fetch) keeps the start-scan path
    // self-describing and races-free.
    void scanStartRequested(double speed_mps);
    void scanPauseRequested();
    void scanResumeRequested();
    void emergencyStopRequested();
    void scanTeleopTwistRequested(double linear_x, double angular_z);
    void scanTeleopArmRequested();
    void scanTeleopDisarmRequested();
    void scanTeleopGprPowerOffRequested();
    void completeMissionRequested();
    // Operator pressed Cancel Scan and confirmed the destructive dialog.
    // AppShell calls /dc/cancel_scan and then notifyScanCancelled() to
    // reset our UI back to Map Processing.
    void cancelScanRequested();
    // Operator pressed Discard Scan (post-Completed reuse of the same
    // button slot) and confirmed the destructive dialog. AppShell calls
    // /dc/cancel_scan with retry-once and then notifyScanDiscarded(). The
    // UI does NOT navigate away — see notifyScanDiscarded() for details.
    void discardScanRequested();
    // Operator pressed Wake GPR inside the pre-scan checklist dialog.
    // AppShell forwards to /gpr_line_stop (the Arduino wake keystroke).
    void wakeGprRequested();

private slots:
    void onBackClicked();
    void onNextClicked();

    // Re-renders the static slider min/max endpoint badges (Voxel /
    // Z-Min / Z-Max / Path Spacing / Headland / Robot Speed) when the
    // operator flips the units toggle in MissionMetadataDialog. The
    // badges are constructed once in buildUi() with strings baked from
    // the current units; without this connection a Metric → Complete →
    // ANSI mission cycle would leave them stale (e.g. "0.30 m/s" next
    // to a slider whose value reads "0.98 ft/s"). Connected to
    // UnitsProvider::unitsChanged in the ctor.
    void relabelUnitEndpointBadges();

    // Shows / hides + re-computes the small "(≈ X.XX ft)" hint next
    // to the scan distance QLineEdit. Visible iff UnitsProvider is in
    // ANSI mode. Triggered by (a) text edits in the field, (b)
    // updateScanSplittingUi() reflowing the field, (c) the
    // UnitsProvider::unitsChanged signal connection in the ctor.
    void refreshScanDistanceAnsiHint();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    enum class PlannerStep {
        MapProcessing = 0,
        CoveragePlanning = 1,
        ScanSplitting = 2,
        Scan = 3,
    };

    enum class ScanRunState {
        Idle,
        Running,
        Paused,
        Completed,
    };

    struct SessionCache {
        struct CoveragePreset {
            QString name;
            QString route_pattern;
            double path_spacing = 0.50;
            double headland_width = 0.30;
            QString scan_axis;
            // Robot cruise speed (m/s) pushed to the controller's
            // `max_linear_velocity` ROS param at scan-start. Per-preset
            // (preset switch updates the slider).
            double scan_speed_mps = 0.40;
            bool custom = false;
        };

        struct CoverageObstacle {
            int id = 0;
            QString type;
            double area_m2 = 0.0;
            QString source;
            Obstacle2D geometry;
        };

        struct ScanSegment {
            QString name;
            PathStateList path;
            double start_m = 0.0;
            double end_m = 0.0;
            double length_m = 0.0;
            int turns = 0;
            bool selected = false;
            bool completed = false;
            // Stage 4 runtime state. BDR_REWIRE: filled by mock data today; should be
            // updated from controller telemetry once per-segment progress topics exist.
            double progress_pct = 0.0;
            double quality_pct = 0.0;
        };

        double voxel_size = 0.05;
        double z_min = -0.10;
        double z_max = 0.10;
        double alpha = 1.50;
        QString coverage_scan_mode = QStringLiteral("complete");
        QString coverage_pattern = QStringLiteral("boustro");
        double coverage_path_spacing = 0.50;
        double coverage_headland_width = 0.30;
        QString coverage_scan_axis = QStringLiteral("parallel");
        // Cruise speed (m/s). Pushed to mpc_accel_autonomous_controller's
        // `max_linear_velocity` param at scan-start (signal-with-payload via
        // scanStartRequested). Slider locked while scan running — config-
        // only, no live runtime mutation.
        double coverage_scan_speed_mps = 0.40;
        QString coverage_selected_preset = QStringLiteral("Standard");
        std::vector<CoveragePreset> coverage_presets;
        QString coverage_new_preset_name;
        bool coverage_show_save_preset = false;
        QString coverage_roi_drawing_tool = QStringLiteral("rectangle");
        bool coverage_roi_drawing_active = false;
        Polygon2D coverage_roi_polygon;
        QString coverage_obstacle_mode = QStringLiteral("automatic");
        // CSF auto-detection sensitivity (0-100, default 50). Higher = more
        // sensitive (smaller CSF class threshold => detects smaller protrusions).
        double coverage_csf_sensitivity = 50.0;
        QString coverage_drawing_tool = QStringLiteral("rectangle");
        bool coverage_drawing_active = false;
        bool coverage_obstacles_detected = false;
        int coverage_next_obstacle_id = 1;
        std::vector<CoverageObstacle> coverage_obstacles;
        double scan_distance_m = 500.0;
        QString scan_progression_mode = QStringLiteral("automatic");
        std::vector<ScanSegment> scan_segments;
        bool scan_splits_dirty = true;
        bool scan_waypoints_published = false;
        // Stage 4 (Scan) execution state. Not persisted across runs.
        ScanRunState scan_run_state = ScanRunState::Idle;
        int scan_active_segment_index = -1;
        double scan_total_coverage_pct = 0.0;
        double scan_avg_quality_pct = 0.0;
        double scan_distance_traveled_m = 0.0;
        qint64 scan_elapsed_ms = 0;
        qint64 scan_estimated_ms_left = -1;
        // Set true after the operator confirms the pre-scan checklist
        // dialog. Reset on every entry to ScanSplitting so each transit
        // through the splitting -> scan path forces a re-acknowledgment.
        // Not persisted across runs.
        bool scan_preflight_acknowledged = false;
        bool raw_loaded = false;
        bool processing_complete = false;
        bool hull_complete = false;
        bool planning_complete = false;
        PointCloudPtr raw_cloud;
        PointCloudPtr processed_cloud;
        std::vector<Point2D> raw_projected_points;
        std::vector<Point2D> processed_projected_points;
        Polygon2D hull_polygon;
        SwathList planned_swaths;
        PathStateList planned_route;
        PathStateList planned_path;
        qsizetype raw_point_count = 0;
        qsizetype processed_point_count = 0;
        double raw_area_estimate_m2 = 0.0;
        double processed_area_estimate_m2 = 0.0;
        double hull_area_m2 = 0.0;
        double planned_effective_area_m2 = 0.0;
        double estimated_file_size_mb = 0.0;
        QString quality_label;
        PlannerStep last_step = PlannerStep::MapProcessing;
    };

    struct StageStepUi {
        QWidget* wrapper = nullptr;
        QPushButton* button = nullptr;
        QLabel* icon = nullptr;
        QLabel* text = nullptr;
        QWidget* separator = nullptr;
        QString icon_path;
        int group_width = 0;
        int button_width = 0;
    };

    struct MapLoadResult {
        bool success = false;
        QString error;
        PointCloudPtr raw_cloud;
        std::vector<Point2D> raw_projected_points;
        qsizetype raw_point_count = 0;
        double area_estimate_m2 = 0.0;
    };

    struct ProcessResult {
        bool success = false;
        QString error;
        PointCloudPtr processed_cloud;
        std::vector<Point2D> processed_projected_points;
        qsizetype raw_point_count = 0;
        qsizetype processed_point_count = 0;
        double reduction_percent = 0.0;
        double estimated_file_size_mb = 0.0;
        QString quality_label;
        double area_estimate_m2 = 0.0;
    };

    struct HullResult {
        bool success = false;
        QString error;
        Polygon2D hull_polygon;
        double area_m2 = 0.0;
    };

    struct PlanningResult {
        bool success = false;
        QString error;
        CoverageResult coverage;
    };

    void buildUi();
    void applyStyle();
    void applyToneToLabel(QLabel* label, ValueTone tone, bool emphasize);
    void updateTopMotorsChipGeometry();
    QString sessionKey() const;
    QString settingsGroupKey() const;
    SessionCache& activeSession();
    const SessionCache* activeSessionPtr() const;
    void restoreCurrentSession();
    void loadPersistedParameters(SessionCache& cache) const;
    void persistParameters() const;
    void persistCurrentStep();
    void ensureCoverageDefaults(SessionCache& cache) const;
    // Preset persistence (production-grade: PresetManager JSON-per-file).
    // Factory presets are code-only constants and never touched on disk.
    void reloadCoveragePresetsFromDisk(SessionCache& cache) const;
    PlanningPreset buildPlanningPresetFromSession(const SessionCache& cache) const;
    void applyPlanningPresetToSession(const PlanningPreset& preset, SessionCache& cache) const;
    static const std::vector<SessionCache::CoveragePreset>& factoryPresets();
    static bool isFactoryPresetName(const QString& name);
    // ETA speed source: live odometry while scanning, else cached controller
    // max_linear_velocity, else 0.4 m/s. Pure (no UI side effects).
    double effectiveScanSpeedMps() const;
    void invalidateProcessingResult(const QString& status_message = QString());
    void invalidateHullResult(const QString& status_message = QString());
    void invalidateCoverageResult(const QString& status_message = QString());
    void updateValueLabels();
    void updatePreview();
    void updateOutputCards();
    void updateStatsChip();
    void updatePlaceholderMessage();
    void updateCoveragePlanningUi();
    void refreshCoveragePresetCombo();
    void rebuildCoveragePresetRows();
    void rebuildCoverageObstacleRows();
    void updateHeaderForCurrentStep();
    void updateStageSteps();
    void updateFooter();
    void updateButtonsAndStatus();
    void applyLiveOverlayToPlot();
    void applySessionToUi();
    void navigateToStep(PlannerStep step);
    void setInlineStatus(const QString& text, const QString& color_hex = QStringLiteral("#71717B"));
    void startMapLoadIfNeeded();
    void startProcessPointCloud();
    void startComputeHull();
    void startGenerateCoverage();
    void maybeRunAutotest();
    void logAutotestSummary(const QString& phase);
    void applyMapLoadResult(quint64 generation, const MapLoadResult& result);
    void applyProcessResult(quint64 generation, const ProcessResult& result);
    void applyHullResult(quint64 generation, const HullResult& result);
    void applyPlanningResult(quint64 generation, const PlanningResult& result);
    void startDetectObstacles();
    void applyDetectResult(quint64 generation, ObstacleDetectionResult result);

    // Scan Splitting
    void rebuildScanSegments();
    void refreshScanSegmentList();
    void pushScanSegmentsToPlot();
    // Lightweight refresh of the Stage 4 status-driven plot overlay.
    // Called from `setLiveRobotTelemetry` on every odom tick so the
    // active-segment split point follows the robot smoothly without
    // rebuilding the full segment geometry. No-op outside the Scan step.
    void refreshScanSegmentOverlay();
    void updateScanSplittingUi();
    std::vector<int> selectedScanSegmentIndices() const;
    PathStateList buildPublishPathFromSegments(const std::vector<int>& indices) const;
    void onSplitPathClicked();
    void onPublishSelectedClicked();
    void onStartSelectedClicked();
    void onScanDistanceEdited();
    void onProgressionModeChanged(const QString& mode);
    void invalidateScanSegments(const QString& status_message = QString());
    // Show the modal pre-scan checklist gate. Blurs the Stage 4 content
    // behind the dialog. Sets cache.scan_preflight_acknowledged = true on
    // confirm. Has no Cancel path — operator must confirm to proceed.
    void showScanPreflightDialog();

    // Scan execution (Stage 4)
    QWidget* buildScanPage(QWidget* parent);
    QWidget* buildScanCurrentSegmentCard(QWidget* parent);
    QWidget* buildScanOverallProgressCard(QWidget* parent);
    QWidget* buildScanTelemetryCard(QWidget* parent);
    QWidget* buildScanManualOverrideCard(QWidget* parent);
    QWidget* buildScanRightRail(QWidget* parent);
    QWidget* buildScanSegmentStatusCard(QWidget* parent);
    QWidget* buildScanStatisticsCard(QWidget* parent);
    QWidget* buildScanCenterControlBar(QWidget* parent);
    QWidget* buildScanStatusPill(QWidget* parent);
    QWidget* buildScanLegendChip(QWidget* parent);
    // Sets the footer next-stage button label AND resizes the button to
    // snug-fit the new text. Used for both the cross-stage "Stage N (...)"
    // forward CTA and the Stage 4 "Complete Mission" terminal CTA.
    void setNextStageLabel(const QString& text);
    // Generic snug-fit helper for the Scan center control bar buttons
    // (Start/Pause, Cancel/Discard, Emergency Stop). All three share the
    // same chrome (76 px) and Arimo bold 16 px stylesheet font.
    void setScanActionLabel(QPushButton* btn, QLabel* label, const QString& text);
    void updateScanRunUi();
    // Toggle a 0.55 opacity overlay on every Scan-stage value derived
    // from live robot telemetry while the link is offline. Called only
    // from setLinkConnectionState on state transitions.
    void setScanLabelsStale(bool stale);
    void updateScanLiveTelemetry();
    void refreshScanSegmentStatusList();
    void ensureScanSegmentSpinnerTimer();
    void tickScanSegmentSpinner();
    void onScanStartPauseClicked();
    void onScanEmergencyStopClicked();
    // Single dispatch slot for the dual-mode Cancel/Discard button. Branches
    // internally on run_state == Completed → Discard flow (post-completion
    // delete), else → Cancel flow (mid-scan abort).
    void onScanCancelClicked();
    void onScanDiscardClicked();
    void onCompleteMissionClicked(const char* trigger = "unspecified");
    void onScanFooterBackClicked();
    void onScanTick();
    void onScanManualTeleopTick();
    void syncScanManualTeleopTimer();
    void enterScanStage();
    int firstPendingSelectedSegmentIndex(int start_index = 0) const;
    bool publishSegmentForExecution(int segment_index);
    void startSegmentExecution(int segment_index, bool resume_action);
    void recomputeScanAggregateStats();
    void maybeScheduleScanQualityUpdate();
    void setScanManualOverride(bool active);
    void updateScanManualOverrideIndicator();
    bool scanManualTeleopAllowed() const;
    bool isDescendantOfScanCamera(const QWidget* widget) const;
    bool isDescendantOfScanMap(const QWidget* widget) const;
    void emitScanTeleopTwistCommand();
    void emitScanZeroTeleopTwist();
    void onScanCameraClicked();
    void onScanMapClicked();
    void stopScanCameraStream();
    void scheduleScanCameraRestart(const QString& reason);
    static double computeReprojectionQualityPercent(const PathStateList& segment_path,
                                                    const std::vector<Point2D>& trail);

    bool dark_mode_ = true;
    bool syncing_widgets_ = false;
    bool load_in_flight_ = false;
    bool process_in_flight_ = false;
    bool hull_in_flight_ = false;
    bool planning_in_flight_ = false;
    bool detect_in_flight_ = false;
    bool autotest_enabled_ = false;
    bool autotest_started_ = false;
    bool autotest_restore_reported_ = false;
    quint64 load_generation_ = 0;
    quint64 process_generation_ = 0;
    quint64 hull_generation_ = 0;
    quint64 planning_generation_ = 0;
    quint64 detect_generation_ = 0;
    PlannerStep current_step_ = PlannerStep::MapProcessing;
    QString active_context_key_;
    QString autotest_mode_;
    QString robot_id_;
    QString map_path_;
    QString top_signal_text_ = QStringLiteral("Signal unavailable");
    QString top_rec_text_ = QStringLiteral("REC ...");
    QString top_bot_text_ = QStringLiteral("BOT ...");
    QString top_lock_text_ = QStringLiteral("Not Ready");
    QString top_motors_text_ = QStringLiteral("DISARMED");
    ValueTone top_signal_tone_ = ValueTone::Muted;
    // Cached battery state — see ExplorationScreen for semantics.
    double top_battery_pct_ = 0.0;
    bool top_battery_stale_ = true;
    ValueTone top_rec_tone_ = ValueTone::Muted;
    ValueTone top_bot_tone_ = ValueTone::Muted;
    ValueTone top_lock_tone_ = ValueTone::Error;
    ValueTone top_motors_tone_ = ValueTone::Muted;
    bool link_connected_ = true;
    // Network-layer reachability mirror.  Only meaningful when
    // !link_connected_ — distinguishes Reconnecting (true) from
    // true Offline (false).  Defaults to true so the first
    // transition out of Healthy doesn't accidentally show the
    // OFFLINE banner before the reachability probe has reported.
    bool link_reachable_ = true;
    qint64 link_disconnected_since_ms_ = 0;
    std::optional<PathState> live_robot_pose_;
    std::vector<Point2D> live_robot_trail_;
    double robot_marker_size_m_ = 0.6;
    // Latest robot ground speed (m/s, unsigned). Pushed from AppShellWindow.
    double live_robot_speed_mps_ = 0.0;
    QHash<QString, SessionCache> session_cache_;
    PresetManager* preset_manager_ = nullptr;

    QWidget* top_bar_ = nullptr;
    QWidget* top_motors_chip_ = nullptr;
    QWidget* title_divider_ = nullptr;
    QWidget* stage_header_ = nullptr;
    QWidget* left_header_ = nullptr;
    QWidget* stage_row_host_ = nullptr;
    QWidget* stage_row_frame_ = nullptr;
    QWidget* left_header_icon_box_ = nullptr;
    QWidget* left_rail_ = nullptr;
    QWidget* output_section_ = nullptr;
    QWidget* placeholder_card_ = nullptr;
    QWidget* center_stage_ = nullptr;
    QWidget* stats_chip_ = nullptr;
    QWidget* footer_ = nullptr;

    QPushButton* btn_back_ = nullptr;
    QPushButton* btn_next_ = nullptr;
    QPushButton* tool_zoom_in_ = nullptr;
    QPushButton* tool_fit_ = nullptr;
    QPushButton* tool_reset_ = nullptr;
    QWidget* tool_stack_ = nullptr;
    QLabel* lbl_next_text_ = nullptr;
    QLabel* lbl_next_icon_ = nullptr;
    QLabel* lbl_back_icon_ = nullptr;
    QLabel* lbl_back_text_ = nullptr;
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_top_battery_ = nullptr;
    QLabel* lbl_top_signal_ = nullptr;
    QLabel* lbl_top_rec_ = nullptr;
    QLabel* lbl_top_bot_ = nullptr;
    QWidget* link_offline_banner_ = nullptr;
    QLabel* lbl_link_offline_text_ = nullptr;
    QLabel* lbl_top_lock_chip_ = nullptr;
    QLabel* lbl_top_motors_dot_ = nullptr;
    QLabel* lbl_top_motors_text_ = nullptr;
    QLabel* lbl_stage_footer_ = nullptr;
    QLabel* lbl_left_header_icon_ = nullptr;
    QLabel* lbl_left_header_title_ = nullptr;
    QLabel* lbl_left_header_subtitle_ = nullptr;
    QLabel* lbl_output_heading_ = nullptr;
    QLabel* lbl_placeholder_title_ = nullptr;
    QLabel* lbl_inline_status_ = nullptr;
    QLabel* lbl_output_points_ = nullptr;
    QLabel* lbl_output_reduction_ = nullptr;
    QLabel* lbl_output_file_size_ = nullptr;
    QLabel* lbl_output_quality_ = nullptr;
    QLabel* lbl_stats_points_ = nullptr;
    QLabel* lbl_stats_area_ = nullptr;
    QLabel* lbl_stage2_message_ = nullptr;
    QLabel* lbl_coverage_scan_complete_icon_ = nullptr;
    QLabel* lbl_coverage_scan_roi_icon_ = nullptr;
    QLabel* lbl_coverage_roi_rectangle_icon_ = nullptr;
    QLabel* lbl_coverage_roi_polygon_icon_ = nullptr;
    QLabel* lbl_coverage_roi_status_icon_ = nullptr;
    QLabel* lbl_coverage_roi_status_text_ = nullptr;
    QLabel* lbl_coverage_pattern_boustro_icon_ = nullptr;
    QLabel* lbl_coverage_pattern_snake_icon_ = nullptr;
    QLabel* lbl_coverage_pattern_spiral_icon_ = nullptr;
    QLabel* lbl_coverage_obstacle_auto_icon_ = nullptr;
    QLabel* lbl_coverage_obstacle_manual_icon_ = nullptr;
    QLabel* lbl_coverage_auto_info_icon_ = nullptr;
    QLabel* lbl_coverage_draw_rectangle_icon_ = nullptr;
    QLabel* lbl_coverage_draw_polygon_icon_ = nullptr;
    QLabel* lbl_coverage_draw_circle_icon_ = nullptr;
    QLabel* lbl_coverage_path_spacing_value_ = nullptr;
    QLabel* lbl_coverage_headland_value_ = nullptr;
    QLabel* lbl_coverage_scan_speed_value_ = nullptr;
    QLabel* lbl_coverage_generate_icon_ = nullptr;
    QLabel* lbl_coverage_generate_text_ = nullptr;
    QLabel* lbl_coverage_area_ = nullptr;
    QLabel* lbl_coverage_waypoints_ = nullptr;
    QLabel* lbl_coverage_path_length_ = nullptr;
    QLabel* lbl_coverage_est_time_ = nullptr;
    QLabel* lbl_coverage_obstacles_area_ = nullptr;
    QLabel* lbl_coverage_legend_boundary_ = nullptr;
    QLabel* lbl_coverage_legend_path_ = nullptr;

    PlannerTrackSlider* slider_voxel_ = nullptr;
    PlannerTrackSlider* slider_z_min_ = nullptr;
    PlannerTrackSlider* slider_z_max_ = nullptr;
    PlannerTrackSlider* slider_alpha_ = nullptr;
    PlannerTrackSlider* slider_coverage_path_spacing_ = nullptr;
    PlannerTrackSlider* slider_coverage_headland_ = nullptr;
    PlannerTrackSlider* slider_coverage_scan_speed_ = nullptr;

    // Scan-splitting "Distance per scan" QLineEdit always stores +
    // displays meters (operator's chosen splits live in SI on the wire
    // and in QSettings). When the operator selects ANSI in the New
    // Scan Information modal, lbl_scan_distance_ansi_hint_ becomes
    // visible right next to the field with the live "(\u2248 X.XX ft)"
    // conversion of the typed value. Hidden in Metric. Refreshed in
    // refreshScanDistanceAnsiHint(), called from
    // updateScanSplittingUi(), the field's textEdited signal, and the
    // UnitsProvider::unitsChanged connection.
    QLabel* lbl_scan_distance_ansi_hint_ = nullptr;

    // Tracks every slider min/max endpoint badge whose displayed
    // string depends on the units toggle. Populated during buildUi()
    // for each unit-bearing range block (Voxel / Z-Min / Z-Max / Path
    // Spacing / Headland / Robot Speed). relabelUnitEndpointBadges()
    // walks this vector on UnitsProvider::unitsChanged and rewrites
    // each label using units::formatLength or units::formatSpeed
    // according to `kind`.
    struct UnitEndpointBadge {
        enum class Kind { Length, Speed };
        QLabel* min_label = nullptr;
        QLabel* max_label = nullptr;
        double min_value = 0.0;
        double max_value = 0.0;
        Kind kind = Kind::Length;
        int decimals = 2;
    };
    std::vector<UnitEndpointBadge> unit_endpoint_badges_;
    QLabel* lbl_voxel_value_ = nullptr;
    QLabel* lbl_z_min_value_ = nullptr;
    QLabel* lbl_z_max_value_ = nullptr;
    QLabel* lbl_alpha_value_ = nullptr;
    QLabel* lbl_process_icon_ = nullptr;
    QLabel* lbl_process_text_ = nullptr;
    QLabel* lbl_hull_icon_ = nullptr;
    QLabel* lbl_hull_text_ = nullptr;
    QLabel* lbl_tool_zoom_in_icon_ = nullptr;
    QLabel* lbl_tool_fit_icon_ = nullptr;
    QLabel* lbl_tool_reset_icon_ = nullptr;

    QPushButton* btn_process_ = nullptr;
    QPushButton* btn_hull_ = nullptr;
    QPushButton* btn_coverage_scan_complete_ = nullptr;
    QPushButton* btn_coverage_scan_roi_ = nullptr;
    QPushButton* btn_coverage_roi_draw_rectangle_ = nullptr;
    QPushButton* btn_coverage_roi_draw_polygon_ = nullptr;
    QPushButton* btn_coverage_roi_start_ = nullptr;
    QPushButton* btn_coverage_roi_clear_ = nullptr;
    QPushButton* btn_coverage_preset_add_ = nullptr;
    QPushButton* btn_coverage_preset_save_ = nullptr;
    QPushButton* btn_coverage_preset_cancel_ = nullptr;
    QPushButton* btn_coverage_pattern_boustro_ = nullptr;
    QPushButton* btn_coverage_pattern_snake_ = nullptr;
    QPushButton* btn_coverage_pattern_spiral_ = nullptr;
    QPushButton* btn_coverage_axis_parallel_ = nullptr;
    QPushButton* btn_coverage_axis_perpendicular_ = nullptr;
    QPushButton* btn_coverage_obstacle_auto_ = nullptr;
    QPushButton* btn_coverage_obstacle_manual_ = nullptr;
    QPushButton* btn_coverage_detect_ = nullptr;
    // CSF "Detection Sensitivity" control (Auto-detect panel).
    PlannerTrackSlider* csf_sensitivity_slider_ = nullptr;
    QLabel* csf_sensitivity_value_ = nullptr;
    QPushButton* btn_coverage_clear_obstacles_ = nullptr;
    QPushButton* btn_coverage_draw_rectangle_ = nullptr;
    QPushButton* btn_coverage_draw_polygon_ = nullptr;
    QPushButton* btn_coverage_draw_circle_ = nullptr;
    QPushButton* btn_coverage_draw_toggle_ = nullptr;
    QPushButton* btn_coverage_generate_ = nullptr;
    PlotWidget* plot_ = nullptr;
    QStackedWidget* content_stack_ = nullptr;
    QStackedWidget* preview_stack_ = nullptr;
    QStackedWidget* preview_bottom_overlay_stack_ = nullptr;
    QStackedWidget* coverage_obstacle_mode_stack_ = nullptr;
    QWidget* map_processing_page_ = nullptr;
    QWidget* coverage_placeholder_page_ = nullptr;
    QWidget* scan_splitting_page_ = nullptr;
    QLineEdit* edit_scan_distance_ = nullptr;
    QPushButton* btn_progression_automatic_ = nullptr;
    QPushButton* btn_progression_manual_ = nullptr;
    QPushButton* btn_scan_split_path_ = nullptr;
    QPushButton* btn_scan_publish_selected_ = nullptr;
    QPushButton* btn_scan_start_selected_ = nullptr;
    QPushButton* btn_segments_select_all_ = nullptr;
    QPushButton* btn_segments_clear_all_ = nullptr;
    QListWidget* list_scan_segments_ = nullptr;
    QLabel* lbl_scan_segments_footer_ = nullptr;
    QLabel* lbl_scan_splitting_status_ = nullptr;
    QLabel* lbl_scan_split_path_icon_ = nullptr;
    QLabel* lbl_scan_publish_icon_ = nullptr;
    QLabel* lbl_scan_start_icon_ = nullptr;
    QWidget* scan_progression_toggle_ = nullptr;
    QWidget* preview_placeholder_ = nullptr;
    QWidget* coverage_save_preset_card_ = nullptr;
    QWidget* coverage_custom_presets_card_ = nullptr;
    QWidget* coverage_roi_section_ = nullptr;
    QWidget* coverage_roi_status_card_ = nullptr;
    QWidget* coverage_auto_info_card_ = nullptr;
    QWidget* coverage_manual_hint_card_ = nullptr;
    QWidget* coverage_obstacle_list_section_ = nullptr;
    QWidget* coverage_obstacle_area_card_ = nullptr;
    QWidget* coverage_legend_chip_ = nullptr;
    QWidget* coverage_legend_boundary_swatch_ = nullptr;
    QWidget* coverage_legend_path_swatch_ = nullptr;
    QComboBox* combo_coverage_presets_ = nullptr;
    QLineEdit* edit_coverage_preset_name_ = nullptr;
    QVBoxLayout* coverage_custom_presets_layout_ = nullptr;
    QVBoxLayout* coverage_obstacles_layout_ = nullptr;
    std::vector<QLabel*> label12_labels_;
    std::vector<QLabel*> label9_labels_;
    std::vector<QLabel*> label10_labels_;
    std::vector<QLabel*> heading10_labels_;
    std::vector<QLabel*> mono9_labels_;
    std::vector<QLabel*> mono12_muted_labels_;
    std::vector<QLabel*> mono12_white_labels_;
    std::vector<QLabel*> mono12_accent_labels_;

    struct StepperButton {
        QPushButton* button = nullptr;
        QLabel* icon = nullptr;
        QString icon_path;
    };
    std::vector<StepperButton> stepper_buttons_;
    void applyStepperButtonStyle(QPushButton* button) const;
    void refreshStepperButtons();
    std::vector<QWidget*> output_cards_;
    std::vector<QWidget*> stage_separator_widgets_;

    StageStepUi step_map_processing_;
    StageStepUi step_coverage_planning_;
    StageStepUi step_scan_splitting_;
    StageStepUi step_scan_;

    // Map card container — promoted to a member so the Scan stage can toggle
    // a border on/off when entering/leaving Stage 4.
    QWidget* preview_container_ = nullptr;

    // Stage 4 (Scan execution)
    QWidget* scan_page_ = nullptr;
    QWidget* scan_current_segment_card_ = nullptr;
    QWidget* scan_right_rail_ = nullptr;
    QWidget* scan_control_bar_ = nullptr;
    QWidget* scan_status_pill_ = nullptr;
    QWidget* scan_legend_chip_ = nullptr;
    QPushButton* footer_back_stage3_ = nullptr;
    QStackedWidget* footer_left_stack_ = nullptr;
    QLabel* lbl_scan_active_segment_ = nullptr;
    QProgressBar* bar_scan_active_progress_ = nullptr;
    QLabel* lbl_scan_active_progress_value_ = nullptr;
    QLabel* lbl_scan_active_quality_ = nullptr;
    QProgressBar* bar_scan_total_coverage_ = nullptr;
    QLabel* lbl_scan_total_coverage_value_ = nullptr;
    QProgressBar* bar_scan_total_quality_ = nullptr;
    QLabel* lbl_scan_total_quality_value_ = nullptr;
    QLabel* lbl_scan_time_value_ = nullptr;
    QLabel* lbl_scan_telemetry_speed_ = nullptr;
    QLabel* lbl_scan_telemetry_pos_x_ = nullptr;
    QLabel* lbl_scan_telemetry_pos_y_ = nullptr;
    QLabel* lbl_scan_telemetry_heading_ = nullptr;
    FPVCameraView* scan_camera_view_ = nullptr;
    QLabel* lbl_scan_manual_override_state_ = nullptr;
    QWidget* scan_segment_status_card_ = nullptr;
    QListWidget* list_scan_segment_status_ = nullptr;
    // Driven by `scan_segment_spinner_timer_`: rotates the active-segment
    // ring icon by 12° every 30 ms (full revolution ~900 ms). The label
    // is owned by the QListWidgetItem and may be destroyed when the list
    // refreshes, so we hold it via QPointer.
    QPointer<QLabel> scan_segment_spinner_label_;
    QTimer* scan_segment_spinner_timer_ = nullptr;
    int scan_segment_spinner_angle_ = 0;
    QLabel* lbl_scan_stats_distance_ = nullptr;
    QLabel* lbl_scan_stats_avg_quality_ = nullptr;
    QLabel* lbl_scan_stats_eta_ = nullptr;
    QLabel* lbl_scan_status_pill_dot_ = nullptr;
    QLabel* lbl_scan_status_pill_text_ = nullptr;
    QLabel* lbl_scan_legend_completed_ = nullptr;
    QLabel* lbl_scan_legend_active_ = nullptr;
    QLabel* lbl_scan_legend_pending_ = nullptr;
    QPushButton* btn_scan_start_pause_ = nullptr;
    QLabel* lbl_scan_start_pause_icon_ = nullptr;
    QLabel* lbl_scan_start_pause_text_ = nullptr;
    QLabel* lbl_scan_run_summary_ = nullptr;
    QPushButton* btn_scan_emergency_stop_ = nullptr;
    QLabel* lbl_scan_emergency_stop_icon_ = nullptr;
    QLabel* lbl_scan_emergency_stop_text_ = nullptr;
    QPushButton* btn_scan_cancel_ = nullptr;
    QLabel* lbl_scan_cancel_icon_ = nullptr;
    QLabel* lbl_scan_cancel_text_ = nullptr;
    QPushButton* btn_complete_mission_ = nullptr;
    QLabel* lbl_complete_mission_icon_ = nullptr;
    QLabel* lbl_complete_mission_text_ = nullptr;
    QTimer* scan_tick_timer_ = nullptr;
    QTimer* scan_manual_teleop_timer_ = nullptr;
    QTimer* scan_camera_restart_timer_ = nullptr;
    QFutureWatcher<double>* scan_quality_watcher_ = nullptr;
    int scan_quality_segment_index_ = -1;
    qint64 scan_last_quality_sample_ms_ = 0;
    size_t scan_active_segment_path_hint_ = 0;
    bool scan_estop_latched_ = false;
    bool scan_pause_explicit_ = false;
    bool scan_manual_override_active_ = false;
    // True between segment_complete (final waypoint reached) and segment_saved
    // (controller confirmed /dc/end_and_save returned). Blocks the next-segment
    // publish so the GP8800 actuator finishes its retract cycle before any
    // new /dc/start fires. Also drives the "Saving…" pill + Continue lockout.
    bool scan_dc_save_in_flight_ = false;
    int scan_pending_next_segment_idx_ = -1;
    bool scan_manual_override_engaged_once_ = false;
    // Set true between the operator confirming Cancel Scan and AppShell
    // calling notifyScanCancelled(). Used to disable the Cancel button so
    // the operator can't double-fire it while the controller is rmtree'ing.
    bool scan_cancel_in_flight_ = false;
    // Same idea for the post-Completed Discard flow. Distinct from
    // scan_cancel_in_flight_ because Discard has retry-once + a different
    // terminal UI state (button locks to "Discarded" instead of resetting).
    bool scan_discard_in_flight_ = false;
    // Latches true once Discard succeeds (or terminally fails after retry).
    // Drives the post-discard Stage-4 visual: button shows "Discarded" grey
    // and disabled, status pill reads "Discarded", segment list cleared,
    // planned path + last stats kept on screen for context. Cleared when
    // the operator presses Complete Mission and Stage 5 tears down.
    bool scan_discarded_ = false;
    // True only when Discard's retry path also failed. Logged by AppShell;
    // UI still latches to "Discarded" so the operator isn't stuck.
    bool scan_discard_failed_ = false;
    bool scan_manual_resume_after_override_ = false;
    bool scan_camera_stream_requested_ = false;
    bool scan_key_w_down_ = false;
    bool scan_key_a_down_ = false;
    bool scan_key_s_down_ = false;
    bool scan_key_d_down_ = false;
    double scan_teleop_linear_speed_mps_ = 0.4;
    double scan_teleop_angular_speed_rps_ = 1.0;
    double scan_teleop_angular_speed_step_rps_ = 0.1;
    double scan_teleop_angular_speed_min_rps_ = 0.0;
    double scan_teleop_angular_speed_max_rps_ = 4.5;
    QDateTime last_telemetry_ts_;
    Point2D last_telemetry_xy_{0.0, 0.0};
    bool last_telemetry_valid_ = false;
};

}  // namespace f2c_cpp
