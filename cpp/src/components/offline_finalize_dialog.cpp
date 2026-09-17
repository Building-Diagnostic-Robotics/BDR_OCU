#include "components/offline_finalize_dialog.hpp"

#include "ui_theme_constants.hpp"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace f2c_cpp {

OfflineFinalizeDialog::OfflineFinalizeDialog(LinkHealthMonitor* monitor,
                                             QWidget* parent)
    : QDialog(parent),
      monitor_(monitor) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    setFixedWidth(560);
    buildUi();
    applyStyle();

    tick_timer_ = new QTimer(this);
    tick_timer_->setInterval(1000);
    connect(tick_timer_, &QTimer::timeout, this, &OfflineFinalizeDialog::onTick);
    tick_timer_->start();

    if (monitor_) {
        connect(monitor_, &LinkHealthMonitor::linkStateChanged,
                this, &OfflineFinalizeDialog::onLinkStateChanged);
    }

    refreshStatusLabels();
}

void OfflineFinalizeDialog::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(16);

    lbl_header_ = new QLabel(QStringLiteral("Robot Offline"), this);
    lbl_header_->setObjectName("OfflineFinalizeHeader");
    root->addWidget(lbl_header_);

    lbl_body_ = new QLabel(this);
    lbl_body_->setObjectName("OfflineFinalizeBody");
    lbl_body_->setWordWrap(true);
    lbl_body_->setText(QStringLiteral(
        "Your scan data is safe on the robot's disk. The mission can "
        "be finalized as soon as the radio reconnects, or you can "
        "save it now over SSH if you don't want to wait."));
    root->addWidget(lbl_body_);

    lbl_link_status_ = new QLabel(this);
    lbl_link_status_->setObjectName("OfflineFinalizeStatus");
    root->addWidget(lbl_link_status_);

    lbl_countdown_ = new QLabel(this);
    lbl_countdown_->setObjectName("OfflineFinalizeCountdown");
    lbl_countdown_->setVisible(false);
    root->addWidget(lbl_countdown_);

    auto* button_row = new QHBoxLayout();
    button_row->setContentsMargins(0, 8, 0, 0);
    button_row->setSpacing(12);
    btn_cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
    btn_cancel_->setObjectName("OfflineFinalizeCancel");
    btn_cancel_->setCursor(Qt::PointingHandCursor);
    btn_cancel_->setMinimumHeight(40);
    btn_ssh_ = new QPushButton(QStringLiteral("Finalize via SSH"), this);
    btn_ssh_->setObjectName("OfflineFinalizeSsh");
    btn_ssh_->setCursor(Qt::PointingHandCursor);
    btn_ssh_->setMinimumHeight(40);
    btn_wait_ = new QPushButton(QStringLiteral("Wait for reconnect"), this);
    btn_wait_->setObjectName("OfflineFinalizeWait");
    btn_wait_->setCursor(Qt::PointingHandCursor);
    btn_wait_->setMinimumHeight(40);
    btn_wait_->setDefault(true);
    btn_wait_->setAutoDefault(true);
    button_row->addWidget(btn_cancel_, 1);
    button_row->addWidget(btn_ssh_, 1);
    button_row->addWidget(btn_wait_, 2);
    root->addLayout(button_row);

    connect(btn_wait_, &QPushButton::clicked,
            this, &OfflineFinalizeDialog::onWaitClicked);
    connect(btn_ssh_, &QPushButton::clicked,
            this, &OfflineFinalizeDialog::onSshClicked);
    connect(btn_cancel_, &QPushButton::clicked,
            this, &OfflineFinalizeDialog::onCancelClicked);
}

void OfflineFinalizeDialog::applyStyle() {
    const UiThemeTokens t = appThemeTokens();
    setStyleSheet(QStringLiteral(R"CSS(
        OfflineFinalizeDialog {
            background: %1;
            border: 1px solid %2;
            border-radius: 14px;
        }
        QLabel#OfflineFinalizeHeader {
            color: %3;
            font-family: 'Arimo';
            font-size: 20px;
            font-weight: 700;
            background: transparent;
        }
        QLabel#OfflineFinalizeBody {
            color: %4;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 400;
            background: transparent;
        }
        QLabel#OfflineFinalizeStatus {
            color: %5;
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 600;
            background: transparent;
        }
        QLabel#OfflineFinalizeCountdown {
            color: %6;
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 600;
            background: transparent;
        }
        QPushButton#OfflineFinalizeWait {
            background: %7;
            border: none;
            border-radius: 8px;
            color: #FFFFFF;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 700;
            padding: 0 18px;
        }
        QPushButton#OfflineFinalizeWait:hover { background: %8; }
        QPushButton#OfflineFinalizeSsh {
            background: %9;
            border: none;
            border-radius: 8px;
            color: #FFFFFF;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 700;
            padding: 0 18px;
        }
        QPushButton#OfflineFinalizeSsh:hover { background: %10; }
        QPushButton#OfflineFinalizeCancel {
            background: transparent;
            border: 1px solid %2;
            border-radius: 8px;
            color: %4;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 600;
            padding: 0 18px;
        }
        QPushButton#OfflineFinalizeCancel:hover {
            background: %11;
            border-color: %12;
        }
    )CSS")
                      .arg(t.surface, t.raised_border, t.text, t.body,
                           t.danger, t.warning, t.info_fill, t.info_fill_hover,
                           t.warning_fill)
                      .arg(t.warning_fill_hover, t.neutral_hover, t.muted));
}

void OfflineFinalizeDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    refreshStatusLabels();
    btn_wait_->setFocus(Qt::OtherFocusReason);
}

void OfflineFinalizeDialog::onWaitClicked() {
    enterWaitMode();
}

void OfflineFinalizeDialog::onSshClicked() {
    chosen_ = Choice::FinalizeOverSsh;
    accept();
}

void OfflineFinalizeDialog::onCancelClicked() {
    chosen_ = Choice::Cancelled;
    reject();
}

void OfflineFinalizeDialog::onLinkStateChanged(LinkHealthMonitor::State,
                                               LinkHealthMonitor::State new_state,
                                               qint64) {
    if (wait_mode_ && new_state == LinkHealthMonitor::State::Healthy) {
        chosen_ = Choice::WaitReconnected;
        accept();
        return;
    }
    refreshStatusLabels();
}

void OfflineFinalizeDialog::onTick() {
    refreshStatusLabels();
    if (wait_mode_ && wait_started_at_ms_ > 0) {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - wait_started_at_ms_;
        if (elapsed >= kAutoFallbackMs) {
            // 5-min ceiling: fall back to SSH so the bot doesn't sit
            // armed indefinitely if the operator forgot the modal.
            chosen_ = Choice::FinalizeOverSsh;
            accept();
        }
    }
}

void OfflineFinalizeDialog::refreshStatusLabels() {
    if (!lbl_link_status_ || !lbl_countdown_) {
        return;
    }
    if (monitor_ && monitor_->isArmed()) {
        const qint64 since = monitor_->msSinceLastSeen();
        const qint64 since_sec = since >= 0 ? (since + 500) / 1000 : 0;
        switch (monitor_->state()) {
            case LinkHealthMonitor::State::Healthy:
                lbl_link_status_->setText(
                    QStringLiteral("Link: LIVE (last contact %1s ago)").arg(since_sec));
                break;
            case LinkHealthMonitor::State::Reconnecting:
                lbl_link_status_->setText(
                    QStringLiteral("Link: RECONNECTING (last contact %1s ago)").arg(since_sec));
                break;
            case LinkHealthMonitor::State::Disconnected:
                lbl_link_status_->setText(
                    QStringLiteral("Link: OFFLINE (last contact %1s ago)").arg(since_sec));
                break;
            case LinkHealthMonitor::State::Idle:
                lbl_link_status_->setText(QStringLiteral("Link: IDLE"));
                break;
        }
    } else {
        lbl_link_status_->setText(QStringLiteral("Link: not monitored"));
    }
    if (wait_mode_ && wait_started_at_ms_ > 0) {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - wait_started_at_ms_;
        const qint64 remaining = std::max(qint64{0}, qint64{kAutoFallbackMs} - elapsed);
        const int rem_sec = static_cast<int>((remaining + 500) / 1000);
        const int mm = rem_sec / 60;
        const int ss = rem_sec % 60;
        lbl_countdown_->setText(QString::fromLatin1(
            "Auto-fallback to SSH in %1:%2 if no reconnect.")
            .arg(mm).arg(ss, 2, 10, QLatin1Char('0')));
    }
}

void OfflineFinalizeDialog::enterWaitMode() {
    wait_mode_ = true;
    wait_started_at_ms_ = QDateTime::currentMSecsSinceEpoch();
    if (btn_wait_) {
        btn_wait_->setEnabled(false);
        btn_wait_->setText(QStringLiteral("Waiting..."));
    }
    if (lbl_countdown_) {
        lbl_countdown_->setVisible(true);
    }
    refreshStatusLabels();
}

}  // namespace f2c_cpp
