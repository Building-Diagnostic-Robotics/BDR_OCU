#include "components/update_banner.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui_theme_constants.hpp"

namespace f2c_cpp {

UpdateBanner::UpdateBanner(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("UpdateBanner"));
    buildUi();
    applyStyle();
}

void UpdateBanner::buildUi() {
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(16, 12, 16, 12);
    outer->setSpacing(12);

    // Icon tile — solid accent square w/ a download arrow glyph. SVG art is
    // deferred (phase 4 stays minimal); a Unicode arrow inside a colored
    // QLabel reads cleanly at 40px.
    icon_tile_ = new QLabel(QStringLiteral("\u21A7"), this);
    icon_tile_->setObjectName(QStringLiteral("UpdateBanner_IconTile"));
    icon_tile_->setAlignment(Qt::AlignCenter);
    icon_tile_->setFixedSize(40, 40);
    outer->addWidget(icon_tile_);

    auto* text_col = new QVBoxLayout();
    text_col->setContentsMargins(0, 0, 0, 0);
    text_col->setSpacing(2);

    auto* title_row = new QHBoxLayout();
    title_row->setContentsMargins(0, 0, 0, 0);
    title_row->setSpacing(8);

    lbl_title_ = new QLabel(QStringLiteral("System Update Available"), this);
    lbl_title_->setObjectName(QStringLiteral("UpdateBanner_Title"));
    title_row->addWidget(lbl_title_);

    lbl_version_pill_ = new QLabel(this);
    lbl_version_pill_->setObjectName(QStringLiteral("UpdateBanner_VersionPill"));
    lbl_version_pill_->setAlignment(Qt::AlignCenter);
    title_row->addWidget(lbl_version_pill_);

    title_row->addStretch();
    text_col->addLayout(title_row);

    lbl_subtitle_ = new QLabel(this);
    lbl_subtitle_->setObjectName(QStringLiteral("UpdateBanner_Subtitle"));
    lbl_subtitle_->setText(QStringLiteral(
        "Tap View Details for release notes and to install."));
    text_col->addWidget(lbl_subtitle_);

    outer->addLayout(text_col, 1);

    btn_view_details_ = new QPushButton(QStringLiteral("View Details"), this);
    btn_view_details_->setObjectName(QStringLiteral("UpdateBanner_ViewDetails"));
    btn_view_details_->setCursor(Qt::PointingHandCursor);
    btn_view_details_->setFixedHeight(36);
    btn_view_details_->setMinimumWidth(116);
    connect(btn_view_details_, &QPushButton::clicked,
            this, &UpdateBanner::onViewDetailsClicked);
    outer->addWidget(btn_view_details_);
}

void UpdateBanner::applyStyle() {
    const UiThemeTokens t = uiThemeTokens(dark_mode_);

    // Card-style banner: subtle accent-tinted card_bg, 1px accent border,
    // matches project conventions instead of the Figma blue gradient.
    setStyleSheet(QStringLiteral(
        "QWidget#UpdateBanner {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 10px;"
        "}"
    ).arg(t.card_bg).arg(t.accent));

    icon_tile_->setStyleSheet(QStringLiteral(
        "QLabel#UpdateBanner_IconTile {"
        "  background-color: %1;"
        "  color: white;"
        "  border-radius: 8px;"
        "  font-size: 22px;"
        "  font-weight: 700;"
        "}"
    ).arg(t.accent));

    lbl_title_->setStyleSheet(QStringLiteral(
        "QLabel#UpdateBanner_Title {"
        "  color: %1;"
        "  font-size: 16px;"
        "  font-weight: 700;"
        "  background: transparent;"
        "  border: none;"
        "}"
    ).arg(t.text));

    lbl_version_pill_->setStyleSheet(QStringLiteral(
        "QLabel#UpdateBanner_VersionPill {"
        "  color: %1;"
        "  background-color: %2;"
        "  padding: 1px 8px;"
        "  border-radius: 9px;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "  border: none;"
        "}"
    ).arg(t.accent).arg(t.card_bg));

    lbl_subtitle_->setStyleSheet(QStringLiteral(
        "QLabel#UpdateBanner_Subtitle {"
        "  color: %1;"
        "  font-size: 13px;"
        "  background: transparent;"
        "  border: none;"
        "}"
    ).arg(t.muted));

    btn_view_details_->setStyleSheet(QStringLiteral(
        "QPushButton#UpdateBanner_ViewDetails {"
        "  background-color: %1;"
        "  color: white;"
        "  font-weight: 700;"
        "  font-size: 13px;"
        "  padding: 6px 14px;"
        "  border-radius: 8px;"
        "  border: none;"
        "}"
        "QPushButton#UpdateBanner_ViewDetails:hover {"
        "  background-color: %2;"
        "}"
        "QPushButton#UpdateBanner_ViewDetails:pressed {"
        "  background-color: %2;"
        "}"
    ).arg(t.accent).arg(t.accent_hover));
}

void UpdateBanner::setDarkMode(bool dark_mode) {
    dark_mode_ = dark_mode;
    applyStyle();
}

void UpdateBanner::setVersionInfo(const update::VersionInfo& info) {
    version_info_ = info;

    QString pill_text = info.tag.isEmpty() ? info.commitSha.left(7) : info.tag;
    if (pill_text.startsWith(QStringLiteral("v-"))) {
        pill_text = pill_text.mid(2);
    }
    lbl_version_pill_->setText(pill_text);

    QString subtitle = QStringLiteral(
        "Tap View Details for release notes and to install.");
    if (info.sizeBytes > 0) {
        subtitle = QStringLiteral(
            "Tap View Details for release notes and to install.  •  %1")
                       .arg(formatSizeMb(info.sizeBytes));
    }
    lbl_subtitle_->setText(subtitle);
}

QString UpdateBanner::formatSizeMb(qint64 bytes) const {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}

void UpdateBanner::onViewDetailsClicked() {
    emit viewDetailsRequested(version_info_);
}

}  // namespace f2c_cpp
