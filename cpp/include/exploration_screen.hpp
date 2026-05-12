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
class CameraSwitchPill;

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
    // Top-bar battery pill — fed by AppShell from the dashboard's MQTT
    // battery subscriber.  `pct` is the latest SoC %; `stale` is true
    // when the dashboard has no fresh payload (no MQTT yet, broker
    // unreachable, or payload older than its stale_after_ms).  When
    // `stale` the pill renders as "—%" in muted color; otherwise the
    // pill mirrors the dashboard tile thresholds (≤12 % red,
    // ≤25 % amber, otherwise green).
    void setTopBatteryState(double pct, bool stale);
    void setTopRecPillState(const QString& text, ValueTone tone);
    // BOT pill: drives the LinkHealthMonitor state to a top-bar chip
    // identical in shape to the Signal / REC pills.  AppShellWindow
    // pushes this on every linkStateChanged transition.  Tone semantics
    // match the existing pills: Good=green LIVE, Warning=amber
    // RECONNECTING, Error=red OFFLINE, Muted=grey IDLE.
    void setBotLinkPillState(const QString& text, ValueTone tone);
    // Disconnect surface, layered LinkHealthMonitor model:
    //
    //   connected=true  -> Healthy.  No banner, no halo, controls
    //                      enabled.  `reachable` ignored.
    //   connected=false, reachable=true  -> Reconnecting.  Hide the
    //                      banner + nav-map halo (the host is still
    //                      on the network — soft visual treatment),
    //                      but gate the primary controls so the
    //                      operator can't fire dropped RPCs.
    //   connected=false, reachable=false -> True OFFLINE.  Banner +
    //                      halo + control lockout.
    //
    // AppShell calls this on every LinkHealthMonitor transition.
    void setLinkConnectionState(bool connected, bool reachable, qint64 since_ms);
    void setTopMotorsChipState(const QString& text, ValueTone tone);
    void setTelemetrySpeedMps(double speed_mps);
    void setTelemetryPositionMeters(double x_m, double y_m);
    void setTelemetryAltitudeMeters(double z_m);
    void setTelemetryScanTimeSeconds(int elapsed_seconds);
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

    // CameraSwitchPill control surface — AppShell drives this via the
    // /stream_camera_status callback so the pill's active button
    // reflects the robot's confirmed state.  `cam` is "left" or
    // "right"; anything else is silently ignored.  Safe to call
    // before the pill widget is constructed (during early Stage 4
    // build) — the value is cached and applied when the widget exists.
    void setStreamingCamera(const QString& cam);

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
    // Operator clicked the Stage 4 FPV camera-switch pill.
    // AppShell forwards to a /stream_camera_select publish and
    // persists to QSettings.  `cam` is "left" or "right".
    void cameraSelectRequested(const QString& cam);

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
    // Cached battery state (driven by AppShell's mirror of the
    // dashboard MQTT subscriber).  Initial state is "stale" so the
    // pill shows "—%" until the first dashboard sample arrives —
    // never the "87%" placeholder the QLabel was constructed with.
    double top_battery_pct_ = 0.0;
    bool top_battery_stale_ = true;
    ValueTone top_rec_tone_ = ValueTone::Muted;
    ValueTone top_bot_tone_ = ValueTone::Muted;
    ValueTone top_lock_tone_ = ValueTone::Muted;
    ValueTone top_motors_tone_ = ValueTone::Muted;
    bool link_connected_ = true;
    // Network-layer reachability mirror.  Only meaningful when
    // !link_connected_ — distinguishes Reconnecting (true) from
    // true Offline (false).  Defaults to true so the first
    // transition out of Healthy doesn't accidentally show the
    // OFFLINE banner before the reachability probe has reported.
    bool link_reachable_ = true;
    qint64 link_disconnected_since_ms_ = 0;
    // Top bar widget pointer kept so the disconnect banner can be
    // inserted right below it on first disconnect (lazy construction).
    QWidget* top_bar_ = nullptr;
    QWidget* link_offline_banner_ = nullptr;
    QLabel* lbl_link_offline_text_ = nullptr;

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
    // Stage 4 FPV camera-switch pill (left/right toggle).  Lives in
    // its own dark strip BELOW the FPV widget — NOT as an overlay over
    // the GStreamer surface.  Putting it in a sibling layout instead
    // of QStackedLayout(StackAll) avoids the well-known Qt5 issue
    // where overlay widgets over a native video sink composite
    // unreliably (intermittent invisibility, repaint residue, etc.).
    CameraSwitchPill* camera_switch_pill_ = nullptr;
    // Container around the pill — fixed-height dark strip per Figma
    // (rgba(24,24,27,0.95), 1 px top border #27272a, 61 px tall).
    QWidget* camera_switch_strip_ = nullptr;
    // Cached operator preference applied as soon as the pill exists,
    // so AppShell can call setStreamingCamera before Stage 4 is built.
    QString pending_streaming_camera_;
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

