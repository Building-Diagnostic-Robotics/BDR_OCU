#include "components/robot_sync_dialog.hpp"

#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include "ui_theme_constants.hpp"

namespace f2c_cpp {

namespace copy = robot_update_copy;

RobotSyncDialog::RobotSyncDialog(QWidget* parent) : QDialog(parent) {
    power_poll_timer_ = new QTimer(this);
    power_poll_timer_->setInterval(5000);
    connect(power_poll_timer_, &QTimer::timeout, this, &RobotSyncDialog::pollPower);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowModality(Qt::ApplicationModal);
    setMinimumWidth(520);
    dark_mode_ = appDarkMode();
    buildUi();
    applyStyle();
    render();
}

void RobotSyncDialog::buildUi() {
    container_ = new QWidget(this);
    container_->setObjectName(QStringLiteral("RobotSyncDialog_Container"));
    container_->setAttribute(Qt::WA_StyledBackground, true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(container_);

    auto* root = new QVBoxLayout(container_);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);

    // Header: icon tile, title + subtitle, close.
    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(12);
    lbl_icon_ = new QLabel(QStringLiteral("↻"), container_);
    lbl_icon_->setObjectName(QStringLiteral("RobotSyncDialog_Icon"));
    lbl_icon_->setAlignment(Qt::AlignCenter);
    lbl_icon_->setFixedSize(40, 40);
    header->addWidget(lbl_icon_, 0, Qt::AlignTop);

    auto* titles = new QVBoxLayout();
    titles->setContentsMargins(0, 0, 0, 0);
    titles->setSpacing(2);
    lbl_title_ = new QLabel(QStringLiteral("ROOFUS Update"), container_);
    lbl_title_->setObjectName(QStringLiteral("RobotSyncDialog_Title"));
    titles->addWidget(lbl_title_);
    lbl_subtitle_ = new QLabel(container_);
    lbl_subtitle_->setObjectName(QStringLiteral("RobotSyncDialog_Subtitle"));
    titles->addWidget(lbl_subtitle_);
    header->addLayout(titles, 1);

    btn_x_ = new QPushButton(QStringLiteral("×"), container_);
    btn_x_->setObjectName(QStringLiteral("RobotSyncDialog_Close"));
    btn_x_->setFlat(true);
    btn_x_->setCursor(Qt::PointingHandCursor);
    btn_x_->setFixedSize(36, 36);
    connect(btn_x_, &QPushButton::clicked, this, &QDialog::hide);
    header->addWidget(btn_x_, 0, Qt::AlignTop);
    root->addLayout(header);

    lbl_message_ = new QLabel(container_);
    lbl_message_->setObjectName(QStringLiteral("RobotSyncDialog_Message"));
    lbl_message_->setWordWrap(true);
    root->addWidget(lbl_message_);

    // Why Update Now is disabled (touchscreens never show tooltips).
    lbl_notice_ = new QLabel(container_);
    lbl_notice_->setObjectName(QStringLiteral("RobotSyncDialog_Notice"));
    lbl_notice_->setWordWrap(true);
    lbl_notice_->hide();
    root->addWidget(lbl_notice_);

    lbl_step_ = new QLabel(container_);
    lbl_step_->setObjectName(QStringLiteral("RobotSyncDialog_Step"));
    lbl_step_->hide();
    root->addWidget(lbl_step_);

    busy_bar_ = new QProgressBar(container_);
    busy_bar_->setObjectName(QStringLiteral("RobotSyncDialog_Bar"));
    busy_bar_->setRange(0, 0);
    busy_bar_->setTextVisible(false);
    busy_bar_->hide();
    root->addWidget(busy_bar_);

    btn_details_ = new QPushButton(QStringLiteral("Details for BDR support"), container_);
    btn_details_->setObjectName(QStringLiteral("RobotSyncDialog_DetailsToggle"));
    btn_details_->setFlat(true);
    btn_details_->setCursor(Qt::PointingHandCursor);
    btn_details_->hide();
    connect(btn_details_, &QPushButton::clicked, this, [this]() {
        lbl_details_->setVisible(!lbl_details_->isVisible());
        adjustSize();
    });
    root->addWidget(btn_details_, 0, Qt::AlignLeft);

    lbl_details_ = new QLabel(container_);
    lbl_details_->setObjectName(QStringLiteral("RobotSyncDialog_Details"));
    lbl_details_->setWordWrap(true);
    lbl_details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lbl_details_->hide();
    root->addWidget(lbl_details_);

    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 4, 0, 0);
    footer->setSpacing(12);
    footer->addStretch();
    btn_secondary_ = new QPushButton(container_);
    btn_secondary_->setObjectName(QStringLiteral("RobotSyncDialog_Secondary"));
    btn_secondary_->setCursor(Qt::PointingHandCursor);
    btn_secondary_->setFixedHeight(44);
    btn_secondary_->setMinimumWidth(140);
    connect(btn_secondary_, &QPushButton::clicked, this, [this]() {
        if (phase_ == Phase::Offer) {
            emit laterRequested();
        }
        hide();
    });
    footer->addWidget(btn_secondary_);
    btn_primary_ = new QPushButton(container_);
    btn_primary_->setObjectName(QStringLiteral("RobotSyncDialog_Primary"));
    btn_primary_->setCursor(Qt::PointingHandCursor);
    btn_primary_->setFixedHeight(44);
    btn_primary_->setMinimumWidth(140);
    connect(btn_primary_, &QPushButton::clicked, this, &RobotSyncDialog::onPrimary);
    footer->addWidget(btn_primary_);
    root->addLayout(footer);
}

void RobotSyncDialog::applyStyle() {
    const UiThemeTokens t = uiThemeTokens(dark_mode_);
    setStyleSheet(QStringLiteral(
        "QWidget#RobotSyncDialog_Container {"
        "  background-color: %1; border: 1px solid %2; border-radius: 12px; }"
        "QLabel#RobotSyncDialog_Icon {"
        "  background-color: %3; color: white; border-radius: 8px;"
        "  font-size: 22px; font-weight: 700; }"
        "QLabel#RobotSyncDialog_Title {"
        "  color: %4; font-size: 18px; font-weight: 700; background: transparent; border: none; }"
        "QLabel#RobotSyncDialog_Subtitle {"
        "  color: %5; font-size: 13px; background: transparent; border: none; }"
        "QPushButton#RobotSyncDialog_Close {"
        "  color: %5; background: transparent; border: none; font-size: 22px; font-weight: 600; }"
        "QPushButton#RobotSyncDialog_Close:hover { color: %4; background-color: %6; border-radius: 6px; }"
        "QLabel#RobotSyncDialog_Message {"
        "  color: %4; font-size: 15px; background: transparent; border: none; }"
        "QLabel#RobotSyncDialog_Notice {"
        "  color: %7; background-color: rgba(245, 158, 11, 0.12); border: 1px solid %7;"
        "  border-radius: 8px; padding: 10px 12px; font-size: 13px; font-weight: 600; }"
        "QLabel#RobotSyncDialog_Step {"
        "  color: %5; font-size: 13px; font-weight: 600; background: transparent; border: none; }"
        "QProgressBar#RobotSyncDialog_Bar {"
        "  background: %6; border: 1px solid %2; border-radius: 4px; min-height: 8px; max-height: 8px; }"
        "QProgressBar#RobotSyncDialog_Bar::chunk { background: %3; border-radius: 3px; }"
        "QPushButton#RobotSyncDialog_DetailsToggle {"
        "  color: %5; background: transparent; border: none; font-size: 12px;"
        "  text-decoration: underline; padding: 0px; }"
        "QLabel#RobotSyncDialog_Details {"
        "  color: %5; background-color: %6; border: 1px solid %2; border-radius: 8px;"
        "  padding: 8px 10px; font-family: monospace; font-size: 11px; }"
        "QPushButton#RobotSyncDialog_Secondary {"
        "  color: %5; background-color: transparent; border: 1px solid %2; border-radius: 8px;"
        "  font-size: 14px; font-weight: 600; padding: 0px 16px; }"
        "QPushButton#RobotSyncDialog_Secondary:hover { color: %4; background-color: %6; }"
        "QPushButton#RobotSyncDialog_Primary {"
        "  color: white; background-color: %3; border: none; border-radius: 8px;"
        "  font-size: 14px; font-weight: 700; padding: 0px 18px; }"
        "QPushButton#RobotSyncDialog_Primary:hover { background-color: %8; }"
        "QPushButton#RobotSyncDialog_Primary:disabled { background-color: %2; color: %5; }")
                      .arg(t.card_bg)       // %1
                      .arg(t.border)        // %2
                      .arg(t.accent)        // %3
                      .arg(t.text)          // %4
                      .arg(t.muted)         // %5
                      .arg(t.bg)            // %6
                      .arg(t.warning)       // %7
                      .arg(t.accent_hover)); // %8
}

void RobotSyncDialog::setDarkMode(bool dark) {
    dark_mode_ = dark;
    applyStyle();
}

void RobotSyncDialog::setSnapshot(const RepoSyncSnapshot& snap) {
    snap_ = snap;
    render();
}

void RobotSyncDialog::setGateState(const GateState& gate) {
    gate_ = gate;
    refreshButtons();
}

void RobotSyncDialog::setBusy(bool busy, const QString& stage) {
    if (busy) {
        phase_ = Phase::Busy;
        stage_ = stage;
    } else {
        // Opening the dialog: start from what the last check found.
        phase_ = Phase::Offer;
        operator_run_ = false;
        sync_after_prepare_ = false;
    }
    render();
}

void RobotSyncDialog::setFinished(int level, const QString& headline, const QString& detail) {
    if (sync_after_prepare_) {
        sync_after_prepare_ = false;
        if (level == RepoSyncManager::LevelOk) {
            // Helpers are in: carry on with the update the operator asked
            // for. Queued: we are inside RepoSyncManager's finish().
            phase_ = Phase::Busy;
            render();
            QTimer::singleShot(0, this, [this]() { emit syncRequested(); });
            return;
        }
    }
    if (!operator_run_) {
        // A background status check: just show what it found.
        phase_ = Phase::Offer;
        render();
        return;
    }
    phase_ = Phase::Finished;
    finished_level_ = level;
    tech_headline_ = headline;
    tech_detail_ = detail;
    render();
}

void RobotSyncDialog::onPrimary() {
    if (isDoneView()) {
        hide();
        return;
    }
    operator_run_ = true;
    if (snap_.needs_prepare) {
        sync_after_prepare_ = true;
        emit prepareRequested();
    } else {
        emit syncRequested();
    }
}

bool RobotSyncDialog::isDoneView() const {
    if (phase_ == Phase::Finished) {
        return copy::outcome(finished_level_, snap_) == copy::Outcome::Done;
    }
    return phase_ == Phase::Offer && !snap_.offer_update && snap_.robot_online;
}

void RobotSyncDialog::render() {
    copy::Text text;
    bool show_details = false;
    bool show_secondary = true;
    const bool busy = phase_ == Phase::Busy;
    const bool done = isDoneView();
    if (busy) {
        text = operator_run_ ? copy::busyText() : copy::checkingText();
    } else if (phase_ == Phase::Finished) {
        const copy::Outcome o = copy::outcome(finished_level_, snap_);
        text = copy::outcomeText(o);
        show_details = (o == copy::Outcome::Failed);
        btn_secondary_->setText(QStringLiteral("Close"));
        btn_primary_->setText(done ? QStringLiteral("Close") : QStringLiteral("Try Again"));
        show_secondary = !done;
    } else if (done) {
        text = copy::outcomeText(copy::Outcome::Done);
        btn_primary_->setText(QStringLiteral("Close"));
        show_secondary = false;
    } else {
        text = copy::offerText(snap_);
        btn_secondary_->setText(QStringLiteral("Later"));
        btn_primary_->setText(QStringLiteral("Update Now"));
    }
    lbl_subtitle_->setText(text.subtitle);
    lbl_message_->setText(text.message);

    const bool show_step = busy && operator_run_;
    lbl_step_->setVisible(show_step);
    lbl_step_->setText(show_step ? copy::stepText(copy::stepForStage(stage_)) : QString());
    busy_bar_->setVisible(busy);
    btn_x_->setVisible(!busy);
    btn_secondary_->setVisible(!busy && show_secondary);
    btn_primary_->setVisible(!busy);

    btn_details_->setVisible(show_details);
    if (!show_details) {
        lbl_details_->hide();
    }
    lbl_details_->setText(
        tech_detail_.isEmpty() ? tech_headline_
                               : tech_headline_ + QLatin1Char('\n') + tech_detail_);
    refreshButtons();
    adjustSize();
}

void RobotSyncDialog::refreshButtons() {
    const bool done = isDoneView();
    const QString why =
        (phase_ == Phase::Busy || done)
            ? QString()
            : copy::blockerText(update::lowBatteryBlocks(gate_.power));
    btn_primary_->setEnabled(why.isEmpty());
    lbl_notice_->setText(why);
    lbl_notice_->setVisible(!why.isEmpty());
}

void RobotSyncDialog::pollPower() {
    const update::PowerStatus power = update::readPowerStatus();
    if (power == gate_.power) {
        return;
    }
    gate_.power = power;
    refreshButtons();
}

void RobotSyncDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    pollPower();
    power_poll_timer_->start();
}

void RobotSyncDialog::hideEvent(QHideEvent* event) {
    power_poll_timer_->stop();
    QDialog::hideEvent(event);
}

}  // namespace f2c_cpp
