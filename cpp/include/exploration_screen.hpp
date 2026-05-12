#pragma once

#include <QByteArray>
#include <QString>
#include <QWidget>

class QEvent;
class QFocusEvent;
class QHideEvent;
class QLabel;
class QKeyEvent;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QResizeEvent;
class QStackedWidget;
class QTimer;
class QGraphicsBlurEffect;

namespace f2c_cpp {

class ExplorationLoadingOverlayWidget;
class ExplorationNavMapWidget;
class ExplorationThermalPixelWidget;
class VideoStreamWidget;

class ExplorationScreen : public QWidget {
    Q_OBJECT

public:
    enum class ValueTone {
        Good,
        Warning,
        Muted,
        Error
    };

    explicit ExplorationScreen(QWidget* parent = nullptr);

    void setDarkMode(bool dark_mode);
    void setLaunchInProgress(bool in_progress);
    void setLaunchProgress(int percent, const QString& status_text);
    void setLaunchDiagnostics(const QString& diagnostics_text);
    void setLaunchReady(bool ready);
    void resetMappingWorkflowUi();
    void beginMappingRunLock(int min_seconds = 60);
    void setPrimaryActionReadyToFinish();
    void showMapSaveInProgress();
    void showMapDownloadInProgress();
    void showMapReady();
    void setPlanningEnabled(bool enabled);
    void setTopLockChipState(const QString& text, ValueTone tone);
    void setTopSignalState(const QString& text, ValueTone tone);
    void setTopRecPillState(const QString& text, ValueTone tone);
    // BOT pill: drives the LinkHealthMonitor state to a top-bar chip
    // identical in shape to the Signal / REC pills.  AppShellWindow
    // pushes this on every linkStateChanged transition.  Tone semantics
    // match the existing pills: Good=green LIVE, Warning=amber LAGGY,
    // Error=red OFFLINE, Muted=grey IDLE.
    void setBotLinkPillState(const QString& text, ValueTone tone);
    // Disconnect surface: shows / hides the inline "Robot offline"
    // banner above the launch progress card and gates the primary
    // controls (Start Mapping, Finish + Save Map, Stop Pipeline) so
    // the operator can't fire RPCs that we already know will be
    // dropped.  When `connected` flips back to true after >= 500 ms
    // of being false, controls re-enable on the next event-loop tick
    // (the AppShell's reconnect handshake does the actual debounce).
    void setLinkConnectionState(bool connected, qint64 since_ms);
    void setTopMotorsChipState(const QString& text, ValueTone tone);
    void setTelemetrySpeedMps(double speed_mps);
    void setTelemetryPositionMeters(double x_m, double y_m);
    void setTelemetryAltitudeMeters(double z_m);
    void setTelemetryScanTimeSeconds(int elapsed_seconds);
    void setFpvSpeedMps(double speed_mps);
    void setThermalSummary(double max_c,
                           double avg_c,
                           double min_c,
                           const QString& state_text,
                           ValueTone state_tone);
    void setThermalUnavailable();
    void setThermalHidden();
    void setThermalThumbnailData(const QByteArray& packed_thumb);
    void setThermalThumbnailStale(bool stale);
    void resetNavigationMap();
    void setNavigationMapData(const QByteArray& packed_grid);
    void setNavigationMapYawRadians(double yaw_rad);
    void setNavigationMapStale(bool stale);
    void setHardwareStatus(const QString& motors_text,
                           ValueTone motors_tone,
                           const QString& lidar_text,
                           ValueTone lidar_tone,
                           const QString& rf_text,
                           ValueTone rf_tone,
                           const QString& storage_text,
                           ValueTone storage_tone);
    void startFpvStream(int port = 5600);
    void stopFpvStream();
    void forceTeleopStop();

    // Stage 6 freeze detector: wall-clock ms of the most recent FPV
    // frame.  0 if no frames yet OR the stream isn't running.  Polled
    // by AppShellWindow's slow tick to stamp
    // LinkHealthMonitor::Source::FpvFrame.
    qint64 lastFpvFrameWallMs() const;

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;

signals:
    void backRequested();
    void startScanRequested();
    void finishSaveMapRequested();
    void startPlanningRequested();
    void stopPipelineRequested();
    void teleopTwistRequested(double linear_x, double angular_z);
    void teleopArmRequested();
    void teleopDisarmRequested();
    void teleopGprPowerOffRequested();

private slots:
    void onDashboardClicked();
    void onStartScanClicked();
    void onStartPlanningClicked();
    void onStopPipelineClicked();
    void onMappingLockTick();
    void onTeleopPublishTick();

private:
    enum class PrimaryActionState {
        StartMapping,
        RunningLocked,
        FinishAndSaveMap,
        SavingMap,
        DownloadingMap,
        MapReady
    };

    void buildUi();
    void applyStyle();
    void setPrimaryActionState(PrimaryActionState state);
    void refreshPrimaryActionButton();
    // Sets the primary action button label AND resizes the button to snug-fit
    // the new text via QFontMetrics (chrome + advance + safety pad). Used by
    // every state transition in refreshPrimaryActionButton(). The per-second
    // mapping-lock timer deliberately bypasses this helper — see
    // onMappingLockTick() — so the button width stays constant during the
    // 60 s lock instead of nudging on each digit rollover.
    void setPrimaryActionLabel(const QString& text);
    void setLoadingOverlayVisible(bool visible);
    void updateLoadingOverlayGeometry();
    void updateLoadingOverlayText();
    void setFpvControlActive(bool active);
    void updateFpvControlIndicator();
    void emitTeleopTwistCommand();
    void emitZeroTeleopTwist();
    void applyToneToLabel(QLabel* label, ValueTone tone, bool emphasize);
    void applyTopStatusToneToLabel(QLabel* label, ValueTone tone, bool emphasize);
    void updateTopMotorsChipGeometry();
    bool teleopMovementAllowed() const;
    bool isDescendantOfFpv(const QWidget* widget) const;

    bool dark_mode_ = false;
    bool launch_in_progress_ = false;
    bool launch_ready_ = false;
    bool planning_enabled_ = false;
    bool primary_action_enabled_by_state_ = true;
    int launch_progress_percent_ = 0;
    QString launch_status_text_ = QStringLiteral("Waiting for Start Scan");
    QString top_signal_text_ = QStringLiteral("Signal unavailable");
    QString top_rec_text_ = QStringLiteral("REC ...");
    QString top_bot_text_ = QStringLiteral("BOT ...");
    QString top_lock_text_ = QStringLiteral("Standby");
    QString top_motors_text_ = QStringLiteral("DISARMED");
    PrimaryActionState primary_action_state_ = PrimaryActionState::StartMapping;
    qint64 mapping_lock_started_at_ms_ = 0;
    int mapping_lock_duration_sec_ = 60;
    ValueTone top_signal_tone_ = ValueTone::Muted;
    ValueTone top_rec_tone_ = ValueTone::Muted;
    ValueTone top_bot_tone_ = ValueTone::Muted;
    ValueTone top_lock_tone_ = ValueTone::Muted;
    ValueTone top_motors_tone_ = ValueTone::Muted;
    bool link_connected_ = true;
    qint64 link_disconnected_since_ms_ = 0;

    QPushButton* btn_dashboard_ = nullptr;
    QPushButton* btn_start_scan_ = nullptr;
    QLabel* btn_start_scan_text_ = nullptr;
    QPushButton* btn_start_planning_ = nullptr;
    QLabel* btn_start_planning_text_ = nullptr;
    QLabel* lbl_top_battery_ = nullptr;
    QLabel* lbl_top_signal_ = nullptr;
    QLabel* lbl_top_rec_ = nullptr;
    QLabel* lbl_top_bot_ = nullptr;
    QLabel* lbl_top_lock_chip_ = nullptr;
    QLabel* lbl_top_motors_dot_ = nullptr;
    QLabel* lbl_top_motors_text_ = nullptr;
    QLabel* lbl_telemetry_speed_ = nullptr;
    QLabel* lbl_telemetry_position_ = nullptr;
    QLabel* lbl_telemetry_altitude_ = nullptr;
    QLabel* lbl_telemetry_scan_time_ = nullptr;
    QLabel* lbl_fpv_speed_overlay_ = nullptr;
    QWidget* top_motors_chip_ = nullptr;
    QWidget* thermal_summary_panel_ = nullptr;
    ExplorationNavMapWidget* nav_map_widget_ = nullptr;
    ExplorationThermalPixelWidget* thermal_pixel_widget_ = nullptr;
    QLabel* lbl_thermal_state_ = nullptr;
    QLabel* lbl_thermal_max_ = nullptr;
    QLabel* lbl_thermal_avg_ = nullptr;
    QLabel* lbl_thermal_stale_ = nullptr;
    QLabel* lbl_hw_motors_ = nullptr;
    QLabel* lbl_hw_lidar_ = nullptr;
    QLabel* lbl_hw_rf_ = nullptr;
    QLabel* lbl_hw_storage_ = nullptr;
    QLabel* lbl_fpv_control_state_ = nullptr;
    QPushButton* btn_stop_pipeline_ = nullptr;
    QLabel* lbl_standby_ = nullptr;
    QWidget* launch_progress_card_ = nullptr;
    QLabel* lbl_launch_percent_ = nullptr;
    QLabel* lbl_launch_status_ = nullptr;
    QPlainTextEdit* launch_diagnostics_view_ = nullptr;
    QProgressBar* launch_progress_bar_ = nullptr;
    QTimer* mapping_lock_timer_ = nullptr;
    QTimer* teleop_publish_timer_ = nullptr;
    QWidget* content_root_ = nullptr;
    ExplorationLoadingOverlayWidget* loading_overlay_ = nullptr;
    QGraphicsBlurEffect* content_blur_effect_ = nullptr;
    bool loading_overlay_visible_ = false;
    QStackedWidget* fpv_media_stack_ = nullptr;
    QWidget* fpv_placeholder_ = nullptr;
    VideoStreamWidget* fpv_stream_widget_ = nullptr;
    QWidget* fpv_focus_target_ = nullptr;
    bool fpv_control_active_ = false;
    bool key_w_down_ = false;
    bool key_a_down_ = false;
    bool key_s_down_ = false;
    bool key_d_down_ = false;
    double teleop_linear_speed_mps_ = 0.4;
    double teleop_angular_speed_rps_ = 1.0;
    double teleop_angular_speed_step_rps_ = 0.1;
    double teleop_angular_speed_min_rps_ = 0.0;
    double teleop_angular_speed_max_rps_ = 4.5;
};

}  // namespace f2c_cpp

