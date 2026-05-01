#include "startup_screen.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace f2c_cpp {

StartupScreen::StartupScreen(QWidget* parent)
    : QWidget(parent) {
    setObjectName("StartupScreenRoot");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(18);

    // Header row: back button
    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(12);

    auto* btn_back = new QPushButton("← Back", this);
    btn_back->setObjectName("StartupBackButton");
    btn_back->setFixedHeight(36);
    btn_back->setFocusPolicy(Qt::NoFocus);
    connect(btn_back, &QPushButton::clicked, this, &StartupScreen::backRequested);
    header->addWidget(btn_back, 0, Qt::AlignLeft);

    header->addStretch(1);
    root->addLayout(header);

    // Body content
    auto* title = new QLabel("STARTUP", this);
    title->setObjectName("StartupTitle");
    root->addWidget(title);

    lbl_robot_ = new QLabel(this);
    lbl_robot_->setObjectName("StartupRobot");
    lbl_robot_->setWordWrap(true);
    root->addWidget(lbl_robot_);

    lbl_status_ = new QLabel(this);
    lbl_status_->setObjectName("StartupStatus");
    lbl_status_->setWordWrap(true);
    root->addWidget(lbl_status_);

    root->addStretch(1);

    // Bottom action row: Calibration | Initiate startup sequence | Arrow
    auto* actions = new QHBoxLayout();
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(14);

    btn_calibration_ = new QPushButton("CALIBRATION", this);
    btn_calibration_->setObjectName("StartupCalibration");
    btn_calibration_->setFixedHeight(46);
    connect(btn_calibration_, &QPushButton::clicked, this, &StartupScreen::onCalibrationClicked);
    actions->addWidget(btn_calibration_, 0);

    btn_initiate_ = new QPushButton("INITIATE STARTUP SEQUENCE", this);
    btn_initiate_->setObjectName("StartupInitiate");
    btn_initiate_->setFixedHeight(46);
    connect(btn_initiate_, &QPushButton::clicked, this, &StartupScreen::onInitiateStartupClicked);
    actions->addWidget(btn_initiate_, 1);

    btn_arrow_ = new QToolButton(this);
    btn_arrow_->setObjectName("StartupArrow");
    btn_arrow_->setText("→");
    btn_arrow_->setFixedSize(56, 46);
    btn_arrow_->setEnabled(false);
    btn_arrow_->setToolTip("Continue");
    connect(btn_arrow_, &QToolButton::clicked, this, &StartupScreen::onContinueClicked);
    actions->addWidget(btn_arrow_, 0);

    root->addLayout(actions);

    applyLocalStyle();
    resetState();
}

void StartupScreen::setRobotId(const QString& robotId) {
    robot_id_ = robotId.trimmed();
    updateUiState();
}

void StartupScreen::resetState() {
    startup_initiated_ = false;
    updateUiState();
}

void StartupScreen::onCalibrationClicked() {
    QMessageBox::information(this, "Calibration", "Calibration screen will be implemented next.");
}

void StartupScreen::onInitiateStartupClicked() {
    // Placeholder behavior for now: mark startup as initiated and enable continue.
    startup_initiated_ = true;
    updateUiState();
}

void StartupScreen::onContinueClicked() {
    emit continueRequested();
}

void StartupScreen::applyLocalStyle() {
    setStyleSheet(R"(
        #StartupScreenRoot {
            background-color: #060606;
            color: #d7dde3;
        }
        #StartupBackButton {
            background-color: transparent;
            border: 1px solid rgba(0, 179, 90, 0.55);
            border-radius: 2px;
            padding: 6px 12px;
            color: #00b35a;
            font-weight: 600;
        }
        #StartupBackButton:hover {
            background-color: rgba(0, 179, 90, 0.12);
        }
        #StartupTitle {
            font-size: 22px;
            font-weight: 800;
            color: #00b35a;
            letter-spacing: 1px;
        }
        #StartupRobot {
            font-size: 13px;
            color: #a9b0b6;
        }
        #StartupStatus {
            font-size: 13px;
            color: #a9b0b6;
        }
        #StartupCalibration {
            background-color: transparent;
            border: 1px solid rgba(0, 179, 90, 0.55);
            border-radius: 2px;
            padding: 10px 14px;
            color: #00b35a;
            font-weight: 700;
            letter-spacing: 1px;
        }
        #StartupCalibration:hover {
            background-color: rgba(0, 179, 90, 0.12);
        }
        #StartupInitiate {
            background-color: #00b35a;
            border: 1px solid #00b35a;
            border-radius: 2px;
            padding: 10px 14px;
            color: #0b0b0b;
            font-weight: 800;
            letter-spacing: 1px;
        }
        #StartupInitiate:hover {
            background-color: #00c565;
            border-color: #00c565;
        }
        #StartupArrow {
            background-color: transparent;
            border: 1px solid rgba(0, 179, 90, 0.55);
            border-radius: 2px;
            color: rgba(0, 179, 90, 0.75);
            font-size: 18px;
            font-weight: 800;
        }
        #StartupArrow:enabled {
            background-color: rgba(0, 179, 90, 0.18);
            border: 1px solid rgba(0, 179, 90, 1.0);
            color: #00b35a;
        }
    )");
}

void StartupScreen::updateUiState() {
    if (lbl_robot_) {
        lbl_robot_->setText(QString("Robot ID: %1").arg(robot_id_.isEmpty() ? "-" : robot_id_));
    }
    if (lbl_status_) {
        lbl_status_->setText(startup_initiated_
                                 ? "Startup sequence initiated. You can continue."
                                 : "Press “INITIATE STARTUP SEQUENCE” to continue.");
    }
    if (btn_arrow_) {
        btn_arrow_->setEnabled(startup_initiated_);
    }
}

}  // namespace f2c_cpp

