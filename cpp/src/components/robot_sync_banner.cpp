#include "components/robot_sync_banner.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "components/robot_update_copy.hpp"
#include "ui_theme_constants.hpp"

namespace f2c_cpp {

RobotSyncBanner::RobotSyncBanner(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("RobotSyncBanner"));
    buildUi();
    applyStyle();
}

void RobotSyncBanner::buildUi() {
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(16, 12, 16, 12);
    outer->setSpacing(12);

    icon_tile_ = new QLabel(QStringLiteral("\u21BB"), this);
    icon_tile_->setObjectName(QStringLiteral("RobotSyncBanner_IconTile"));
    icon_tile_->setAlignment(Qt::AlignCenter);
    icon_tile_->setFixedSize(40, 40);
    outer->addWidget(icon_tile_);

    auto* text_col = new QVBoxLayout();
    text_col->setContentsMargins(0, 0, 0, 0);
    text_col->setSpacing(2);

    auto* title_row = new QHBoxLayout();
    title_row->setContentsMargins(0, 0, 0, 0);
    title_row->setSpacing(8);

    lbl_title_ = new QLabel(robot_update_copy::bannerTitle(), this);
    lbl_title_->setObjectName(QStringLiteral("RobotSyncBanner_Title"));
    title_row->addWidget(lbl_title_);

    title_row->addStretch();
    text_col->addLayout(title_row);

    lbl_subtitle_ = new QLabel(robot_update_copy::bannerSubtitle(), this);
    lbl_subtitle_->setObjectName(QStringLiteral("RobotSyncBanner_Subtitle"));
    text_col->addWidget(lbl_subtitle_);

    outer->addLayout(text_col, 1);

    btn_view_ = new QPushButton(QStringLiteral("Update"), this);
    btn_view_->setObjectName(QStringLiteral("RobotSyncBanner_ViewDetails"));
    btn_view_->setCursor(Qt::PointingHandCursor);
    btn_view_->setFixedHeight(36);
    btn_view_->setMinimumWidth(116);
    connect(btn_view_, &QPushButton::clicked, this, &RobotSyncBanner::viewDetailsRequested);
    outer->addWidget(btn_view_);
}

void RobotSyncBanner::applyStyle() {
    const UiThemeTokens t = uiThemeTokens(dark_mode_);
    setStyleSheet(QStringLiteral(
        "QWidget#RobotSyncBanner {"
        "  background-color: %1; border: 1px solid %2; border-radius: 10px;"
        "}").arg(t.card_bg, t.warning));
    icon_tile_->setStyleSheet(QStringLiteral(
        "QLabel#RobotSyncBanner_IconTile {"
        "  background-color: %1; color: white; border-radius: 8px;"
        "  font-size: 22px; font-weight: 700;"
        "}").arg(t.warning));
    lbl_title_->setStyleSheet(QStringLiteral(
        "QLabel#RobotSyncBanner_Title {"
        "  color: %1; font-size: 16px; font-weight: 700;"
        "  background: transparent; border: none;"
        "}").arg(t.text));
    lbl_subtitle_->setStyleSheet(QStringLiteral(
        "QLabel#RobotSyncBanner_Subtitle {"
        "  color: %1; font-size: 13px; background: transparent; border: none;"
        "}").arg(t.muted));
    btn_view_->setStyleSheet(QStringLiteral(
        "QPushButton#RobotSyncBanner_ViewDetails {"
        "  background-color: %1; color: white; font-weight: 700; font-size: 13px;"
        "  padding: 6px 14px; border-radius: 8px; border: none;"
        "}"
        "QPushButton#RobotSyncBanner_ViewDetails:hover { background-color: %2; }"
        ).arg(t.warning,
              dark_mode_ ? QStringLiteral("#D97706") : QStringLiteral("#B45309")));
}

void RobotSyncBanner::setDarkMode(bool dark_mode) {
    dark_mode_ = dark_mode;
    applyStyle();
}

void RobotSyncBanner::setSnapshot(const RepoSyncSnapshot& snap) {
    // Same words whatever the technical reason (laptop behind, robot
    // behind, helpers missing): the dialog handles all of them.
    snap_ = snap;
}

}  // namespace f2c_cpp
