#include "app_shell.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "setup_screen.hpp"
#include "startup_screen.hpp"

namespace f2c_cpp {

AppShellWindow::AppShellWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("BDR Coverage Planning Suite");
    setMinimumSize(1100, 700);

    stack_ = new QStackedWidget(this);
    setCentralWidget(stack_);

    stage1_ = new SetupScreen(this);
    stage2_ = new StartupScreen(this);
    stage3_ = buildStage3Placeholder();

    stack_->addWidget(stage1_);
    stack_->addWidget(stage2_);
    stack_->addWidget(stage3_);
    stack_->setCurrentWidget(stage1_);

    connect(stage1_, &SetupScreen::loginSubmitted, this, &AppShellWindow::onLoginSubmitted);
    connect(stage2_, &StartupScreen::backRequested, this, &AppShellWindow::goToStage1);
    connect(stage2_, &StartupScreen::continueRequested, this, &AppShellWindow::goToStage3);
}

void AppShellWindow::onLoginSubmitted(const QString& robotId, const QString& accessCode) {
    robot_id_ = robotId;
    access_code_ = accessCode;

    if (stage2_) {
        stage2_->setRobotId(robot_id_);
        stage2_->resetState();
    }
    goToStage2();
}

void AppShellWindow::goToStage1() {
    stack_->setCurrentWidget(stage1_);
}

void AppShellWindow::goToStage2() {
    if (stage2_) {
        stack_->setCurrentWidget(stage2_);
    }
}

void AppShellWindow::goToStage3() {
    updateStage3Labels();
    if (stage3_) {
        stack_->setCurrentWidget(stage3_);
    }
}

QWidget* AppShellWindow::buildStage3Placeholder() {
    auto* w = new QWidget(this);
    w->setObjectName("Stage3PlaceholderRoot");

    auto* root = new QVBoxLayout(w);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(18);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);

    auto* btn_back = new QPushButton("← Back", w);
    btn_back->setObjectName("Stage3BackButton");
    btn_back->setFixedHeight(36);
    connect(btn_back, &QPushButton::clicked, this, &AppShellWindow::goToStage2);
    header->addWidget(btn_back, 0, Qt::AlignLeft);
    header->addStretch(1);

    root->addLayout(header);

    lbl_stage3_title_ = new QLabel("Stage 3", w);
    lbl_stage3_title_->setObjectName("Stage3Title");
    root->addWidget(lbl_stage3_title_);

    lbl_stage3_subtitle_ = new QLabel("Next screen not implemented yet.", w);
    lbl_stage3_subtitle_->setObjectName("Stage3Subtitle");
    lbl_stage3_subtitle_->setWordWrap(true);
    root->addWidget(lbl_stage3_subtitle_);

    root->addStretch(1);

    // Local styling (kept simple for now)
    w->setStyleSheet(R"(
        #Stage3PlaceholderRoot {
            background-color: #060606;
            color: #d7dde3;
        }
        #Stage3BackButton {
            background-color: transparent;
            border: 1px solid rgba(0, 179, 90, 0.55);
            border-radius: 2px;
            padding: 6px 12px;
            color: #00b35a;
            font-weight: 600;
        }
        #Stage3BackButton:hover {
            background-color: rgba(0, 179, 90, 0.12);
        }
        #Stage3Title {
            font-size: 22px;
            font-weight: 800;
            color: #00b35a;
        }
        #Stage3Subtitle {
            font-size: 13px;
            color: #a9b0b6;
        }
    )");

    return w;
}

void AppShellWindow::updateStage3Labels() {
    if (!lbl_stage3_title_ || !lbl_stage3_subtitle_) {
        return;
    }
    lbl_stage3_title_->setText("Stage 3");
    lbl_stage3_subtitle_->setText(
        QString("Robot ID: %1\n\nNext: we’ll implement the third screen here.")
            .arg(robot_id_.isEmpty() ? "-" : robot_id_));
}

}  // namespace f2c_cpp

