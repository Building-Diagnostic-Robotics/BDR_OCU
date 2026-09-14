#include "components/robot_sync_dialog.hpp"
#include "ui_theme_constants.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace f2c_cpp {

RobotSyncDialog::RobotSyncDialog(QWidget* parent) : QDialog(parent) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowModality(Qt::ApplicationModal);
    setMinimumWidth(480);
    buildUi();
    applyStyle();
}

void RobotSyncDialog::buildUi() {
    container_ = new QWidget(this);
    container_->setObjectName(QStringLiteral("RobotSyncDialogCard"));
    container_->setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(container_);

    auto* col = new QVBoxLayout(container_);
    col->setContentsMargins(24, 22, 24, 20);
    col->setSpacing(12);

    lbl_title_ = new QLabel(QStringLiteral("Robot software"), container_);
    lbl_title_->setObjectName(QStringLiteral("RobotSyncDialogTitle"));
    col->addWidget(lbl_title_);

    lbl_headline_ = new QLabel(container_);
    lbl_headline_->setObjectName(QStringLiteral("RobotSyncDialogHeadline"));
    lbl_headline_->setWordWrap(true);
    col->addWidget(lbl_headline_);

    lbl_detail_ = new QLabel(container_);
    lbl_detail_->setObjectName(QStringLiteral("RobotSyncDialogDetail"));
    lbl_detail_->setWordWrap(true);
    col->addWidget(lbl_detail_);

    lbl_stage_ = new QLabel(container_);
    lbl_stage_->setObjectName(QStringLiteral("RobotSyncDialogStage"));
    lbl_stage_->hide();
    col->addWidget(lbl_stage_);

    busy_bar_ = new QProgressBar(container_);
    busy_bar_->setObjectName(QStringLiteral("RobotSyncDialogBar"));
    busy_bar_->setRange(0, 0);
    busy_bar_->setTextVisible(false);
    busy_bar_->hide();
    col->addWidget(busy_bar_);

    auto* row = new QHBoxLayout();
    row->setSpacing(10);
    btn_prepare_ = new QPushButton(QStringLiteral("Prepare robot"), container_);
    btn_prepare_->setObjectName(QStringLiteral("RobotSyncDialogSecondary"));
    btn_prepare_->setCursor(Qt::PointingHandCursor);
    btn_prepare_->setMinimumHeight(40);
    btn_prepare_->hide();
    connect(btn_prepare_, &QPushButton::clicked, this, &RobotSyncDialog::prepareRequested);
    row->addWidget(btn_prepare_);

    row->addStretch();

    btn_later_ = new QPushButton(QStringLiteral("Later"), container_);
    btn_later_->setObjectName(QStringLiteral("RobotSyncDialogSecondary"));
    btn_later_->setCursor(Qt::PointingHandCursor);
    btn_later_->setMinimumHeight(40);
    connect(btn_later_, &QPushButton::clicked, this, [this]() {
        emit laterRequested();
        hide();
    });
    row->addWidget(btn_later_);

    btn_sync_ = new QPushButton(QStringLiteral("Sync now"), container_);
    btn_sync_->setObjectName(QStringLiteral("RobotSyncDialogPrimary"));
    btn_sync_->setCursor(Qt::PointingHandCursor);
    btn_sync_->setMinimumHeight(40);
    connect(btn_sync_, &QPushButton::clicked, this, &RobotSyncDialog::syncRequested);
    row->addWidget(btn_sync_);

    btn_close_ = new QPushButton(QStringLiteral("Close"), container_);
    btn_close_->setObjectName(QStringLiteral("RobotSyncDialogSecondary"));
    btn_close_->setCursor(Qt::PointingHandCursor);
    btn_close_->setMinimumHeight(40);
    btn_close_->hide();
    connect(btn_close_, &QPushButton::clicked, this, &QDialog::hide);
    row->addWidget(btn_close_);

    col->addLayout(row);
}

void RobotSyncDialog::applyStyle() {
    const auto t = uiThemeTokens(dark_mode_);
    const QString bg = dark_mode_ ? QStringLiteral("#18181B") : QStringLiteral("#FFFFFF");
    const QString border = dark_mode_ ? QStringLiteral("#3F3F46") : QStringLiteral("#E5E7EB");
    const QString text = dark_mode_ ? QStringLiteral("#F4F4F5") : QStringLiteral("#111827");
    const QString muted = dark_mode_ ? QStringLiteral("#A1A1AA") : QStringLiteral("#52525B");
    const QString control = dark_mode_ ? QStringLiteral("#27272A") : QStringLiteral("#F9FAFB");

    setStyleSheet(QStringLiteral(R"CSS(
        QWidget#RobotSyncDialogCard {
            background-color: %1; border: 1px solid %2; border-radius: 14px;
        }
        QLabel#RobotSyncDialogTitle {
            font-family: 'Arimo'; font-size: 22px; font-weight: 700;
            color: %3; background: transparent;
        }
        QLabel#RobotSyncDialogHeadline {
            font-family: 'Arimo'; font-size: 15px; font-weight: 600;
            color: %3; background: transparent;
        }
        QLabel#RobotSyncDialogDetail, QLabel#RobotSyncDialogStage {
            font-family: 'Arimo'; font-size: 13px; font-weight: 500;
            color: %4; background: transparent;
        }
        QProgressBar#RobotSyncDialogBar {
            background: %5; border: 1px solid %2; border-radius: 4px;
            min-height: 8px; max-height: 8px;
        }
        QProgressBar#RobotSyncDialogBar::chunk { background: %6; border-radius: 3px; }
        QPushButton#RobotSyncDialogPrimary {
            background: %6; color: #FFFFFF; border: none; border-radius: 8px;
            font-family: 'Arimo'; font-size: 14px; font-weight: 600; padding: 10px 18px;
        }
        QPushButton#RobotSyncDialogPrimary:hover { background: %7; }
        QPushButton#RobotSyncDialogPrimary:disabled { background: %5; color: %4; }
        QPushButton#RobotSyncDialogSecondary {
            background: %5; color: %3; border: 1px solid %2; border-radius: 8px;
            font-family: 'Arimo'; font-size: 14px; font-weight: 600; padding: 10px 18px;
        }
    )CSS")
                      .arg(bg, border, text, muted, control, t.accent, t.accent_hover));
}

void RobotSyncDialog::setDarkMode(bool dark) {
    dark_mode_ = dark;
    applyStyle();
}

void RobotSyncDialog::setSnapshot(const RepoSyncSnapshot& snap) {
    snap_ = snap;
    lbl_headline_->setText(snap.headline);
    lbl_detail_->setText(snap.detail);
    btn_prepare_->setVisible(snap.needs_prepare && !busy_);
    refreshButtons();
}

void RobotSyncDialog::setGateState(const GateState& gate) {
    gate_ = gate;
    refreshButtons();
}

void RobotSyncDialog::setBusy(bool busy, const QString& stage) {
    busy_ = busy;
    lbl_stage_->setVisible(busy && !stage.isEmpty());
    lbl_stage_->setText(stage);
    busy_bar_->setVisible(busy);
    btn_close_->setVisible(false);
    btn_later_->setVisible(!busy);
    btn_sync_->setVisible(!busy);
    btn_prepare_->setVisible(snap_.needs_prepare && !busy);
    refreshButtons();
}

void RobotSyncDialog::setFinished(int level, const QString& headline, const QString& detail) {
    Q_UNUSED(level);
    busy_ = false;
    lbl_headline_->setText(headline);
    lbl_detail_->setText(detail);
    lbl_stage_->hide();
    busy_bar_->hide();
    btn_later_->hide();
    btn_sync_->setVisible(level != RepoSyncManager::LevelOk);
    btn_prepare_->setVisible(snap_.needs_prepare);
    btn_close_->show();
    refreshButtons();
}

void RobotSyncDialog::refreshButtons() {
    QString why;
    if (gate_.launch_active) {
        why = QStringLiteral("A scan is active. Complete Mission first.");
    } else if (gate_.battery_pct >= 0 && gate_.battery_pct < 20) {
        why = QStringLiteral("Battery below 20%. Plug in before syncing.");
    }
    const bool ok = why.isEmpty() && !busy_;
    btn_sync_->setEnabled(ok);
    btn_sync_->setToolTip(why);
    btn_prepare_->setEnabled(ok);
    btn_prepare_->setToolTip(why);
}

}  // namespace f2c_cpp
