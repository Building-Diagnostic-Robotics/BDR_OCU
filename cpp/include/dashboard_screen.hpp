#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QWidget>
#include <optional>

class QLabel;
class QPushButton;
class QProcess;
class QShowEvent;
class QHideEvent;
class QTimer;
class QEvent;
class QGraphicsOpacityEffect;
class QPropertyAnimation;

namespace f2c_cpp {

class TiltCalibrationDialog;

class DashboardScreen : public QWidget {
    Q_OBJECT

public:
    explicit DashboardScreen(QWidget* parent = nullptr);
    ~DashboardScreen() override;

    void setRobotId(const QString& robotId);
    void setDarkMode(bool dark_mode);

    // Latest Stage 2 preflight rollup: "READY" / "WARN" / "FAIL" / "".
    // Folded into the Stage 3 System Status card alongside live battery
    // + reachability state. Empty = no report seen this session (treated
    // as neutral, doesn't degrade Status).
    void setPreflightResult(const QString& result);

signals:
    void logoutRequested();
    void startNewScanRequested();
    void runDiagnosticsRequested();
    void viewRecordingsRequested();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void onLogoutClicked();
    void onStartNewScanClicked();
    void onRunDiagnosticsClicked();
    void onViewRecordingsClicked();
    void onCalibrateTiltRequested();

    void onBatteryStaleTimerTick();
    void onBatteryProcessReadyRead();
    void onBatteryProcessFinished();
    void onCalibrationProbeFinished();
    void onCalibrationRefreshTimerTick();
    void onStatusCardRefreshTimerTick();

private:
    void applyStyle();
    QString robotHostFromSettings() const;
    void loadRobotProfileFromRegistry();

    // --- Battery (MQTT via mosquitto_sub) ---
    // Robot publishes JSON to pilot/battery/state @ port 1883 (see
    // pilot_control/docs/battery_mqtt_setup.md). We spawn mosquitto_sub as
    // a child QProcess so we don't pull a native MQTT lib into the build.
    void startBatteryMonitor();
    void stopBatteryMonitor();
    void handleBatteryPayload(const QString& jsonLine);
    void refreshBatteryDisplay();
    void setBatteryDisplay(const QString& valueText, const QString& tooltip,
                           const QString& color);

    // --- Last-calibration SSH probe ---
    // Robot stores tilt_correction_matrices_<idx>.npz under
    // /R_DATA/tilt_calibration/. We mtime the latest entry over SSH and
    // render relative time on the card with absolute on hover.
    void startCalibrationProbe();
    void stopCalibrationProbe();
    void setCalibrationDisplay(const QString& valueText, const QString& tooltip);
    static QString formatRelativeTime(const QDateTime& past);

    // --- System Status card ---
    // Three-state rollup of {preflight, battery, robot reachability}.
    // FAIL beats WARN beats READY (OR-logic).
    enum class SystemStatus { Initializing, Ready, Warning, NotReady };
    void refreshSystemStatusCard();
    static const char* statusCardText(SystemStatus s);
    static const char* statusCardColorHex(SystemStatus s);
    // Robot reachability proxy: true iff the most recent battery MQTT
    // payload arrived within `battery_payload_stale_after_ms_` of now.
    // Zero extra probe overhead — the MQTT subscriber already runs at
    // 1 Hz, so a fresh payload is direct evidence of an alive
    // network + battery telemetry pipeline.
    bool robotReachableViaMqttBattery() const;

    // --- Total Scans + Next Calibration cards ---
    // Both fed from the same SSH probe that already runs for Last
    // Calibration (`calibration_proc_`). The combined probe returns
    // three whitespace-separated values: <cal_mtime_epoch> <total_scans>
    // <scans_since_cal>. Saves an extra round-trip.
    void setScansAndCalibrationDisplays(int totalScans, int scansSinceCal);
    void setCalibrationDueBlink(bool blink);
    bool eventFilter(QObject* watched, QEvent* event) override;

    void refreshUptimeDisplay();

    QString robot_id_;
    bool dark_mode_ = false;

    // Cached from RobotRegistry on construction. Used for both MQTT and
    // SSH probes. Empty = no probes attempted (we don't fall back to a
    // hardcoded IP, so missing config is visible rather than silently
    // pinging the wrong host).
    QString robot_host_;
    QString robot_ssh_user_;

    QWidget* header_ = nullptr;
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_subtitle_ = nullptr;
    QPushButton* btn_logout_ = nullptr;

    QWidget* card_status_ = nullptr;
    QLabel* lbl_status_value_ = nullptr;
    QWidget* card_scans_ = nullptr;
    QLabel* lbl_scans_value_ = nullptr;
    QWidget* card_battery_top_ = nullptr;
    QLabel* lbl_battery_card_value_ = nullptr;
    QWidget* card_calibration_ = nullptr;
    QLabel* lbl_calibration_value_ = nullptr;

    QPushButton* btn_start_scan_ = nullptr;
    QPushButton* btn_run_diagnostics_ = nullptr;
    QPushButton* btn_view_recordings_ = nullptr;
    QPushButton* btn_calibrate_tilt_ = nullptr;

    QLabel* lbl_robot_id_value_ = nullptr;
    QLabel* lbl_firmware_value_ = nullptr;
    QLabel* lbl_calibration_value_info_ = nullptr;
    QLabel* lbl_uptime_value_ = nullptr;
    QLabel* lbl_battery_value_ = nullptr;

    // Battery monitor state.
    QProcess* battery_proc_ = nullptr;
    QTimer* battery_stale_timer_ = nullptr;
    QByteArray battery_stdout_buf_;
    QString battery_topic_;
    int battery_port_ = 1883;
    bool battery_has_payload_ = false;
    std::optional<double> battery_soc_pct_;
    std::optional<double> battery_voltage_v_;
    std::optional<double> battery_current_a_;
    bool battery_warn_flag_ = false;
    bool battery_critical_flag_ = false;
    qint64 battery_payload_updated_at_ms_ = 0;
    qint64 battery_payload_stale_after_ms_ = 5000;
    qint64 battery_last_start_attempt_ms_ = 0;

    // Calibration probe state.
    QProcess* calibration_proc_ = nullptr;
    QTimer* calibration_refresh_timer_ = nullptr;
    QDateTime calibration_last_mtime_;

    // Status-card state. Refreshed by a 1 Hz timer + every time any
    // contributing signal changes (battery payload, preflight setter).
    QTimer* status_refresh_timer_ = nullptr;
    QString preflight_status_;  // "" / "READY" / "WARN" / "FAIL"
    qint64 dashboard_first_shown_ms_ = 0;

    // Calibration-due blink state for the top calibration card.
    QGraphicsOpacityEffect* calibration_blink_effect_ = nullptr;
    QPropertyAnimation* calibration_blink_anim_ = nullptr;
    bool calibration_blink_active_ = false;
};

}  // namespace f2c_cpp
