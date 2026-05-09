/**
 * @file update_modal.cpp
 * @brief Implementation of the OTA "What's New" modal.
 */

#include "components/update_modal.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QStringList>
#include <QStyle>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include "ui_theme_constants.hpp"
#include "update/update_log.hpp"

namespace f2c_cpp {

namespace {

// 5 s polling cadence while the modal is visible. Cheap (a single sysfs
// read) and gives the operator near-immediate feedback when AC is plugged
// in to clear the low-battery gate.
constexpr int kBatteryPollIntervalMs = 5000;

// Locked spec phase 4 Q3: block Install Now under 20 % battery.
constexpr int kBatteryGateThresholdPct = 20;

// Cap on parsed release-note bullets. Anything longer than this and the
// modal stops feeling minimal. Operators can read full notes on the
// GitHub release page if they really want to.
constexpr int kMaxReleaseNoteBullets = 5;

}  // namespace

UpdateModal::UpdateModal(const update::VersionInfo& info,
                         const GateState& gate_state,
                         bool dark_mode,
                         QWidget* parent)
    : QDialog(parent),
      info_(info),
      gate_(gate_state),
      dark_mode_(dark_mode) {
    setObjectName(QStringLiteral("UpdateModal"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowModality(Qt::ApplicationModal);
    setMinimumWidth(560);

    battery_poll_timer_ = new QTimer(this);
    battery_poll_timer_->setInterval(kBatteryPollIntervalMs);
    connect(battery_poll_timer_, &QTimer::timeout,
            this, &UpdateModal::onPollBattery);

    buildUi();
    applyStyle();
    populateContent();
    refreshInstallButton();
}

void UpdateModal::buildUi() {
    auto* container = new QWidget(this);
    container->setObjectName(QStringLiteral("UpdateModal_Container"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(container);

    auto* root = new QVBoxLayout(container);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);

    // Header row: icon tile + title column + close button.
    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(12);

    lbl_icon_tile_ = new QLabel(QStringLiteral("\u21A7"), container);
    lbl_icon_tile_->setObjectName(QStringLiteral("UpdateModal_IconTile"));
    lbl_icon_tile_->setAlignment(Qt::AlignCenter);
    lbl_icon_tile_->setFixedSize(40, 40);
    header->addWidget(lbl_icon_tile_, 0, Qt::AlignTop);

    auto* title_col = new QVBoxLayout();
    title_col->setContentsMargins(0, 0, 0, 0);
    title_col->setSpacing(2);

    lbl_title_ = new QLabel(QStringLiteral("System Update"), container);
    lbl_title_->setObjectName(QStringLiteral("UpdateModal_Title"));
    title_col->addWidget(lbl_title_);

    lbl_subtitle_ = new QLabel(container);
    lbl_subtitle_->setObjectName(QStringLiteral("UpdateModal_Subtitle"));
    title_col->addWidget(lbl_subtitle_);

    header->addLayout(title_col, 1);

    btn_close_ = new QPushButton(QStringLiteral("\u00D7"), container);
    btn_close_->setObjectName(QStringLiteral("UpdateModal_Close"));
    btn_close_->setFlat(true);
    btn_close_->setCursor(Qt::PointingHandCursor);
    btn_close_->setFixedSize(32, 32);
    // Locked spec Q1=B: X is close-only, no snooze. Just dismiss the dialog;
    // banner stays visible so the operator can reopen.
    connect(btn_close_, &QPushButton::clicked, this, &QDialog::reject);
    header->addWidget(btn_close_, 0, Qt::AlignTop);

    root->addLayout(header);

    // What's New card.
    auto* whats_new = new QWidget(container);
    whats_new->setObjectName(QStringLiteral("UpdateModal_WhatsNew"));
    auto* wn_lay = new QVBoxLayout(whats_new);
    wn_lay->setContentsMargins(16, 12, 16, 12);
    wn_lay->setSpacing(8);

    lbl_whats_new_title_ = new QLabel(QStringLiteral("What's New"), whats_new);
    lbl_whats_new_title_->setObjectName(
        QStringLiteral("UpdateModal_WhatsNewTitle"));
    wn_lay->addWidget(lbl_whats_new_title_);

    whats_new_list_layout_ = new QVBoxLayout();
    whats_new_list_layout_->setContentsMargins(0, 0, 0, 0);
    whats_new_list_layout_->setSpacing(4);
    wn_lay->addLayout(whats_new_list_layout_);

    root->addWidget(whats_new);

    // Warning callout (amber).
    lbl_warning_ = new QLabel(
        QStringLiteral(
            "\u26A0  Installation will close the application and may take "
            "a few minutes. Save your work before continuing."),
        container);
    lbl_warning_->setObjectName(QStringLiteral("UpdateModal_Warning"));
    lbl_warning_->setWordWrap(true);
    root->addWidget(lbl_warning_);

    // Stat row: download size + battery readout.
    auto* stat_row = new QHBoxLayout();
    stat_row->setContentsMargins(0, 0, 0, 0);
    stat_row->setSpacing(20);

    auto* size_col = new QVBoxLayout();
    size_col->setContentsMargins(0, 0, 0, 0);
    size_col->setSpacing(2);
    auto* size_label_caption = new QLabel(
        QStringLiteral("Download Size"), container);
    size_label_caption->setObjectName(
        QStringLiteral("UpdateModal_StatCaption"));
    size_col->addWidget(size_label_caption);
    lbl_size_value_ = new QLabel(container);
    lbl_size_value_->setObjectName(QStringLiteral("UpdateModal_StatValue"));
    size_col->addWidget(lbl_size_value_);
    stat_row->addLayout(size_col);

    auto* batt_col = new QVBoxLayout();
    batt_col->setContentsMargins(0, 0, 0, 0);
    batt_col->setSpacing(2);
    auto* batt_label_caption = new QLabel(
        QStringLiteral("Battery"), container);
    batt_label_caption->setObjectName(
        QStringLiteral("UpdateModal_StatCaption"));
    batt_col->addWidget(batt_label_caption);
    lbl_battery_value_ = new QLabel(container);
    lbl_battery_value_->setObjectName(
        QStringLiteral("UpdateModal_StatValue"));
    batt_col->addWidget(lbl_battery_value_);
    stat_row->addLayout(batt_col);

    stat_row->addStretch();
    root->addLayout(stat_row);

    // Footer: Remind Me Later (text-button) + Install Now (accent).
    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 4, 0, 0);
    footer->setSpacing(12);
    footer->addStretch();

    btn_remind_later_ = new QPushButton(
        QStringLiteral("Remind Me Later"), container);
    btn_remind_later_->setObjectName(
        QStringLiteral("UpdateModal_RemindLater"));
    btn_remind_later_->setCursor(Qt::PointingHandCursor);
    btn_remind_later_->setFixedHeight(36);
    btn_remind_later_->setMinimumWidth(140);
    connect(btn_remind_later_, &QPushButton::clicked,
            this, &UpdateModal::onRemindMeLaterClicked);
    footer->addWidget(btn_remind_later_);

    btn_install_now_ = new QPushButton(
        QStringLiteral("Install Now"), container);
    btn_install_now_->setObjectName(
        QStringLiteral("UpdateModal_InstallNow"));
    btn_install_now_->setCursor(Qt::PointingHandCursor);
    btn_install_now_->setFixedHeight(36);
    btn_install_now_->setMinimumWidth(140);
    connect(btn_install_now_, &QPushButton::clicked,
            this, &UpdateModal::onInstallClicked);
    footer->addWidget(btn_install_now_);

    root->addLayout(footer);
}

void UpdateModal::applyStyle() {
    const UiThemeTokens t = uiThemeTokens(dark_mode_);

    setStyleSheet(QStringLiteral(
        "QWidget#UpdateModal_Container {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 12px;"
        "}"
        "QLabel#UpdateModal_IconTile {"
        "  background-color: %3;"
        "  color: white;"
        "  border-radius: 8px;"
        "  font-size: 22px;"
        "  font-weight: 700;"
        "}"
        "QLabel#UpdateModal_Title {"
        "  color: %4;"
        "  font-size: 18px;"
        "  font-weight: 700;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel#UpdateModal_Subtitle {"
        "  color: %5;"
        "  font-size: 13px;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QPushButton#UpdateModal_Close {"
        "  color: %5;"
        "  background: transparent;"
        "  border: none;"
        "  font-size: 22px;"
        "  font-weight: 600;"
        "}"
        "QPushButton#UpdateModal_Close:hover {"
        "  color: %4;"
        "  background-color: %6;"
        "  border-radius: 6px;"
        "}"
        "QWidget#UpdateModal_WhatsNew {"
        "  background-color: %6;"
        "  border: 1px solid %2;"
        "  border-radius: 8px;"
        "}"
        "QLabel#UpdateModal_WhatsNewTitle {"
        "  color: %4;"
        "  font-size: 13px;"
        "  font-weight: 700;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel#UpdateModal_WhatsNewBullet {"
        "  color: %4;"
        "  font-size: 13px;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel#UpdateModal_WhatsNewEmpty {"
        "  color: %5;"
        "  font-size: 13px;"
        "  font-style: italic;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel#UpdateModal_Warning {"
        "  color: %7;"
        "  background-color: rgba(245, 158, 11, 0.12);"
        "  border: 1px solid %7;"
        "  border-radius: 8px;"
        "  padding: 10px 12px;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        "QLabel#UpdateModal_StatCaption {"
        "  color: %5;"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "  text-transform: uppercase;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel#UpdateModal_StatValue {"
        "  color: %4;"
        "  font-size: 14px;"
        "  font-weight: 600;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel#UpdateModal_StatValueWarn {"
        "  color: %7;"
        "  font-size: 14px;"
        "  font-weight: 700;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QPushButton#UpdateModal_RemindLater {"
        "  color: %5;"
        "  background-color: transparent;"
        "  border: 1px solid %2;"
        "  border-radius: 8px;"
        "  font-size: 13px;"
        "  font-weight: 600;"
        "  padding: 0px 14px;"
        "}"
        "QPushButton#UpdateModal_RemindLater:hover {"
        "  color: %4;"
        "  background-color: %6;"
        "}"
        "QPushButton#UpdateModal_InstallNow {"
        "  color: white;"
        "  background-color: %3;"
        "  border: none;"
        "  border-radius: 8px;"
        "  font-size: 13px;"
        "  font-weight: 700;"
        "  padding: 0px 16px;"
        "}"
        "QPushButton#UpdateModal_InstallNow:hover {"
        "  background-color: %8;"
        "}"
        "QPushButton#UpdateModal_InstallNow:disabled {"
        "  background-color: %2;"
        "  color: %5;"
        "}"
    )
        .arg(t.card_bg)        // %1 container bg
        .arg(t.border)          // %2 border
        .arg(t.accent)          // %3 accent (icon tile + Install)
        .arg(t.text)            // %4 primary text
        .arg(t.muted)           // %5 muted text
        .arg(t.bg)              // %6 nested card / hover bg
        .arg(t.warning)         // %7 amber warning
        .arg(t.accent_hover));  // %8 install hover
}

void UpdateModal::populateContent() {
    // Subtitle: version pill text + localized release date when available.
    QString version_text = info_.tag.isEmpty() ? info_.commitSha.left(7)
                                                : info_.tag;
    if (version_text.startsWith(QStringLiteral("v-"))) {
        version_text = version_text.mid(2);
    }
    const QString date_text = formatPublishedDate(info_.publishedAtIso8601);
    QString subtitle = QStringLiteral("Version %1").arg(version_text);
    if (!date_text.isEmpty()) {
        subtitle += QStringLiteral("  \u2022  Released %1").arg(date_text);
    }
    lbl_subtitle_->setText(subtitle);

    // Bullet list. Clear any prior children (defensive — populateContent is
    // currently called once from the ctor, but the layout indirection makes
    // it cheap to re-run if we ever want to refresh on the fly).
    while (QLayoutItem* item = whats_new_list_layout_->takeAt(0)) {
        if (auto* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }
    const QStringList bullets = parseReleaseNotesBullets(
        info_.releaseNotes, kMaxReleaseNoteBullets);
    if (bullets.isEmpty()) {
        auto* empty = new QLabel(
            QStringLiteral("No release notes provided."), this);
        empty->setObjectName(QStringLiteral("UpdateModal_WhatsNewEmpty"));
        empty->setWordWrap(true);
        whats_new_list_layout_->addWidget(empty);
    } else {
        for (const QString& line : bullets) {
            auto* row = new QLabel(QStringLiteral("\u2022  %1").arg(line),
                                   this);
            row->setObjectName(
                QStringLiteral("UpdateModal_WhatsNewBullet"));
            row->setWordWrap(true);
            whats_new_list_layout_->addWidget(row);
        }
    }

    // Download size readout.
    if (info_.sizeBytes > 0) {
        lbl_size_value_->setText(formatSizeMb(info_.sizeBytes));
    } else {
        lbl_size_value_->setText(QStringLiteral("—"));
    }
}

void UpdateModal::refreshInstallButton() {
    // Battery readout & gate decision. Locked spec Q2=C: -1 means "no
    // BAT* node present" → AC, allow install, render literal "AC".
    QString battery_text;
    bool low_batt = false;
    if (gate_.battery_pct < 0) {
        battery_text = QStringLiteral("AC");
    } else {
        battery_text = QStringLiteral("%1%").arg(gate_.battery_pct);
        if (gate_.battery_pct < kBatteryGateThresholdPct) {
            low_batt = true;
            battery_text = QStringLiteral("%1%  (low)")
                               .arg(gate_.battery_pct);
        }
    }
    lbl_battery_value_->setText(battery_text);
    lbl_battery_value_->setObjectName(
        low_batt ? QStringLiteral("UpdateModal_StatValueWarn")
                 : QStringLiteral("UpdateModal_StatValue"));
    // Re-apply the stylesheet so the renamed objectName picks up the new
    // selector. Qt does not re-evaluate parent stylesheet matches on
    // setObjectName by itself.
    lbl_battery_value_->style()->unpolish(lbl_battery_value_);
    lbl_battery_value_->style()->polish(lbl_battery_value_);

    // Compose the gate decision and tooltip.
    QStringList reasons;
    if (gate_.has_active_mission) {
        reasons << QStringLiteral("a mission is in progress");
    }
    if (gate_.has_active_transfer) {
        reasons << QStringLiteral("a data transfer is running");
    }
    if (gate_.has_active_upload) {
        reasons << QStringLiteral("a cloud upload is running");
    }
    if (low_batt) {
        reasons << QStringLiteral(
            "battery is below %1%").arg(kBatteryGateThresholdPct);
    }

    const bool blocked = !reasons.isEmpty();
    btn_install_now_->setEnabled(!blocked);
    if (blocked) {
        btn_install_now_->setToolTip(
            QStringLiteral("Cannot install now — %1.")
                .arg(reasons.join(QStringLiteral(", "))));
    } else {
        btn_install_now_->setToolTip(QString());
    }
}

void UpdateModal::setGateState(const GateState& gate_state) {
    gate_ = gate_state;
    refreshInstallButton();
}

void UpdateModal::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // Center on parent so the modal lands over whatever stage is active.
    if (QWidget* p = parentWidget()) {
        const QRect parent_geo = p->geometry();
        const QPoint center = parent_geo.center();
        move(center.x() - width() / 2, center.y() - height() / 2);
    }
    if (battery_poll_timer_ && !battery_poll_timer_->isActive()) {
        battery_poll_timer_->start();
    }
    // Pull a fresh battery reading on open so the operator never sees a
    // stale value carried over from the AppShell-computed snapshot.
    onPollBattery();
}

void UpdateModal::hideEvent(QHideEvent* event) {
    if (battery_poll_timer_ && battery_poll_timer_->isActive()) {
        battery_poll_timer_->stop();
    }
    QDialog::hideEvent(event);
}

void UpdateModal::onPollBattery() {
    const int pct = readBatteryPercent();
    if (pct == gate_.battery_pct) {
        return;
    }
    gate_.battery_pct = pct;
    refreshInstallButton();
}

void UpdateModal::onInstallClicked() {
    // Defense-in-depth — refreshInstallButton already disables the button
    // when the gate is set, but a stale click queued before the timer
    // re-evaluated could land here. Treat it as a no-op rather than firing
    // installRequested with a dirty gate.
    if (!btn_install_now_->isEnabled()) {
        return;
    }
    update::log::info(
        "modal",
        QStringLiteral("install requested: tag=%1 sha=%2")
            .arg(info_.tag).arg(info_.commitSha));
    // Locked spec Q3=A: signal-only, modal stays open. AppShell will log
    // and (in phase 7) wire the actual installer takeover.
    emit installRequested(info_);
}

void UpdateModal::onRemindMeLaterClicked() {
    update::log::info(
        "modal",
        QStringLiteral("remind me later: tag=%1 sha=%2")
            .arg(info_.tag).arg(info_.commitSha));
    emit remindMeLaterRequested(info_);
    accept();  // close the dialog; AppShell handles the snooze + banner hide.
}

QStringList UpdateModal::parseReleaseNotesBullets(const QString& body,
                                                  int max_bullets) {
    QStringList out;
    if (body.isEmpty() || max_bullets <= 0) {
        return out;
    }
    // GitHub release bodies are markdown. We only honor "- " or "* "
    // prefixed lines (with optional leading whitespace) — matches the
    // CHANGELOG format documented in CHANGELOG.md and keeps parsing dumb
    // and predictable on the device.
    const QStringList lines = body.split(
        QRegularExpression(QStringLiteral("\r?\n")));
    for (const QString& raw : lines) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty()) continue;
        QString item;
        if (trimmed.startsWith(QStringLiteral("- ")) ||
            trimmed.startsWith(QStringLiteral("* "))) {
            item = trimmed.mid(2).trimmed();
        } else if (trimmed == QStringLiteral("-") ||
                   trimmed == QStringLiteral("*")) {
            continue;
        } else {
            continue;
        }
        if (item.isEmpty()) continue;
        out.append(item);
        if (out.size() >= max_bullets) break;
    }
    return out;
}

QString UpdateModal::formatSizeMb(qint64 bytes) {
    if (bytes <= 0) return QStringLiteral("—");
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}

QString UpdateModal::formatPublishedDate(const QString& iso8601) {
    if (iso8601.isEmpty()) return QString();
    QDateTime dt = QDateTime::fromString(iso8601, Qt::ISODate);
    if (!dt.isValid()) {
        // GitHub's published_at is always RFC 3339 with the trailing 'Z'.
        // Qt::ISODate handles that, but fall back to ISODateWithMs just
        // in case a future release format adds milliseconds.
        dt = QDateTime::fromString(iso8601, Qt::ISODateWithMs);
    }
    if (!dt.isValid()) return QString();
    dt = dt.toLocalTime();
    // QLocale::LongFormat → e.g. "May 8, 2026" on en_US.
    return QLocale().toString(dt.date(), QLocale::LongFormat);
}

int UpdateModal::readBatteryPercent() {
    QDir psu(QStringLiteral("/sys/class/power_supply"));
    if (!psu.exists()) return -1;
    const QStringList entries = psu.entryList(
        QStringList() << QStringLiteral("BAT*"),
        QDir::Dirs | QDir::NoDotAndDotDot);
    if (entries.isEmpty()) return -1;
    // Prefer the first BAT* node. Multi-battery laptops are rare in this
    // fleet; if one ever shows up, the OCU still behaves correctly because
    // any BAT below threshold is enough to cancel install.
    for (const QString& name : entries) {
        QFile cap(psu.absoluteFilePath(name) +
                  QStringLiteral("/capacity"));
        if (!cap.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QTextStream ts(&cap);
        bool ok = false;
        const int pct = ts.readLine().trimmed().toInt(&ok);
        cap.close();
        if (ok && pct >= 0 && pct <= 100) {
            return pct;
        }
    }
    return -1;
}

}  // namespace f2c_cpp
