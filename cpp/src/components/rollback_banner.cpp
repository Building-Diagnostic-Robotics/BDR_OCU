#include "components/rollback_banner.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui_theme_constants.hpp"

namespace f2c_cpp {

RollbackBanner::RollbackBanner(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("RollbackBanner"));
    buildUi();
    applyStyle();
}

void RollbackBanner::buildUi() {
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(16, 12, 16, 12);
    outer->setSpacing(12);

    // Amber warning glyph in a 40px tile, mirrors UpdateBanner's icon
    // tile shape so the two banners read as siblings of the same family.
    icon_tile_ = new QLabel(QStringLiteral("\u26A0"), this);  // ⚠
    icon_tile_->setObjectName(QStringLiteral("RollbackBanner_IconTile"));
    icon_tile_->setAlignment(Qt::AlignCenter);
    icon_tile_->setFixedSize(40, 40);
    outer->addWidget(icon_tile_);

    auto* text_col = new QVBoxLayout();
    text_col->setContentsMargins(0, 0, 0, 0);
    text_col->setSpacing(2);

    lbl_title_ = new QLabel(QStringLiteral("Update Rolled Back"), this);
    lbl_title_->setObjectName(QStringLiteral("RollbackBanner_Title"));
    text_col->addWidget(lbl_title_);

    lbl_subtitle_ = new QLabel(this);
    lbl_subtitle_->setObjectName(QStringLiteral("RollbackBanner_Subtitle"));
    lbl_subtitle_->setText(QStringLiteral(
        "The previous update did not start cleanly. The previous version "
        "has been restored. Contact support if this persists."));
    lbl_subtitle_->setWordWrap(true);
    text_col->addWidget(lbl_subtitle_);

    outer->addLayout(text_col, 1);

    btn_dismiss_ = new QPushButton(QStringLiteral("Dismiss"), this);
    btn_dismiss_->setObjectName(QStringLiteral("RollbackBanner_Dismiss"));
    btn_dismiss_->setCursor(Qt::PointingHandCursor);
    btn_dismiss_->setFixedHeight(36);
    btn_dismiss_->setMinimumWidth(116);
    connect(btn_dismiss_, &QPushButton::clicked,
            this, &RollbackBanner::onDismissClicked);
    outer->addWidget(btn_dismiss_);
}

void RollbackBanner::applyStyle() {
    const UiThemeTokens t = uiThemeTokens(dark_mode_);

    // Amber-tinted card: warning border + warning-colored icon tile.
    // Body uses the same neutral card_bg as UpdateBanner so the two read
    // as a family but with distinct severity. The Dismiss button is
    // outlined-on-warning, not solid-warning, so it doesn't compete with
    // primary CTAs elsewhere in the app.
    setStyleSheet(QStringLiteral(
        "QWidget#RollbackBanner {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 10px;"
        "}"
    ).arg(t.card_bg).arg(t.warning));

    icon_tile_->setStyleSheet(QStringLiteral(
        "QLabel#RollbackBanner_IconTile {"
        "  background-color: %1;"
        "  color: white;"
        "  border-radius: 8px;"
        "  font-size: 22px;"
        "  font-weight: 700;"
        "}"
    ).arg(t.warning));

    lbl_title_->setStyleSheet(QStringLiteral(
        "QLabel#RollbackBanner_Title {"
        "  color: %1;"
        "  font-size: 16px;"
        "  font-weight: 700;"
        "  background: transparent;"
        "  border: none;"
        "}"
    ).arg(t.text));

    lbl_subtitle_->setStyleSheet(QStringLiteral(
        "QLabel#RollbackBanner_Subtitle {"
        "  color: %1;"
        "  font-size: 13px;"
        "  background: transparent;"
        "  border: none;"
        "}"
    ).arg(t.muted));

    btn_dismiss_->setStyleSheet(QStringLiteral(
        "QPushButton#RollbackBanner_Dismiss {"
        "  background-color: transparent;"
        "  color: %1;"
        "  font-weight: 700;"
        "  font-size: 13px;"
        "  padding: 6px 14px;"
        "  border-radius: 8px;"
        "  border: 1px solid %1;"
        "}"
        "QPushButton#RollbackBanner_Dismiss:hover {"
        "  background-color: %2;"
        "}"
        "QPushButton#RollbackBanner_Dismiss:pressed {"
        "  background-color: %2;"
        "}"
    ).arg(t.warning).arg(t.card_bg));
}

void RollbackBanner::setDarkMode(bool dark_mode) {
    dark_mode_ = dark_mode;
    applyStyle();
}

void RollbackBanner::setMessage(const QString& message) {
    if (message.isEmpty()) {
        lbl_subtitle_->setText(QStringLiteral(
            "The previous update did not start cleanly. The previous "
            "version has been restored. Contact support if this persists."));
    } else {
        lbl_subtitle_->setText(message);
    }
}

void RollbackBanner::onDismissClicked() {
    emit dismissRequested();
}

}  // namespace f2c_cpp
