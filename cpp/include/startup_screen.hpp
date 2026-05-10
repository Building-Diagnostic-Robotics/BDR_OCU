#pragma once

#include <QWidget>
#include <QProcess>

class QLabel;
class QShowEvent;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QToolButton;

namespace f2c_cpp {

class StartupScreen : public QWidget {
    Q_OBJECT

public:
    explicit StartupScreen(QWidget* parent = nullptr);

    void setRobotId(const QString& robotId);
    void resetState();
    void setDarkMode(bool dark_mode);

    // Latest preflight rollup. Returns one of "READY" / "WARN" / "FAIL" /
    // "" (empty = no report parsed yet). The DashboardScreen System
    // Status card folds this into a broader robot-health rollup.
    QString preflightResult() const { return overall_status_; }

protected:
    void showEvent(QShowEvent* event) override;

signals:
    void backRequested();
    void continueRequested();

private slots:
    void onStartDiagnosticsClicked();
    void onRerunDiagnosticsClicked();
    void onLaunchDashboardClicked();
    void onRetryFetchReportClicked();

    void onPreflightStdoutReady();
    void onPreflightStderrReady();
    void onPreflightFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onPreflightError(QProcess::ProcessError error);

    void onReportFetchFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onReportError(QProcess::ProcessError error);

private:
    void applyLocalStyle();
    void updateUiState();
    void startDiagnostics(bool scroll_to_results);
    void setLiveResultsActive(bool active);
    void scrollToLiveResults();
    void updateStatusFromLogLine(const QString& line);
    void updateCombinedRgbStatus();

    QString robot_id_;
    bool preflight_running_ = false;
    bool preflight_completed_ = false;
    QString overall_status_;

    // Header
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_subtitle_ = nullptr;

    // Diagnostic rows (left pane): each row has Title, Subtitle (dynamic data), StatusBadge
    QLabel* lbl_rgb_icon_ = nullptr;
    QLabel* lbl_rgb_status_ = nullptr;
    QLabel* lbl_rgb_subtitle_ = nullptr;
    QString left_rgb_status_;
    QString right_rgb_status_;

    QLabel* lbl_thermal_icon_ = nullptr;
    QLabel* lbl_thermal_status_ = nullptr;
    QLabel* lbl_thermal_subtitle_ = nullptr;

    QLabel* lbl_lidar_icon_ = nullptr;
    QLabel* lbl_lidar_status_ = nullptr;
    QLabel* lbl_lidar_subtitle_ = nullptr;

    QLabel* lbl_rf_icon_ = nullptr;
    QLabel* lbl_rf_status_ = nullptr;
    QLabel* lbl_rf_subtitle_ = nullptr;

    QLabel* lbl_gps_icon_ = nullptr;
    QLabel* lbl_gps_status_ = nullptr;
    QLabel* lbl_gps_subtitle_ = nullptr;
    QWidget* gps_card_ = nullptr;  // Hidden when indoor mode skips GPS

    QLabel* lbl_motors_icon_ = nullptr;
    QLabel* lbl_motors_status_ = nullptr;
    QLabel* lbl_motors_subtitle_ = nullptr;

    // Overall status (left pane)
    QLabel* lbl_overall_icon_ = nullptr;
    QLabel* lbl_overall_status_ = nullptr;

    QScrollArea* scroll_area_ = nullptr;
    QWidget* live_results_section_ = nullptr;
    QWidget* live_results_log_pane_ = nullptr;

    QPlainTextEdit* txt_log_ = nullptr;
    QPushButton* btn_retry_report_ = nullptr;
    QPushButton* btn_indoor_ = nullptr;
    QPushButton* btn_start_ = nullptr;
    QPushButton* btn_rerun_ = nullptr;
    QPushButton* btn_launch_ = nullptr;

    QProcess* preflight_proc_ = nullptr;
    QProcess* report_proc_ = nullptr;

    void appendLog(const QString& line);
    void resetResultsUi();
    void applyStatusBadge(QLabel* icon, QLabel* text, const QString& status,
                         QLabel* subtitle = nullptr, const QString& subtitleText = QString()) const;
    QString combineStatus(const QString& a, const QString& b) const;
    void fetchLatestReport();

    bool live_results_active_ = false;
    bool dark_mode_ = false;

    QString detectLocalIP() const;
    QString robotHostFromSettings() const;
    QStringList sshBaseArgs(const QString& robotHost) const;
};

}  // namespace f2c_cpp

