#pragma once

#include <QByteArray>
#include <QMainWindow>
#include <QPoint>
#include <QProcess>
#include <QString>
#include <QtGlobal>
#include <functional>
#include <vector>

#include <geometry_msgs/msg/twist.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <odrive_can/msg/controller_status.hpp>
#include <odrive_can/srv/axis_state.hpp>
#include <rcl_interfaces/srv/set_parameters.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/float64_multi_array.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <std_srvs/srv/trigger.hpp>

#include "link_health_monitor.hpp"
#include "robot_reachability_probe.hpp"
#include "robot_registry.hpp"

class QResizeEvent;

class QLabel;
class QEvent;
class QFrame;
class QProcess;
class QPropertyAnimation;
class QStackedWidget;
class QTimer;
class QToolButton;
class QWidget;

namespace f2c_cpp {

class DashboardScreen;
class ExplorationScreen;
class PlannerScreen;
class SetupScreen;
class StartupScreen;
class UpdateBanner;
class RollbackBanner;
class UploadDialog;

namespace update {
class UpdateChecker;
struct VersionInfo;
}

class AppShellWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit AppShellWindow(QWidget* parent = nullptr);
    ~AppShellWindow() override;

    /// Show the Phase 9 rollback advisory banner. Driven by the
    /// startup-time marker dispatch in `main.cpp` when the previous OCU
    /// session ended in a rolled-back update. Idempotent.
    /// @param message Optional custom subtitle. Empty falls back to the
    ///                generic "previous version restored" copy.
    void showRolledBackBanner(const QString& message = {});

signals:
    /// Emitted once after construction completes and the Qt event loop
    /// has processed at least one tick. Phase 9's startup watchdog
    /// connects to this to mark `update_state.json` as `done` and stop
    /// the 60 s rollback timer. Firing is gated on the event loop being
    /// alive so that "ctor returns but app is wedged" is correctly
    /// classified as unhealthy.
    void bootHealthy();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    // closeEvent: gated shutdown for the OCU's main window.
    // If the exploration launch tree is alive on the robot (operator
    // already pressed Start Mapping / past Stage 4), the close is
    // intercepted and a confirmation dialog is shown.  Acceptance runs
    // the same SYNCHRONOUS teardown as Complete Mission so the robot's
    // unified_data_collector gets a chance to call
    // seekcamera_manager_destroy() before the OCU process exits.
    // Without this, hitting the window X mid-scan would leave UDC alive
    // on the robot AND the Seek SDK in a wedged state for the next OCU
    // session (the bug we fixed in commit 283ac26 — but the fix only
    // applied to the Complete Mission teardown path).
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onLoginSubmitted(const QString& robotId, const QString& accessCode);
    void goToStage1();
    void goToStage2();
    void goToStage3();
    void goToStage4();
    void goToStage5();
    void onThemeToggleChanged(bool dark_mode);

    void onStartNewScan();
    /// Stage 3 quick-action: "Upload Data" button. Opens the modal
    /// `UploadDialog` which SSH-probes the robot's `/R_DATA/` and
    /// streams `pilot_control/scripts/uploader.py` over SSH for any
    /// section/mission the operator selects. Hard-blocks while a
    /// scan/launch tree is alive (same `launch_active` rule the close
    /// guard uses) — operators must finish the mission first.
    void onUploadDataRequested();
    void onExplorationStartScanRequested();
    void onExplorationFinishSaveMapRequested();
    void onExplorationStartPlanningRequested();
    void onExplorationStopPipelineRequested();
    void onExplorationTeleopTwistRequested(double linear_x, double angular_z);
    void onExplorationTeleopArmRequested();
    void onExplorationTeleopDisarmRequested();
    void onExplorationTeleopGprPowerOffRequested();
    void onPlannerPublishScanSegmentsRequested(const std::vector<double>& xy_pairs);
    void onPlannerStartScanSegmentsRequested(const QString& progression_mode);
    // Carries the cruise speed (m/s) the planner UI's slider was set to at
    // start-scan time. Pushed straight into the MPC controller's
    // `max_linear_velocity` ROS param via sendControllerMaxLinearVelocity()
    // before autonomy is armed.
    void onPlannerScanStartRequested(double speed_mps);
    void onPlannerScanPauseRequested();
    void onPlannerScanResumeRequested();
    // Operator pressed the Wake GPR button inside the pre-scan checklist
    // dialog. Forwards to /gpr_line_stop Trigger (the Arduino bridge
    // interprets that as the wake-from-sleep keystroke).
    void onPlannerWakeGprRequested();
    void onPlannerEmergencyStopRequested();
    void onPlannerCompleteMissionRequested();
    void onPlannerCancelScanRequested();
    void onPlannerDiscardScanRequested();
    void onExplorationLaunchPoll();
    void onExplorationLiveFastTick();
    void onExplorationLiveSlowTick();

private:
    void setDarkMode(bool dark_mode);
    /// Apply the current theme's window background to a banner margin host
    /// (object-name-scoped so it doesn't cascade into the banner card).
    /// Used by both the OTA and rollback banner hosts so the title-bar band
    /// behind the floating window controls matches the active theme.
    void applyBannerHostTheme(QWidget* host);
    /// Build a fresh GateState (battery, transfers, uploads, mission) and
    /// open the OTA "What's New" modal centered on this window. Called
    /// from the UpdateBanner::viewDetailsRequested handler. The modal is
    /// app-modal but parented to `this` so it tracks dark-mode at open
    /// time. Phase 6 = signal-only Install Now (Q3=A).
    void showUpdateModal(const update::VersionInfo& info);

    /// Phase 7 OCU→runner handoff (locked Q1=A CLI args, Q3=B lockfile-
    /// gated wait, concerns #2 + #3). Spawns bdr-update-runner detached
    /// with the version info encoded as CLI flags, polls for the runner's
    /// lockfile to be held, then qApp->quit(). On spawn failure or wait
    /// timeout the modal stays open with an error message and the OCU
    /// keeps running (Install Now stays disabled until reopened).
    /// `modal_window` is the still-open UpdateModal — used to surface
    /// errors back to the operator if the handoff fails.
    void handoffToUpdateRunner(const update::VersionInfo& info,
                               QWidget* modal_window);
    void ensureStage2();
    void ensureStage3();
    void ensureStage4();
    void ensureStage5();
    void setupWindowControls();
    void updateWindowControlsPosition();
    void updateWindowControlsTheme();
    void updateWindowControlsToggleUi();
    void onWindowThemeToggleClicked();
    void onWindowMinimizeClicked();
    void onWindowMaximizeClicked();
    void onWindowCloseClicked();
    void ensureExplorationRosInterfaces();
    void startLaptopTeleopLaunch(const QString& robot_host);
    void startRobotCompleteLaunch(const ResolvedRobotSshTarget& ssh_target);
    void setExplorationLaunchFailed(const QString& reason);
    void publishExplorationStreamTarget();
    bool requestSavedMapPathWithRetry(QString* saved_remote_path, QString* error_message);
    QString resolveLatestCorrectedMapUnderDataRoot(QString* error_message) const;
    void beginCorrectedMapResolveAndDownload(int retries_remaining);
    QString extractSavedMapPath(const QString& save_message) const;
    void clearPendingSavedMapState();
    void failSavedMapWorkflow(const QString& progress_message, const QString& diagnostic_message);
    QString localMapDownloadPathForRemote(const QString& remote_map_path) const;
    void startSavedMapDownload(const QString& remote_map_path, int retries_remaining);
    void updateExplorationProgressUi();
    QString buildExplorationDiagnostics(const QString& headline = QString()) const;
    void publishExplorationTeleopTwist(double linear_x, double angular_z);
    void sendExplorationAxisStateRequest(int requested_state);
    void sendExplorationGprPowerOffRequest();
    // Sends a /gpr_line_stop Trigger. The Arduino bridge translates this
    // into the wake-from-sleep keystroke on the GPR control panel. Used
    // by the pre-scan checklist dialog's Wake GPR button. Fire-and-forget
    // — no acknowledgment from the GPR is observable, so the operator
    // can press multiple times if needed.
    void sendGprLineStopRequest();
    // Asks the coordinator to stop continuous mission GNSS logging and
    // finalize mission_config.json, then invokes `done()` on the Qt thread.
    // Always invokes `done()` exactly once — on success, on service error,
    // or on the 6 s safety ceiling — so the Complete Mission state machine
    // always advances to pipeline teardown.
    void finalizeMissionDataCollection(std::function<void()> done);
    void beginExplorationMotorsIdleWait(std::function<void(bool timed_out)> continuation);
    void performExplorationPipelineTeardownPreamble();
    void explorationStopPipelineTeardownKillProcessesAndResetUi();
    void performExplorationPipelineTeardown();
    // Asks the coordinator to abort the active scan: stop DC, delete every
    // section folder this mission produced (including the in-flight one),
    // and remove the entire mission folder (continuous UBX + config). The
    // pipeline launch processes are NOT torn down — they keep running so
    // the operator can immediately re-plan and start a new mission. Always
    // invokes `done(success)` exactly once on the Qt thread, even on the 8 s
    // safety ceiling, so the planner UI always advances.
    void cancelActiveScanDataCollection(std::function<void(bool)> done);
    // Wraps cancelActiveScanDataCollection with one automatic retry on
    // failure. Used for the post-Completed Discard Scan flow where the
    // operator has explicitly chosen to throw away a finished mission and
    // we want a best-effort second attempt before reporting failure.
    // `done(success)` always invoked exactly once on the Qt thread.
    void discardCompletedScanDataCollection(std::function<void(bool)> done);
    QString detectLocalIP() const;
    ResolvedRobotSshTarget resolveRobotSshForRemoteOps() const;
    bool isLocalProcessRunning(const QString& process_name) const;
    /// True if the laptop-side Zenoh daemon from `laptop_teleop.launch.py` is running
    /// (`zenohd -c /tmp/zenohd_laptop_<robot_ip>.json5`). Uses `pgrep -f` so we don't rely
    /// on `/proc/*/comm` being exactly `zenohd` (that made `pgrep -x zenohd` false negatives).
    bool localZenohLaptopBridgeRunning() const;
    bool isRobotPipelineRunning(const ResolvedRobotSshTarget& ssh_target) const;
    void onExplorationStreamStatus(const std_msgs::msg::String::SharedPtr msg);
    void onExplorationOdom(const nav_msgs::msg::Odometry::SharedPtr msg);
    void onExplorationLocalNavGrid(const std_msgs::msg::UInt8MultiArray::SharedPtr msg);
    void onExplorationThermalThumb(const std_msgs::msg::UInt8MultiArray::SharedPtr msg);
    void onExplorationThermalSummary(const std_msgs::msg::String::SharedPtr msg);
    void onPlannerScanExecutionStatus(const std_msgs::msg::String::SharedPtr msg);
    void onExplorationLeftControllerStatus(const odrive_can::msg::ControllerStatus::SharedPtr msg);
    void onExplorationRightControllerStatus(const odrive_can::msg::ControllerStatus::SharedPtr msg);
    void updateExplorationMotorSampleFromLatest();
    void publishPlannerAutonomyEnable(bool enabled);
    void callPlannerTriggerService(
        const rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr& client);
    void loadPreviousScanMotorBaseline();
    void persistCurrentScanMotorBaseline();
    void loadExplorationRfConfigForActiveRobot();
    void requestExplorationRfProbe();
    void handleExplorationRfProbeFinished(int exit_code, QProcess::ExitStatus exit_status);
    void requestExplorationStorageProbe();
    void handleExplorationStorageProbeFinished(int exit_code, QProcess::ExitStatus exit_status);
    void pushExplorationTelemetryToUiFast();
    void pushExplorationTelemetryToUiSlow();
    void pushPlannerTelemetrySnapshot();
    void pushExplorationTopMotorsChipState();
    void pushPlannerMotorsChipState();

    // Mirrors the dashboard's MQTT battery state onto the Stage 4 /
    // Stage 5 top-bar pills.  Slot connected to
    // DashboardScreen::batterySocChanged; caches the latest sample so
    // goToStage4()/goToStage5() can seed the pill on transition.
    void onDashboardBatteryStateChanged(double pct, bool stale);
    void onUdcHealthMessage(const std_msgs::msg::String::SharedPtr msg);
    void pushUdcRecPillState();
    bool isUdcDeadOrWedged() const;

    // Streaming-camera selection (Stage 4 FPV pill).  See
    // CameraSwitchPill — the operator picks which onboard camera UDC
    // streams over RTP.  AppShell owns the publisher (so we can
    // coalesce with the per-launch first-status push) and forwards
    // confirmation back to ExplorationScreen.
    void onExplorationCameraSelectRequested(const QString& cam);
    void publishStreamCameraSelection(const QString& cam);
    void onStreamCameraStatus(const std_msgs::msg::String::SharedPtr msg);

    // Link-health resilience (Stages 0-3 of the disconnect-resilience
    // feature).  link_monitor_ owns all "is the robot reachable?"
    // truth.  AppShell stamps freshness from existing ROS callbacks and
    // routes state-change events to:
    //   - PlannerScreen / ExplorationScreen top-bar BOT pill + disconnect
    //     banner + scan-tick clock pause + button lockout
    //   - showCommandDroppedToast() when an RPC is intentionally dropped
    //     because the link is offline (today's silent qWarning lines).
    //   - re-sync hook on Disconnected -> Healthy transitions to
    //     re-publish autonomy / cruise speed in case the robot rebooted.
    // Armed when the exploration launch starts; disarmed at teardown.
    void onLinkStateChanged(LinkHealthMonitor::State old_state,
                            LinkHealthMonitor::State new_state,
                            qint64 since_ms);
    // Toast: small, auto-dismissing overlay anchored to the AppShell.
    // Used for "command dropped" hints when RPCs are skipped because
    // the link is offline.  Single-queue with a 1 s dedup window so a
    // mashed E-Stop button doesn't stack toasts.
    void showCommandDroppedToast(const QString& message);
    void hideCommandDroppedToast();
    // Returns true when the LinkHealthMonitor is armed AND we should
    // hard-block any RPC to the robot.  Covers BOTH true OFFLINE
    // (probe + ROS topics both gone — host unreachable) AND
    // RECONNECTING (ROS topics stale but probe says host is up).
    // Used as a gate by the planner RPC paths so we don't
    // optimistically mutate local state (planner_estop_active_, etc.)
    // when the robot can't possibly have received the command.
    bool isRobotLinkOffline() const;
    // Strict variant: true ONLY when the link is truly unreachable
    // (probe failed AND ROS topics stale).  Used by the Complete
    // Mission flow to decide between the normal RPC path and the
    // OfflineFinalizeDialog — RECONNECTING shouldn't pop the dialog
    // because the link might recover in seconds.
    bool isRobotLinkUnreachable() const;
    // Re-publish all "soft" state the robot needs to be in sync with
    // the OCU's current model.  Called once per Disconnected -> Healthy
    // transition so that a bot that rebooted mid-disconnect lands back
    // in the operator-intended autonomy / cruise-speed configuration
    // without requiring a manual click.
    void onRobotLinkRecovered();

    // Stage 4 data-first Complete Mission helpers.
    //
    // executeCompleteMissionNormalPath() is the existing Healthy/
    // Degraded code path: motor-disarm wait, /dc/finalize_mission RPC,
    // teardown, return to Stage 3.  Extracted from
    // onPlannerCompleteMissionRequested so the offline path can call
    // it directly when the link recovers mid-modal.
    //
    // executeCompleteMissionSshFallback() runs the SSH-offline finaliser
    // (`finalize_mission_local.py` on the robot), then the existing
    // teardown SSH script (which kills the controller -> motors disarm
    // naturally), then returns to Stage 3.  The robot ends up with
    // mission_finalized_at + finalized_via=ssh_offline written to disk,
    // so post-flight tooling can flag the mission for review without
    // confusing it for an abandoned one.
    void executeCompleteMissionNormalPath();
    void executeCompleteMissionSshFallback();

    // Push the operator-selected cruise speed to
    // mpc_accel_autonomous_controller's `max_linear_velocity` /
    // `desired_linear_speed` ROS params via a synchronous SetParameters
    // call (wait_for_service + spin_until_future_complete). The client
    // is built fresh on every call (NOT cached) — see the comment at the
    // top of the implementation for the failure mode caching produced.
    // Called exactly once per start-scan press from
    // onPlannerScanStartRequested. Returns true on confirmed success;
    // on failure surfaces a BdrMessageBox warning to the operator and
    // returns false. The caller may choose to proceed or block
    // start-scan based on that.
    bool sendControllerMaxLinearVelocity(double max_linear_velocity_mps);

    // Push the operator's New-Scan-Information modal payload (building
    // name, operator name, units preference) to the robot's
    // /data_collection_coordinator as ROS string parameters, so the
    // coordinator's next ensure_mission_session() (driven by the
    // controller's /dc/start a few hundred ms later) lands the
    // section folder under /R_DATA/<day>/<building_slug>/Section_*/
    // instead of the BDR_test fallback.
    //
    // Async: dispatches a SetParameters request and invokes
    // `on_complete(true)` on the Qt main thread when ALL three params
    // are accepted, or `on_complete(false)` (with a BdrMessageBox
    // already shown to the operator) on any failure path:
    //   - exploration node not initialized
    //   - /data_collection_coordinator/set_parameters not reachable
    //     within 1.5 s (coordinator probably not launched yet)
    //   - response missing / partial / any param rejected
    //
    // Caller MUST gate downstream side-effects (autonomy_enable,
    // /dc/start chain) on `ok == true` — see
    // onPlannerScanStartRequested for the canonical wiring.
    void sendDataCollectorSessionMetadata(
        const QString& building_name,
        const QString& operator_name,
        const QString& units_preference,
        std::function<void(bool ok)> on_complete);

    struct ExplorationOdomSample {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        qint64 received_at_ms = 0;
    };

    QStackedWidget* stack_ = nullptr;
    SetupScreen* stage1_ = nullptr;
    StartupScreen* stage2_ = nullptr;
    DashboardScreen* stage3_ = nullptr;
    ExplorationScreen* stage4_ = nullptr;
    PlannerScreen* stage5_ = nullptr;

    // Lazily constructed cloud upload dialog (Stage 3 quick-action).
    // Lives across openings so it can carry probe state in-process if
    // the operator closes and reopens it within a session. See
    // `onUploadDataRequested()` for the seeding contract.
    UploadDialog* upload_dialog_ = nullptr;

    // OTA update plumbing. Banner sits above the stage stack so it persists
    // across stage transitions; checker polls GitHub Releases on a 5-min
    // cadence and toggles banner visibility.
    UpdateBanner* update_banner_ = nullptr;
    /// Margin host wrapping `update_banner_`. Tracked so it can be hidden
    /// alongside the banner — otherwise its reserved top margin leaves a
    /// thin strip at the top of the window when no update is available.
    QWidget* update_banner_host_ = nullptr;
    /// Phase 9 advisory banner. Created lazily in `showRolledBackBanner`
    /// because the typical operator session never sees it.
    RollbackBanner* rollback_banner_ = nullptr;
    /// Margin host wrapping `rollback_banner_`. Tracked (like
    /// `update_banner_host_`) so its themed background follows live theme
    /// toggles instead of leaving an unthemed white band at the top.
    QWidget* rollback_banner_host_ = nullptr;
    update::UpdateChecker* update_checker_ = nullptr;

    QWidget* central_root_ = nullptr;
    QWidget* window_controls_ = nullptr;
    QWidget* window_theme_host_ = nullptr;
    QToolButton* window_theme_toggle_ = nullptr;
    QLabel* window_theme_left_icon_ = nullptr;
    QLabel* window_theme_right_icon_ = nullptr;
    QWidget* window_theme_knob_ = nullptr;
    QToolButton* window_minimize_ = nullptr;
    QToolButton* window_maximize_ = nullptr;
    QToolButton* window_close_ = nullptr;

    QString robot_id_;
    QString access_code_;  // in-memory only
    bool dark_mode_ = false;
    bool dragging_ = false;
    QPoint drag_offset_;
    int drag_height_ = 40;

    QProcess* laptop_launch_proc_ = nullptr;
    QProcess* robot_launch_proc_ = nullptr;
    QProcess* saved_map_download_proc_ = nullptr;
    QProcess* exploration_rf_probe_proc_ = nullptr;
    QProcess* exploration_storage_probe_proc_ = nullptr;
    QTimer* exploration_launch_poll_timer_ = nullptr;
    QTimer* exploration_live_fast_timer_ = nullptr;
    QTimer* exploration_live_slow_timer_ = nullptr;
    bool exploration_launch_in_progress_ = false;
    bool exploration_launch_ready_ = false;
    bool exploration_launch_failed_ = false;
    bool laptop_launch_started_ = false;
    bool robot_launch_started_ = false;
    bool laptop_launch_confirmed_ = false;
    bool robot_launch_confirmed_ = false;
    bool local_zenoh_ready_ = false;
    bool video_service_ready_ = false;
    bool odom_ready_ = false;
    bool stream_status_ready_ = false;
    bool stream_target_published_ = false;
    bool fpv_started_ = false;
    double exploration_latest_x_m_ = 0.0;
    double exploration_latest_y_m_ = 0.0;
    double exploration_latest_z_m_ = 0.0;
    double exploration_latest_vx_mps_ = 0.0;
    double exploration_latest_vy_mps_ = 0.0;
    double exploration_latest_yaw_rad_ = 0.0;
    qint64 exploration_last_odom_at_ms_ = 0;
    qint64 exploration_last_stream_status_at_ms_ = 0;
    qint64 exploration_last_thermal_thumb_at_ms_ = 0;
    qint64 exploration_last_thermal_summary_at_ms_ = 0;
    qint64 exploration_last_local_nav_grid_at_ms_ = 0;
    qint64 exploration_last_rf_probe_at_ms_ = 0;
    qint64 exploration_last_rf_success_at_ms_ = 0;
    qint64 exploration_scan_started_at_ms_ = 0;
    QString exploration_storage_text_ = QStringLiteral("N/A");
    bool exploration_storage_ready_ = false;
    bool exploration_rf_config_ready_ = false;
    bool exploration_thermal_thumb_ready_ = false;
    bool exploration_thermal_summary_ready_ = false;
    bool exploration_local_nav_grid_ready_ = false;
    bool exploration_rf_probe_in_flight_ = false;
    bool exploration_rf_metric_ready_ = false;
    bool exploration_rf_link_latched_ = false;
    bool exploration_storage_probe_in_flight_ = false;
    qint64 exploration_last_storage_probe_at_ms_ = 0;
    int exploration_rf_rssi_dbm_ = 0;

    // Latest battery sample mirrored from DashboardScreen.  Populated
    // by onDashboardBatteryStateChanged.  `last_battery_stale_` starts
    // true so Stage 4/5 pills render "—%" until the first real
    // dashboard payload arrives, never the constructor placeholder.
    double last_battery_pct_ = 0.0;
    bool last_battery_stale_ = true;

    double exploration_thermal_max_c_ = 0.0;
    double exploration_thermal_avg_c_ = 0.0;
    double exploration_thermal_min_c_ = 0.0;
    QString exploration_rf_radio_ip_;
    QString exploration_rf_snmp_ro_community_;
    QString exploration_rf_rssi_oid_;
    QString exploration_rf_snr_oid_;
    double exploration_left_iq_measured_ = 0.0;
    double exploration_right_iq_measured_ = 0.0;
    int exploration_left_axis_state_ = 1;
    int exploration_right_axis_state_ = 1;
    qint64 exploration_left_iq_at_ms_ = 0;
    qint64 exploration_right_iq_at_ms_ = 0;
    std::vector<double> exploration_motor_current_scan_samples_;
    size_t exploration_motor_current_max_samples_ = 30000;
    bool exploration_prev_scan_baseline_available_ = false;
    double exploration_prev_scan_median_abs_iq_ = 0.0;
    double exploration_prev_scan_mad_abs_iq_ = 0.0;
    int exploration_motor_spike_streak_ = 0;
    bool exploration_motor_warning_active_ = false;
    QString stream_status_text_;
    QByteArray exploration_thermal_thumb_bytes_;
    QByteArray exploration_local_nav_grid_bytes_;
    std::vector<ExplorationOdomSample> exploration_odom_samples_;
    size_t exploration_odom_max_points_ = 5000;
    qint64 latest_map_save_cutoff_at_ms_ = 0;
    qint64 exploration_launch_started_at_ms_ = 0;
    qint64 last_launch_probe_at_ms_ = 0;
    qint64 last_stream_target_publish_at_ms_ = 0;
    QString active_robot_host_;
    QString active_robot_ssh_user_;
    QString laptop_launch_last_output_;
    QString robot_launch_last_output_;
    QString latest_saved_map_local_path_;
    QString pending_saved_map_remote_path_;
    QString pending_saved_map_local_path_;
    int pending_saved_map_retry_remaining_ = 0;
    bool map_save_or_download_in_progress_ = false;

    rclcpp::Node::SharedPtr exploration_ros_node_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr exploration_stream_target_pub_;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr exploration_stream_status_sub_;
    rclcpp::Subscription<std_msgs::msg::UInt8MultiArray>::SharedPtr exploration_thermal_thumb_sub_;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr exploration_thermal_summary_sub_;
    rclcpp::Subscription<std_msgs::msg::UInt8MultiArray>::SharedPtr exploration_local_nav_grid_sub_;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr planner_scan_status_sub_;
    // /udc/health subscriber + cached payload.  Drives the new "REC"
    // pill on Stage 4 + Stage 5 top bars and gates Start Scan when
    // state == "DEAD_MAX_RESTARTS".  The supervisor publishes the DEAD
    // state with TRANSIENT_LOCAL durability, so subscribing late still
    // catches it.
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr udc_health_sub_;
    // Streaming-camera selection (CameraSwitchPill <-> UDC).  Bound to
    // /stream_camera_select (pub) and /stream_camera_status (sub) when
    // ensureExplorationRosInterfaces() spins up the orchestrator node.
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr stream_camera_select_pub_;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr stream_camera_status_sub_;
    QString udc_health_state_ = QStringLiteral("UNKNOWN");
    qint64 udc_health_last_msg_ms_ = 0;
    quint64 udc_health_total_rows_ = 0;
    qint64 udc_health_last_row_advance_ms_ = 0;
    quint64 udc_health_last_seen_total_rows_ = 0;

    // Single-source-of-truth for "is the robot reachable?".  Owned by
    // AppShell, stamped from existing ROS callbacks, consumed by
    // PlannerScreen + ExplorationScreen via setRobotLinkState().
    LinkHealthMonitor* link_monitor_ = nullptr;
    // Network-layer ICMP/TCP-22 probe.  Pushes its reachability
    // verdict into link_monitor_->setReachability so the layered
    // model can distinguish RECONNECTING (Zenoh hiccup, host still
    // up) from DISCONNECTED (host genuinely gone).  Armed/disarmed
    // alongside link_monitor_.
    RobotReachabilityProbe* reachability_probe_ = nullptr;
    // Toast widget used for "command dropped — robot offline" hints.
    // Lazily constructed on first show; always parented to central_root_
    // so the OTA banner / window controls stay above it.  Single
    // instance; show queues replace the current message.
    QFrame* command_toast_ = nullptr;
    QLabel* command_toast_label_ = nullptr;
    QTimer* command_toast_dismiss_timer_ = nullptr;
    QString command_toast_last_message_;
    qint64 command_toast_last_shown_at_ms_ = 0;
    qint64 link_disconnect_started_at_ms_ = 0;
    bool link_recovery_resync_pending_ = false;
    rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr exploration_odom_sub_;
    rclcpp::Subscription<odrive_can::msg::ControllerStatus>::SharedPtr exploration_left_status_sub_;
    rclcpp::Subscription<odrive_can::msg::ControllerStatus>::SharedPtr exploration_right_status_sub_;
    rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr exploration_cmd_vel_pub_;
    rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr planner_f2c_waypoints_pub_;
    rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr planner_mpc_autonomy_pub_;
    rclcpp::Client<std_srvs::srv::SetBool>::SharedPtr exploration_video_record_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr exploration_save_map_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr planner_dc_pause_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr planner_dc_resume_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr planner_dc_finalize_mission_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr planner_dc_cancel_scan_client_;
    rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr exploration_left_axis_client_;
    rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr exploration_right_axis_client_;
    rclcpp::Client<odrive_can::srv::AxisState>::SharedPtr exploration_gpr_axis_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr exploration_gpr_power_off_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr exploration_gpr_line_stop_client_;
    bool planner_estop_active_ = false;

    // (No cached SetParameters client. sendControllerMaxLinearVelocity
    // creates one local to each call to avoid stale Zenoh-bridge
    // discovery state and the rclcpp Humble race between cached
    // clients' response waitables and `spin_until_future_complete`. See
    // the implementation comment for full diagnosis.)

    // Shared: after IDLE request, poll left/right axis_state until IDLE (≤2 s)
    // before killing the launch tree so odrive_can receives the service.
    QTimer* exploration_motors_idle_wait_timer_ = nullptr;
    int exploration_motors_idle_wait_ticks_ = 0;

    // /dc/finalize_mission async wait. We send the request after motors
    // disarm and before tearing down the launch tree, so the gps_driver
    // gets a chance to flush the continuous mission .ubx cleanly. Hard
    // ceiling at 6 s so the UI never wedges if the service hangs.
    QTimer* planner_finalize_mission_wait_timer_ = nullptr;
    int planner_finalize_mission_wait_ticks_ = 0;
    bool planner_finalize_mission_in_flight_ = false;

    // /dc/cancel_scan async wait. Mirrors the finalize timer above: a single
    // shared QTimer polls for the async response so we never block the Qt
    // event loop, and an 8 s ceiling guarantees the UI always advances even
    // if the controller hangs while ripping data off disk.
    QTimer* planner_cancel_scan_wait_timer_ = nullptr;
    int planner_cancel_scan_wait_ticks_ = 0;
    bool planner_cancel_scan_in_flight_ = false;
};

}  // namespace f2c_cpp

