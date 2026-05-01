#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QToolButton;

namespace f2c_cpp {

class StartupScreen : public QWidget {
    Q_OBJECT

public:
    explicit StartupScreen(QWidget* parent = nullptr);

    void setRobotId(const QString& robotId);
    void resetState();

signals:
    void backRequested();
    void continueRequested();

private slots:
    void onCalibrationClicked();
    void onInitiateStartupClicked();
    void onContinueClicked();

private:
    void applyLocalStyle();
    void updateUiState();

    QString robot_id_;
    bool startup_initiated_ = false;

    QLabel* lbl_robot_ = nullptr;
    QLabel* lbl_status_ = nullptr;
    QPushButton* btn_calibration_ = nullptr;
    QPushButton* btn_initiate_ = nullptr;
    QToolButton* btn_arrow_ = nullptr;
};

}  // namespace f2c_cpp

