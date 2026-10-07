#pragma once

#include "offload_status.hpp"
#include "thumb_drive_watcher.hpp"

#include <QByteArray>
#include <QString>
#include <QWidget>
#include <optional>

class QLabel;
class QPushButton;
class QProcess;
class QShowEvent;
class QHideEvent;
class QTimer;

namespace f2c_cpp {

class DashboardScreen : public QWidget {
    Q_OBJECT

public:
    explicit DashboardScreen(QWidget* parent = nullptr);
    ~DashboardScreen() override;

    void setRobotId(const QString& robotId);
    void setDarkMode(bool dark_mode);
    /** Re-read robot `/R_DATA/.offload/status.json` and the laptop stick. */
    void refreshThumbCopyStatus();
    /** True when robot offload is idle and the laptop stick is not a
     *  partial copy. Last probe is cached so a powered-off robot cannot
     *  look "ready" if the last check said otherwise. */
    bool thumbCopyReady() const;
    QString thumbCopyBlockReason() const;

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

    /**
     * Emitted on every battery state change so AppShell can mirror the
     * percentage onto the Stage 4 / Stage 5 top-bar pill (matching the
     * dashboard tile thresholds — see kBatteryLowPct / kBatteryCriticalPct
     * in dashboard_screen.cpp).  `pct` is the latest SoC percent
     * (`std::nan("")` when the dashboard has no payload yet); `stale`
     * is true when the MQTT payload is older than its
     * stale_after_ms threshold OR no payload has arrived at all.
     */
    void batterySocChanged(double pct, bool stale);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void onLogoutClicked();
    void onStartNewScanClicked();
    void onRunDiagnosticsClicked();
    void onViewRecordingsClicked();

    void onBatteryStaleTimerTick();
    void onBatteryProcessReadyRead();
    void onBatteryProcessFinished();
    void onScansProbeFinished();
    void onScansRefreshTimerTick();
    void onStatusCardRefreshTimerTick();
    void onOffloadProbeFinished();
    void onOffloadRefreshTimerTick();
    void onStickStateChanged();
    void onUploadBlinkTick();

private:
    void applyStyle();
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

    // --- Total Scans SSH probe ---
    // Counts Section_* folders under /R_DATA/<day>/<building>/.
    void startScansProbe();
    void stopScansProbe();
    void setTotalScansDisplay(int totalScans);

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

    void startOffloadProbe();
    void stopOffloadProbe();
    void applyCachedOffload();
    void persistOffloadCache();
    void refreshUploadAction();
    void setUploadSyncBlink(bool blink);
    void applyUploadBlinkFrame();
    bool robotCopyIncomplete() const;
    bool stickCopyIncomplete() const;
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

    QPushButton* btn_start_scan_ = nullptr;
    QPushButton* btn_run_diagnostics_ = nullptr;
    QPushButton* btn_view_recordings_ = nullptr;
    QLabel* lbl_robot_id_value_ = nullptr;
    QLabel* lbl_firmware_value_ = nullptr;
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

    // Total Scans probe.
    QProcess* scans_proc_ = nullptr;
    QTimer* scans_refresh_timer_ = nullptr;

    // Status-card state. Refreshed by a 1 Hz timer + every time any
    // contributing signal changes (battery payload, preflight setter).
    QTimer* status_refresh_timer_ = nullptr;
    QString preflight_status_;  // "" / "READY" / "WARN" / "FAIL"
    qint64 dashboard_first_shown_ms_ = 0;

    // Robot → RDATA_EXT offload. SSH-reads status.json; the laptop
    // stick walk is the second signal. Cache survives robot power-off.
    QProcess* offload_proc_ = nullptr;
    QTimer* offload_refresh_timer_ = nullptr;
    ThumbDriveWatcher* stick_watcher_ = nullptr;
    OffloadSnapshot offload_;
    bool offload_probe_ok_ = false;
    bool offload_cached_incomplete_ = false;
    QString offload_cached_state_;
    StickSync stick_sync_ = StickSync::Absent;
    // Stylesheet pulse — a QGraphicsOpacityEffect on this button sits
    // under the actions-card drop shadow and Qt cannot nest those
    // (the card paints at a wrong offset or vanishes).
    QTimer* upload_blink_timer_ = nullptr;
    bool upload_blink_active_ = false;
    bool upload_blink_dimmed_ = false;
};

}  // namespace f2c_cpp
