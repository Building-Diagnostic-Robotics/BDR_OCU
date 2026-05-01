#pragma once

#include <QWidget>

namespace f2c_cpp {
class TiltCalibrationDialog;
}

class QLabel;
class QPushButton;

namespace f2c_cpp {

class DashboardScreen : public QWidget {
    Q_OBJECT

public:
    explicit DashboardScreen(QWidget* parent = nullptr);

    void setRobotId(const QString& robotId);
    void setDarkMode(bool dark_mode);

signals:
    void logoutRequested();
    void startNewScanRequested();
    void runDiagnosticsRequested();
    void viewRecordingsRequested();

private slots:
    void onLogoutClicked();
    void onStartNewScanClicked();
    void onRunDiagnosticsClicked();
    void onViewRecordingsClicked();
    void onCalibrateTiltRequested();

private:
    void applyStyle();
    QString robotHostFromSettings() const;

    QString robot_id_;
    bool dark_mode_ = false;

    QWidget* header_ = nullptr;
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_subtitle_ = nullptr;
    QPushButton* btn_logout_ = nullptr;

    QWidget* card_status_ = nullptr;
    QLabel* lbl_status_value_ = nullptr;
    QWidget* card_scans_ = nullptr;
    QLabel* lbl_scans_value_ = nullptr;
    QWidget* card_cameras_ = nullptr;
    QLabel* lbl_cameras_value_ = nullptr;
    QWidget* card_calibration_ = nullptr;
    QLabel* lbl_calibration_value_ = nullptr;

    QPushButton* btn_start_scan_ = nullptr;
    QPushButton* btn_run_diagnostics_ = nullptr;
    QPushButton* btn_view_recordings_ = nullptr;
    QPushButton* btn_calibrate_tilt_ = nullptr;

    QLabel* lbl_robot_id_value_ = nullptr;
    QLabel* lbl_firmware_value_ = nullptr;
    QLabel* lbl_calibration_value_info_ = nullptr;
    QLabel* lbl_battery_value_ = nullptr;
};

}  // namespace f2c_cpp
