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

class QResizeEvent;

class QLabel;
class QEvent;
class QProcess;
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

class AppShellWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit AppShellWindow(QWidget* parent = nullptr);
    ~AppShellWindow() override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onLoginSubmitted(const QString& robotId, const QString& accessCode);
    void goToStage1();
    void goToStage2();
    void goToStage3();
    void goToStage4();
    void goToStage5();
    void onThemeToggleChanged(bool dark_mode);

    void onStartNewScan();
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
    void startRobotCompleteLaunch(const QString& robot_host);
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
    QString robotHostFromSettings() const;
    bool isLocalProcessRunning(const QString& process_name) const;
    bool isRobotPipelineRunning(const QString& robot_host) const;
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

    struct ExplorationOdomSample {
        double x = 0.0;
        double y = 0.0;
        qint64 received_at_ms = 0;
    };

    QStackedWidget* stack_ = nullptr;
    SetupScreen* stage1_ = nullptr;
    StartupScreen* stage2_ = nullptr;
    DashboardScreen* stage3_ = nullptr;
    ExplorationScreen* stage4_ = nullptr;
    PlannerScreen* stage5_ = nullptr;

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

