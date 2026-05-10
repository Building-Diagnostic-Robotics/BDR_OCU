/**
 * @file tilt_calibration_dialog.hpp
 * @brief 3-step tilt calibration dialog: Setup -> Progress -> Success.
 */

#pragma once

#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>

namespace f2c_cpp {

class TiltCalibrationDialog : public QDialog {
    Q_OBJECT

public:
    explicit TiltCalibrationDialog(const QString& robotHost,
                                   const QString& sshUser = QString(),
                                   QWidget* parent = nullptr);

private slots:
    void onStartCalibrationClicked();
    void onSkipClicked();
    void onStdoutReady();
    void onStderrReady();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onCountdownTick();
    void onCompletionFallback();

private:
    void buildUi();
    void applyStyle();
    void switchToPage(int index);
    void startCalibrationProcess();
    QString stripAnsiCodes(const QString& raw) const;
    QString extractPitchAngle(const QString& stdoutText) const;
    QStringList sshBaseArgs() const;

    QString robot_host_;
    QString ssh_user_;
    QStackedWidget* stack_ = nullptr;

    QProcess* proc_ = nullptr;
    QString stdout_buffer_;
    QString stderr_buffer_;

    QTimer* countdown_timer_ = nullptr;
    QTimer* completion_fallback_timer_ = nullptr;
    int countdown_sec_ = 3;

    QLabel* lbl_setup_badge_ = nullptr;
    QLabel* lbl_setup_title_ = nullptr;
    QLabel* lbl_setup_subtitle_ = nullptr;
    QFrame* frame_instructions_ = nullptr;
    QPushButton* btn_start_ = nullptr;
    QPushButton* btn_skip_ = nullptr;

    QLabel* lbl_progress_title_ = nullptr;
    QLabel* lbl_progress_subtitle_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QLabel* lbl_log_ = nullptr;
    QFrame* frame_warning_ = nullptr;

    QLabel* lbl_success_icon_ = nullptr;
    QLabel* lbl_success_title_ = nullptr;
    QLabel* lbl_success_subtitle_ = nullptr;
    QFrame* frame_result_ = nullptr;
    QLabel* lbl_pitch_angle_ = nullptr;
    QLabel* lbl_redirect_ = nullptr;
};

}  // namespace f2c_cpp
