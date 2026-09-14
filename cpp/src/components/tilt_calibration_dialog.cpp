/**
 * @file tilt_calibration_dialog.cpp
 * @brief 3-step tilt calibration dialog: Setup -> Progress -> Success.
 */

#include "components/tilt_calibration_dialog.hpp"
#include "components/bdr_message_box.hpp"
#include "ui_theme_constants.hpp"

#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace f2c_cpp {

namespace {

const int PAGE_SETUP = 0;
const int PAGE_PROGRESS = 1;
const int PAGE_SUCCESS = 2;

}  // namespace

TiltCalibrationDialog::TiltCalibrationDialog(const QString& robotHost,
                                               const QString& sshUser,
                                               QWidget* parent)
    : QDialog(parent)
    , robot_host_(robotHost)
    , ssh_user_(sshUser.trimmed().isEmpty() ? QStringLiteral("roofus") : sshUser.trimmed()) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowModality(Qt::ApplicationModal);
    setMinimumWidth(440);
    buildUi();
    applyStyle();
}

void TiltCalibrationDialog::setDarkMode(bool dark) {
    if (dark_mode_ == dark) {
        return;
    }
    dark_mode_ = dark;
    applyStyle();
}

void TiltCalibrationDialog::buildUi() {
    auto* container = new QWidget(this);
    container->setObjectName("TiltCalibrationContainer");
    container->setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(container);

    auto* mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    stack_ = new QStackedWidget(container);
    mainLayout->addWidget(stack_);

    // ── Page 1: Setup ─────────────────────────────────────────────────────
    auto* page1 = new QWidget();
    auto* p1Layout = new QVBoxLayout(page1);
    p1Layout->setContentsMargins(24, 24, 24, 24);
    p1Layout->setSpacing(16);

    lbl_setup_badge_ = new QLabel(tr("RECOMMENDED MAINTENANCE"), page1);
    lbl_setup_badge_->setObjectName("setupBadge");
    p1Layout->addWidget(lbl_setup_badge_, 0, Qt::AlignLeft);

    lbl_setup_title_ = new QLabel(tr("Sensor Calibration"), page1);
    lbl_setup_title_->setObjectName("titleText");
    p1Layout->addWidget(lbl_setup_title_, 0, Qt::AlignLeft);

    lbl_setup_subtitle_ = new QLabel(tr("Periodic calibration recommended every 3 scans."), page1);
    lbl_setup_subtitle_->setObjectName("subText");
    lbl_setup_subtitle_->setWordWrap(true);
    p1Layout->addWidget(lbl_setup_subtitle_, 0, Qt::AlignLeft);

    frame_instructions_ = new QFrame(page1);
    frame_instructions_->setObjectName("instructionsFrame");
    frame_instructions_->setAttribute(Qt::WA_StyledBackground, true);
    auto* instrLayout = new QVBoxLayout(frame_instructions_);
    instrLayout->setContentsMargins(16, 16, 16, 16);
    instrLayout->setSpacing(8);

    auto addInstr = [&](const QString& text) {
        auto* l = new QLabel(text, frame_instructions_);
        l->setObjectName("subText");
        l->setWordWrap(true);
        instrLayout->addWidget(l, 0, Qt::AlignLeft);
    };
    addInstr(tr("1. Place the robot on level, stable ground."));
    addInstr(tr("2. Ensure the robot is perfectly stationary."));
    addInstr(tr("3. No active scans or motion tasks can be running."));

    p1Layout->addWidget(frame_instructions_);

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(12);
    btn_start_ = new QPushButton(tr("Start Calibration"), page1);
    btn_start_->setObjectName("primaryButton");
    btn_start_->setCursor(Qt::PointingHandCursor);
    btn_start_->setMinimumHeight(40);
    connect(btn_start_, &QPushButton::clicked, this, &TiltCalibrationDialog::onStartCalibrationClicked);
    btnRow->addWidget(btn_start_);

    btn_skip_ = new QPushButton(tr("Skip for Now"), page1);
    btn_skip_->setObjectName("secondaryButton");
    btn_skip_->setCursor(Qt::PointingHandCursor);
    btn_skip_->setMinimumHeight(40);
    connect(btn_skip_, &QPushButton::clicked, this, &TiltCalibrationDialog::onSkipClicked);
    btnRow->addWidget(btn_skip_);

    p1Layout->addLayout(btnRow);
    p1Layout->addStretch();

    stack_->addWidget(page1);

    // ── Page 2: Progress ───────────────────────────────────────────────────
    auto* page2 = new QWidget();
    auto* p2Layout = new QVBoxLayout(page2);
    p2Layout->setContentsMargins(24, 24, 24, 24);
    p2Layout->setSpacing(16);

    lbl_progress_title_ = new QLabel(tr("Calibrating..."), page2);
    lbl_progress_title_->setObjectName("titleText");
    p2Layout->addWidget(lbl_progress_title_, 0, Qt::AlignLeft);

    lbl_progress_subtitle_ = new QLabel(tr("Do not move the robot"), page2);
    lbl_progress_subtitle_->setObjectName("subText");
    p2Layout->addWidget(lbl_progress_subtitle_, 0, Qt::AlignLeft);

    progress_bar_ = new QProgressBar(page2);
    progress_bar_->setObjectName("progressBar");
    progress_bar_->setRange(0, 0);
    progress_bar_->setValue(0);
    progress_bar_->setTextVisible(false);
    p2Layout->addWidget(progress_bar_);

    lbl_log_ = new QLabel(tr("Connecting..."), page2);
    lbl_log_->setObjectName("logText");
    lbl_log_->setWordWrap(true);
    lbl_log_->setMinimumHeight(60);
    p2Layout->addWidget(lbl_log_);

    frame_warning_ = new QFrame(page2);
    frame_warning_->setObjectName("warningFrame");
    frame_warning_->setAttribute(Qt::WA_StyledBackground, true);
    auto* warnLayout = new QVBoxLayout(frame_warning_);
    warnLayout->setContentsMargins(12, 12, 12, 12);
    auto* warnLbl = new QLabel(tr("Calibrating... Do not move robot"), frame_warning_);
    warnLbl->setObjectName("subText");
    warnLayout->addWidget(warnLbl);
    p2Layout->addWidget(frame_warning_);
    p2Layout->addStretch();

    stack_->addWidget(page2);

    // ── Page 3: Success ────────────────────────────────────────────────────
    auto* page3 = new QWidget();
    auto* p3Layout = new QVBoxLayout(page3);
    p3Layout->setContentsMargins(24, 24, 24, 24);
    p3Layout->setSpacing(16);

    lbl_success_icon_ = new QLabel(tr("✓"), page3);
    lbl_success_icon_->setObjectName("successIcon");
    p3Layout->addWidget(lbl_success_icon_, 0, Qt::AlignCenter);

    lbl_success_title_ = new QLabel(tr("Calibration Complete"), page3);
    lbl_success_title_->setObjectName("titleText");
    lbl_success_title_->setAlignment(Qt::AlignCenter);
    p3Layout->addWidget(lbl_success_title_, 0, Qt::AlignCenter);

    lbl_success_subtitle_ = new QLabel(tr("System is ready for operation"), page3);
    lbl_success_subtitle_->setObjectName("subText");
    lbl_success_subtitle_->setAlignment(Qt::AlignCenter);
    p3Layout->addWidget(lbl_success_subtitle_, 0, Qt::AlignCenter);

    frame_result_ = new QFrame(page3);
    frame_result_->setObjectName("resultFrame");
    frame_result_->setAttribute(Qt::WA_StyledBackground, true);
    auto* resultLayout = new QVBoxLayout(frame_result_);
    resultLayout->setContentsMargins(16, 16, 16, 16);
    lbl_pitch_angle_ = new QLabel(tr("Pitch Angle: —"), frame_result_);
    lbl_pitch_angle_->setObjectName("resultText");
    resultLayout->addWidget(lbl_pitch_angle_);
    p3Layout->addWidget(frame_result_);

    lbl_redirect_ = new QLabel(tr("Calibration complete. Closing window in 3..."), page3);
    lbl_redirect_->setObjectName("redirectText");
    lbl_redirect_->setAlignment(Qt::AlignCenter);
    p3Layout->addWidget(lbl_redirect_);
    p3Layout->addStretch();

    stack_->addWidget(page3);
}

void TiltCalibrationDialog::applyStyle() {
    const auto t = uiThemeTokens(dark_mode_);
    const QString dialog_bg = dark_mode_ ? QStringLiteral("#18181B")
                                         : QStringLiteral("#FFFFFF");
    const QString border = dark_mode_ ? QStringLiteral("#3F3F46")
                                      : QStringLiteral("#E5E7EB");
    const QString text = dark_mode_ ? QStringLiteral("#F4F4F5")
                                    : QStringLiteral("#111827");
    const QString muted = dark_mode_ ? QStringLiteral("#A1A1AA")
                                     : QStringLiteral("#52525B");
    const QString control_bg = dark_mode_ ? QStringLiteral("#27272A")
                                          : QStringLiteral("#F9FAFB");
    const QString control_hover = dark_mode_ ? QStringLiteral("#3F3F46")
                                             : QStringLiteral("#F3F4F6");
    const QString badge_fg = dark_mode_ ? QStringLiteral("#FBBF24")
                                        : QStringLiteral("#92400E");
    const QColor accent(t.accent);
    const QString result_fill = QStringLiteral("rgba(%1, %2, %3, 0.12)")
                                    .arg(accent.red())
                                    .arg(accent.green())
                                    .arg(accent.blue());

    setStyleSheet(QStringLiteral(R"CSS(
        QWidget#TiltCalibrationContainer {
            background-color: %1;
            border: 1px solid %2;
            border-radius: 14px;
        }
        QLabel#titleText {
            font-family: 'Arimo';
            font-size: 22px;
            font-weight: 700;
            color: %3;
            background: transparent;
        }
        QLabel#subText {
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 500;
            color: %4;
            background: transparent;
        }
        QLabel#logText {
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 500;
            color: %3;
            background: transparent;
        }
        QLabel#setupBadge {
            font-family: 'Arimo';
            font-size: 11px;
            font-weight: 700;
            color: %5;
            background-color: rgba(245, 158, 11, 0.18);
            padding: 4px 10px;
            border-radius: 6px;
        }
        QFrame#instructionsFrame {
            background-color: %6;
            border-radius: 10px;
            border: 1px solid %2;
        }
        QPushButton#primaryButton {
            background-color: %7;
            color: #FFFFFF;
            border: none;
            border-radius: 8px;
            padding: 10px 20px;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 600;
        }
        QPushButton#primaryButton:hover {
            background-color: %8;
        }
        QPushButton#secondaryButton {
            background-color: %6;
            color: %3;
            border: 1px solid %2;
            border-radius: 8px;
            padding: 10px 20px;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 600;
        }
        QPushButton#secondaryButton:hover {
            background-color: %9;
        }
        QProgressBar#progressBar {
            background-color: %6;
            border: 1px solid %2;
            border-radius: 4px;
            min-height: 8px;
            max-height: 8px;
        }
        QProgressBar#progressBar::chunk {
            background-color: %7;
            border-radius: 3px;
        }
        QFrame#warningFrame {
            background-color: rgba(245, 158, 11, 0.18);
            border: 1px solid rgba(245, 158, 11, 0.55);
            border-radius: 8px;
        }
        QLabel#successIcon {
            font-family: 'Arimo';
            font-size: 48px;
            color: %7;
            font-weight: 700;
            background: transparent;
        }
        QFrame#resultFrame {
            background-color: %10;
            border: 1px solid %7;
            border-radius: 8px;
        }
        QLabel#resultText {
            font-family: 'Arimo';
            font-size: 18px;
            font-weight: 700;
            color: %7;
            background: transparent;
        }
        QLabel#redirectText {
            font-family: 'Arimo';
            font-size: 12px;
            font-weight: 500;
            color: %4;
            background: transparent;
        }
    )CSS")
                      .arg(dialog_bg, border, text, muted, badge_fg,
                           control_bg, t.accent, t.accent_hover, control_hover)
                      .arg(result_fill));
}

void TiltCalibrationDialog::switchToPage(int index) {
    if (stack_) {
        stack_->setCurrentIndex(index);
    }
}

QString TiltCalibrationDialog::stripAnsiCodes(const QString& raw) const {
    QString s = raw;
    s.remove(QRegularExpression("\\x1B\\[[0-?]*[ -/]*[@-~]"));
    return s;
}

QString TiltCalibrationDialog::extractPitchAngle(const QString& stdoutText) const {
    QRegularExpression re("Pitch angle:\\s*([+-]?\\d+\\.?\\d*)\\s*°");
    int idx = stdoutText.indexOf(re);
    if (idx >= 0) {
        auto m = re.match(stdoutText, idx);
        if (m.hasMatch()) {
            return m.captured(1) + "°";
        }
    }
    return QString();
}

QStringList TiltCalibrationDialog::sshBaseArgs() const {
    return QStringList()
        << QStringLiteral("-tt")
        << QStringLiteral("-o") << QStringLiteral("ConnectTimeout=10")
        << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=no")
        << QStringLiteral("-o") << QStringLiteral("UserKnownHostsFile=/dev/null")
        << QStringLiteral("-o") << QStringLiteral("BatchMode=yes")
        << QStringLiteral("%1@%2").arg(ssh_user_, robot_host_);
}

void TiltCalibrationDialog::onStartCalibrationClicked() {
    switchToPage(PAGE_PROGRESS);
    startCalibrationProcess();
}

void TiltCalibrationDialog::onSkipClicked() {
    reject();
}

void TiltCalibrationDialog::startCalibrationProcess() {
    if (completion_fallback_timer_) {
        completion_fallback_timer_->stop();
        completion_fallback_timer_->deleteLater();
        completion_fallback_timer_ = nullptr;
    }
    stdout_buffer_.clear();
    stderr_buffer_.clear();
    lbl_log_->setText(tr("Connecting to robot..."));
    progress_bar_->setRange(0, 0);
    progress_bar_->setValue(0);

    QString script = QString(
        "set -e; "
        "if [ -f /opt/ros/humble/setup.bash ]; then source /opt/ros/humble/setup.bash; "
        "elif [ -f /opt/ros/foxy/setup.bash ]; then source /opt/ros/foxy/setup.bash; "
        "elif [ -f /opt/ros/galactic/setup.bash ]; then source /opt/ros/galactic/setup.bash; "
        "fi; "
        "if [ -f \"$HOME/pilot_ws/install/setup.bash\" ]; then source \"$HOME/pilot_ws/install/setup.bash\"; fi; "
        "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp; export ROS_DOMAIN_ID=0; "
        "ros2 run pilot_control tilt_calibration --ros-args -p num_samples:=100 -p start_lidar:=true");
    QString remote_cmd = QString("bash -lc \"%1\"").arg(script.replace("\"", "\\\""));

    proc_ = new QProcess(this);
    connect(proc_, &QProcess::readyReadStandardOutput, this, &TiltCalibrationDialog::onStdoutReady);
    connect(proc_, &QProcess::readyReadStandardError, this, &TiltCalibrationDialog::onStderrReady);
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &TiltCalibrationDialog::onProcessFinished);
    connect(proc_, &QProcess::errorOccurred, this, &TiltCalibrationDialog::onProcessError);

    proc_->setProgram("ssh");
    proc_->setArguments(sshBaseArgs() << remote_cmd);
    proc_->start();
}

void TiltCalibrationDialog::onStdoutReady() {
    if (!proc_) return;
    stdout_buffer_ += QString::fromUtf8(proc_->readAllStandardOutput());

    QString text = stripAnsiCodes(stdout_buffer_).trimmed();
    QString displayLine;
    if (text.contains("Collecting IMU samples")) {
        QRegularExpression re("Collecting IMU samples\\.\\.\\.\\s*\\((\\d+)/(\\d+)\\)");
        QRegularExpressionMatch lastMatch;
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            lastMatch = it.next();
        }
        if (lastMatch.hasMatch()) {
            const int current = lastMatch.captured(1).toInt();
            const int total = lastMatch.captured(2).toInt();
            displayLine = tr("Collecting IMU samples... (%1/%2)").arg(lastMatch.captured(1), lastMatch.captured(2));
            progress_bar_->setRange(0, total > 0 ? total : 100);
            progress_bar_->setValue(current);
        } else {
            displayLine = text;
        }
    } else if (text.contains("Starting LiDAR") || text.contains("LiDAR driver")) {
        displayLine = tr("Starting LiDAR...");
    } else if (text.contains("Waiting for") || text.contains("initialize")) {
        displayLine = tr("Waiting for LiDAR to initialize...");
    } else if (text.contains("COMPUTING") || text.contains("Computing")) {
        displayLine = tr("Computing calibration...");
    } else if (text.contains("CALIBRATION COMPLETE")) {
        displayLine = tr("Calibration complete, finalizing...");
    } else {
        int lastNewline = text.lastIndexOf('\n');
        displayLine = lastNewline >= 0 ? text.mid(lastNewline + 1).trimmed() : text;
        if (displayLine.length() > 120) {
            displayLine = displayLine.left(117) + "...";
        }
    }
    if (!displayLine.isEmpty()) {
        lbl_log_->setText(displayLine);
    }

    if (text.contains("CALIBRATION COMPLETE") && !completion_fallback_timer_) {
        completion_fallback_timer_ = new QTimer(this);
        completion_fallback_timer_->setSingleShot(true);
        connect(completion_fallback_timer_, &QTimer::timeout, this, &TiltCalibrationDialog::onCompletionFallback);
        completion_fallback_timer_->start(3000);
    }
}

void TiltCalibrationDialog::onStderrReady() {
    if (!proc_) return;
    stderr_buffer_ += QString::fromUtf8(proc_->readAllStandardError());
}

void TiltCalibrationDialog::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (proc_) {
        proc_->disconnect(this);
    }

    const bool hasSuccessIndicators =
        stdout_buffer_.contains("CALIBRATION COMPLETE") ||
        (stdout_buffer_.contains("Saved to:") && stdout_buffer_.contains(".npz"));
    const bool success =
        (exitStatus == QProcess::NormalExit && exitCode == 0) ||
        (exitStatus == QProcess::NormalExit && hasSuccessIndicators);

    if (success) {
        if (completion_fallback_timer_) {
            completion_fallback_timer_->stop();
            completion_fallback_timer_->deleteLater();
            completion_fallback_timer_ = nullptr;
        }
        QString pitch = extractPitchAngle(stdout_buffer_);
        if (!pitch.isEmpty()) {
            lbl_pitch_angle_->setText(tr("Pitch Angle: %1").arg(pitch));
        } else {
            lbl_pitch_angle_->setText(tr("Pitch Angle: OK"));
        }

        switchToPage(PAGE_SUCCESS);
        progress_bar_->setRange(0, 100);
        progress_bar_->setValue(100);

        countdown_sec_ = 3;
        lbl_redirect_->setText(tr("Calibration complete. Closing window in 3..."));

        if (countdown_timer_) {
            countdown_timer_->stop();
            countdown_timer_->deleteLater();
        }
        countdown_timer_ = new QTimer(this);
        countdown_timer_->setSingleShot(false);
        connect(countdown_timer_, &QTimer::timeout, this, &TiltCalibrationDialog::onCountdownTick);
        countdown_timer_->start(1000);
    } else {
        if (completion_fallback_timer_) {
            completion_fallback_timer_->stop();
            completion_fallback_timer_->deleteLater();
            completion_fallback_timer_ = nullptr;
        }
        switchToPage(PAGE_SETUP);
        QString errMsg = stripAnsiCodes(stderr_buffer_).trimmed();
        if (errMsg.isEmpty()) {
            errMsg = tr("Calibration failed (exit code %1).").arg(exitCode);
        }
        BdrMessageBox::warning(
            this,
            tr("Tilt Calibration Failed"),
            tr("Calibration did not complete successfully:\n\n%1\n\n"
               "Ensure the robot is reachable and the LiDAR driver is available.")
                .arg(errMsg));
        if (proc_) {
            proc_->deleteLater();
            proc_ = nullptr;
        }
    }
}

void TiltCalibrationDialog::onProcessError(QProcess::ProcessError error) {
    if (completion_fallback_timer_) {
        completion_fallback_timer_->stop();
        completion_fallback_timer_->deleteLater();
        completion_fallback_timer_ = nullptr;
    }
    switchToPage(PAGE_SETUP);
    QString errStr;
    switch (error) {
    case QProcess::FailedToStart:
        errStr = tr("Failed to start SSH. Ensure SSH is installed and the robot is reachable.");
        break;
    case QProcess::Crashed:
        errStr = tr("SSH process crashed.");
        break;
    case QProcess::Timedout:
        errStr = tr("SSH timed out.");
        break;
    default:
        errStr = tr("An error occurred during calibration.");
        break;
    }
    BdrMessageBox::warning(this, tr("Tilt Calibration Failed"), errStr);
    if (proc_) {
        proc_->deleteLater();
        proc_ = nullptr;
    }
}

void TiltCalibrationDialog::onCompletionFallback() {
    if (!completion_fallback_timer_) return;
    completion_fallback_timer_->deleteLater();
    completion_fallback_timer_ = nullptr;
    if (stack_ && stack_->currentIndex() != PAGE_PROGRESS) return;
    if (!stdout_buffer_.contains("CALIBRATION COMPLETE") &&
        !(stdout_buffer_.contains("Saved to:") && stdout_buffer_.contains(".npz"))) return;

    QString pitch = extractPitchAngle(stdout_buffer_);
    if (!pitch.isEmpty()) {
        lbl_pitch_angle_->setText(tr("Pitch Angle: %1").arg(pitch));
    } else {
        lbl_pitch_angle_->setText(tr("Pitch Angle: OK"));
    }
    switchToPage(PAGE_SUCCESS);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(100);
    countdown_sec_ = 3;
    lbl_redirect_->setText(tr("Calibration complete. Closing window in 3..."));
    if (countdown_timer_) {
        countdown_timer_->stop();
        countdown_timer_->deleteLater();
    }
    countdown_timer_ = new QTimer(this);
    countdown_timer_->setSingleShot(false);
    connect(countdown_timer_, &QTimer::timeout, this, &TiltCalibrationDialog::onCountdownTick);
    countdown_timer_->start(1000);
}

void TiltCalibrationDialog::onCountdownTick() {
    countdown_sec_--;
    if (countdown_sec_ > 0) {
        lbl_redirect_->setText(tr("Calibration complete. Closing window in %1...").arg(countdown_sec_));
    } else {
        if (countdown_timer_) {
            countdown_timer_->stop();
            countdown_timer_->deleteLater();
            countdown_timer_ = nullptr;
        }
        if (proc_) {
            proc_->disconnect(this);
            if (proc_->state() != QProcess::NotRunning) {
                proc_->terminate();
                proc_->waitForFinished(2000);
                if (proc_->state() != QProcess::NotRunning) {
                    proc_->kill();
                    proc_->waitForFinished(1000);
                }
            }
            proc_ = nullptr;
        }
        accept();
    }
}

}  // namespace f2c_cpp
