#include "setup_screen.hpp"
#include "dev_flags.hpp"
#include "robot_registry.hpp"
#include "settings_constants.hpp"
#include "ui_theme_constants.hpp"
#include "ui_zoom.hpp"
#include "update/update_log.hpp"

#include <algorithm>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace f2c_cpp {

namespace {

constexpr int kConnectionIconInset = 16;
constexpr int kFieldHeight = 56;
constexpr int kFieldSpacing = 12;
constexpr int kRootMargin = 32;
constexpr int kZoomButtonSize = 44;

QPixmap tintWireframeToColor(const QPixmap& src, const QColor& color) {
    // Treat white pixels as transparent; map darker pixels to the provided color.
    QImage img = src.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = QColor::fromRgba(line[x]);
            const int intensity = (c.red() + c.green() + c.blue()) / 3;
            // Avoid green haze from off-white backgrounds by thresholding the alpha.
            // (Wireframe sources often have slightly-gray paper/gradient backgrounds.)
            int alpha = 0;
            if (intensity < 245) {
                alpha = std::min(255, (245 - intensity) * 6);
            }
            line[x] = qRgba(color.red(), color.green(), color.blue(), alpha);
        }
    }
    return QPixmap::fromImage(img);
}

QString robotIpOverride() {
    // Release deletes the key at startup and must not honour it if a
    // dev build wrote it back while this process is running.
    if constexpr (!kDevMode) {
        return {};
    }
    const QSettings settings(kSettingsOrgName, kSettingsAppName);
    return settings.value(QStringLiteral("robot_ip"), QString()).toString().trimmed();
}

}  // namespace

SetupScreen::SetupScreen(QWidget* parent)
    : QWidget(parent) {
    setObjectName("SetupScreenRoot");

    // Root contains a top-left status + centered panel.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(kRootMargin, kRootMargin, kRootMargin, kRootMargin);
    root->setSpacing(0);

    // Top-left "CONNECTED" indicator (matches Figma)
    auto* status_row = new QHBoxLayout();
    status_row->setContentsMargins(0, 0, 0, 0);
    status_row->setSpacing(0);

    auto* status_col = new QVBoxLayout();
    status_col->setContentsMargins(0, 0, 0, 0);
    status_col->setSpacing(6);

    lbl_connection_icon_ = new QLabel(this);
    lbl_connection_icon_->setObjectName("SetupConnectedIcon");
    lbl_connection_icon_->setFixedSize(140, 80);
    lbl_connection_icon_->setAlignment(Qt::AlignCenter);
    const QPixmap src_wireframe(":/assets/roofus_wireframe.png");
    if (!src_wireframe.isNull()) {
        wireframe_connected_ = tintWireframeToColor(src_wireframe, QColor("#16a34a"));
        wireframe_disconnected_ = tintWireframeToColor(src_wireframe, QColor("#ff6b6b"));
        const QSize iconInset(kConnectionIconInset, kConnectionIconInset);
        lbl_connection_icon_->setPixmap(
            wireframe_connected_.scaled(lbl_connection_icon_->size() - iconInset,
                                        Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation));
    }
    status_col->addWidget(lbl_connection_icon_, 0, Qt::AlignLeft);

    lbl_connection_ = new QLabel("CONNECTED", this);
    lbl_connection_->setObjectName("SetupConnectedLabel");
    status_col->addWidget(lbl_connection_, 0, Qt::AlignLeft);

    status_row->addLayout(status_col);
    status_row->addStretch(1);

    // Text size, top right: A- / A+ zoom the whole app (ui_zoom.hpp). Qt
    // fixes the zoom at launch, so a change shows "Restart to apply".
    const QVariant active_prop = qApp->property(kUiZoomActiveProperty);
    active_zoom_ = active_prop.isValid() ? active_prop.toDouble() : kUiZoomDefault;
    zoom_index_ = 0;
    const double saved_zoom = savedUiZoom();
    for (int i = 0; i < static_cast<int>(kUiZoomLevels.size()); ++i) {
        if (qFuzzyCompare(kUiZoomLevels[static_cast<size_t>(i)].factor, saved_zoom)) {
            zoom_index_ = i;
        }
    }

    auto* zoom_col = new QVBoxLayout();
    // The window's min/max/close and theme toggle float over the top of
    // every stage; start below that band so the buttons never sit under them.
    zoom_col->setContentsMargins(0, kWindowTitleBarHeight - kRootMargin + 8, 0, 0);
    zoom_col->setSpacing(6);
    auto* zoom_row = new QHBoxLayout();
    zoom_row->setContentsMargins(0, 0, 0, 0);
    zoom_row->setSpacing(6);
    btn_zoom_out_ = new QPushButton(QStringLiteral("A\u2212"), this);
    btn_zoom_out_->setObjectName("SetupZoomOut");
    btn_zoom_out_->setToolTip(QStringLiteral("Smaller text"));
    btn_zoom_in_ = new QPushButton(QStringLiteral("A+"), this);
    btn_zoom_in_->setObjectName("SetupZoomIn");
    btn_zoom_in_->setToolTip(QStringLiteral("Larger text"));
    for (QPushButton* b : {btn_zoom_out_, btn_zoom_in_}) {
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(kZoomButtonSize, kZoomButtonSize);  // touch-sized
        zoom_row->addWidget(b);
    }
    connect(btn_zoom_out_, &QPushButton::clicked, this, [this]() { stepZoom(-1); });
    connect(btn_zoom_in_, &QPushButton::clicked, this, [this]() { stepZoom(+1); });
    zoom_col->addLayout(zoom_row);

    btn_restart_ = new QPushButton(QStringLiteral("Restart to apply"), this);
    btn_restart_->setObjectName("SetupRestartButton");
    btn_restart_->setCursor(Qt::PointingHandCursor);
    btn_restart_->setFixedHeight(32);
    connect(btn_restart_, &QPushButton::clicked, this, [this]() {
        update::log::info("setup", QStringLiteral("restart requested for text size"));
        emit restartRequested();
    });
    zoom_col->addWidget(btn_restart_, 0, Qt::AlignRight);
    zoom_col->addStretch(1);
    status_row->addLayout(zoom_col);
    refreshZoomControls();

    root->addLayout(status_row);

    root->addStretch(1);

    auto* panel = new QWidget(this);
    panel->setObjectName("SetupPanel");
    panel->setFixedWidth(672);

    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // Logo
    auto* logo = new QLabel(panel);
    logo->setObjectName("SetupLogo");
    logo->setAlignment(Qt::AlignHCenter);
    const QPixmap px(":/assets/bdr_wordmark.png");
    if (!px.isNull()) {
        logo->setPixmap(px.scaledToHeight(96, Qt::SmoothTransformation));
    } else {
        logo->setText("BDR");
    }
    logo->setFixedHeight(96);
    v->addWidget(logo, 0, Qt::AlignHCenter);
    v->addSpacing(16);

    // Subtitle
    auto* subtitle = new QLabel("COVERAGE PLANNING SUITE", panel);
    subtitle->setObjectName("SetupSubtitle");
    subtitle->setAlignment(Qt::AlignHCenter);
    v->addWidget(subtitle);

    v->addSpacing(48);

    // Which robot: paired / single entry -> fixed label, several -> picker.
    RobotRegistry registry;
    QString reg_err;
    QString pair_err;
    QStringList robot_ids;
    if (registry.load(&reg_err)) {
        robot_ids = registry.robotIds();
        paired_robot_id_ = registry.pairedRobotId(&pair_err);
        QHash<QString, QString> robot_by_token;
        for (const RobotProfile& profile : registry.robots()) {
            host_by_id_.insert(profile.robot_id, profile.host.trimmed());
            const QString token = profile.cloud_device_token;
            if (token.isEmpty()) {
                continue;
            }
            if (robot_by_token.contains(token)) {
                update::log::warn(
                    "setup",
                    QStringLiteral("robots %1 and %2 share a cloud_device_token; "
                                   "uploads from both land under one device")
                        .arg(robot_by_token.value(token), profile.robot_id));
            } else {
                robot_by_token.insert(token, profile.robot_id);
            }
        }
        update::log::info(
            "setup",
            QStringLiteral("registry loaded: %1 robot(s), paired=%2 (from %3)")
                .arg(robot_ids.size())
                .arg(paired_robot_id_.isEmpty() ? QStringLiteral("none")
                                                : paired_robot_id_)
                .arg(registry.sourcePath()));
    } else {
        update::log::warn("setup",
                          QStringLiteral("registry load failed: %1").arg(reg_err));
    }

    QString config_error;
    if (!reg_err.isEmpty()) {
        config_error = reg_err;
    } else if (!pair_err.isEmpty()) {
        config_error = pair_err;
    } else if (robot_ids.isEmpty()) {
        config_error = QStringLiteral("No robots are listed in %1.")
                           .arg(registry.sourcePath());
    }
    if (!pair_err.isEmpty()) {
        update::log::warn("setup", pair_err);
    }

    const bool pickable = config_error.isEmpty() && paired_robot_id_.isEmpty();

    auto* field_label = new QLabel(pickable ? QStringLiteral("ROBOT")
                                            : QStringLiteral("PAIRED ROBOT"),
                                   panel);
    field_label->setObjectName("SetupFieldLabel");
    v->addWidget(field_label);
    v->addSpacing(kFieldSpacing);

    if (pickable) {
        robot_picker_ = new QComboBox(panel);
        robot_picker_->setObjectName("SetupRobotPicker");
        robot_picker_->setFixedHeight(kFieldHeight);
        for (const QString& id : robot_ids) {
            robot_picker_->addItem(id.toUpper(), id);
        }
        const QSettings settings(kSettingsOrgName, kSettingsAppName);
        const int last = robot_picker_->findData(
            settings.value(kSettingsRobotIdKey, QString()).toString().trimmed());
        robot_picker_->setCurrentIndex(last >= 0 ? last : 0);
        connect(robot_picker_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &SetupScreen::checkConnection);
        v->addWidget(robot_picker_);
    } else {
        lbl_robot_value_ = new QLabel(
            paired_robot_id_.isEmpty() ? QStringLiteral("—") : paired_robot_id_.toUpper(),
            panel);
        lbl_robot_value_->setObjectName("SetupRobotValue");
        lbl_robot_value_->setFixedHeight(kFieldHeight);
        v->addWidget(lbl_robot_value_);
    }
    v->addSpacing(32);

    btn_start_ = new QPushButton("GET STARTED", panel);
    btn_start_->setObjectName("SetupStartButton");
    btn_start_->setFixedHeight(kFieldHeight);
    btn_start_->setEnabled(!selectedRobotId().isEmpty());
    v->addWidget(btn_start_);

    v->addSpacing(8);

    // Visible so a laptop never ships with the dev override still set: it
    // beats the pairing for this ping and for every robot SSH target.
    lbl_override_ = new QLabel(panel);
    lbl_override_->setObjectName("SetupNotice");
    lbl_override_->setWordWrap(true);
    lbl_override_->setVisible(false);
    v->addWidget(lbl_override_);

    lbl_error_ = new QLabel(config_error, panel);
    lbl_error_->setObjectName("SetupError");
    lbl_error_->setWordWrap(true);
    lbl_error_->setVisible(!config_error.isEmpty());
    v->addWidget(lbl_error_);

    root->addWidget(panel, 0, Qt::AlignHCenter);
    root->addStretch(2);

    applyLocalStyle();
    startConnectionMonitor();

    connect(btn_start_, &QPushButton::clicked, this, &SetupScreen::submit);
}

void SetupScreen::setDarkMode(bool dark_mode) {
    if (dark_mode_ == dark_mode) {
        return;
    }
    dark_mode_ = dark_mode;
    applyLocalStyle();
}

QString SetupScreen::selectedRobotId() const {
    if (robot_picker_) {
        return robot_picker_->currentData().toString();
    }
    return paired_robot_id_;
}

void SetupScreen::submit() {
    const QString robot_id = selectedRobotId();
    if (robot_id.isEmpty()) {
        return;
    }
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    settings.setValue(kSettingsRobotIdKey, robot_id);
    update::log::info("setup", QStringLiteral("get started: robot_id=%1").arg(robot_id));
    emit getStartedRequested(robot_id);
}

void SetupScreen::stepZoom(int delta) {
    const int last = static_cast<int>(kUiZoomLevels.size()) - 1;
    const int next = qBound(0, zoom_index_ + delta, last);
    if (next == zoom_index_) return;
    zoom_index_ = next;
    const double factor = kUiZoomLevels[static_cast<size_t>(next)].factor;
    saveUiZoom(factor);
    update::log::info("setup", QStringLiteral("text size set to %1").arg(factor));
    refreshZoomControls();
}

void SetupScreen::refreshZoomControls() {
    const int last = static_cast<int>(kUiZoomLevels.size()) - 1;
    if (btn_zoom_out_) btn_zoom_out_->setEnabled(zoom_index_ > 0);
    if (btn_zoom_in_) btn_zoom_in_->setEnabled(zoom_index_ < last);
    if (btn_restart_) {
        const double chosen = kUiZoomLevels[static_cast<size_t>(zoom_index_)].factor;
        btn_restart_->setVisible(!qFuzzyCompare(chosen, active_zoom_));
    }
}

void SetupScreen::startConnectionMonitor() {
    if (!ping_proc_) {
        ping_proc_ = new QProcess(this);
        connect(ping_proc_,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                &SetupScreen::onPingFinished);
    }
    if (!ping_timer_) {
        ping_timer_ = new QTimer(this);
        ping_timer_->setInterval(3000);
        connect(ping_timer_, &QTimer::timeout, this, &SetupScreen::checkConnection);
        ping_timer_->start();
    }
    updateConnectionUi(false);
    checkConnection();
}

void SetupScreen::checkConnection() {
    const QString override_ip = robotIpOverride();
    if (lbl_override_) {
        lbl_override_->setText(
            QStringLiteral("Dev override active: robot_ip=%1").arg(override_ip));
        lbl_override_->setVisible(!override_ip.isEmpty());
    }

    if (!ping_proc_ || ping_proc_->state() != QProcess::NotRunning) {
        return;
    }

    // No hardcoded fallback by design — if no host can be resolved, we leave
    // the indicator in DISCONNECTED rather than silently ping a random IP.
    const QString target =
        !override_ip.isEmpty() ? override_ip : host_by_id_.value(selectedRobotId());
    if (target.isEmpty()) {
        updateConnectionUi(false);
        return;
    }

    ping_proc_->setProgram("ping");
    ping_proc_->setArguments(QStringList() << "-c" << "1" << "-W" << "1" << target);
    ping_proc_->start();
}

void SetupScreen::onPingFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    const bool connected = (exitStatus == QProcess::NormalExit && exitCode == 0);
    updateConnectionUi(connected);
}

void SetupScreen::applyLocalStyle() {
    // Apply styling locally so we don't affect the existing planner theming.
    // (We intentionally do NOT touch qApp->setStyleSheet here.)
    const QString theme = dark_mode_ ? QStringLiteral("dark") : QStringLiteral("light");
    setProperty("theme", theme);
    setAttribute(Qt::WA_StyledBackground, true);

    // Palette fallback so root background is always correct (Qt can skip stylesheet background in some cases)
    QPalette pal = palette();
    pal.setColor(QPalette::Window, dark_mode_ ? QColor(0, 0, 0) : QColor(0xF9, 0xFA, 0xFB));
    setPalette(pal);
    setAutoFillBackground(true);

    setStyleSheet(R"(
        /* Dark theme: black background, green accents, white/light text */
        #SetupScreenRoot[theme="dark"] {
            background-color: #000000;
        }
        #SetupScreenRoot[theme="dark"] #SetupConnectedIcon {
            background-color: #000000;
        }

        #SetupScreenRoot[theme="light"] {
            background-color: #F9FAFB;
        }

        #SetupConnectedIcon {
            background-color: #000000;
            border: 1px solid rgba(22, 163, 74, 0.55);
            border-radius: 2px;
            padding: 6px;
        }
        #SetupConnectedLabel {
            color: #16a34a;
            font-size: 10px;
            font-weight: 700;
            letter-spacing: 1px;
        }
        #SetupConnectedLabel[conn="bad"] {
            color: #ef4444;
        }
        #SetupConnectedIcon[conn="bad"] {
            border: 1px solid rgba(239, 68, 68, 0.85);
        }
        #SetupScreenRoot[theme="light"] #SetupConnectedIcon {
            background-color: #FFFFFF;
            border: 1px solid #E5E7EB;
        }
        #SetupScreenRoot[theme="light"] #SetupConnectedIcon[conn="bad"] {
            border: 1px solid rgba(239, 68, 68, 0.75);
        }

        #SetupPanel {
            background: transparent;
        }

        #SetupSubtitle {
            color: #E5E7EB;
            font-size: 20px;
            letter-spacing: 3px;
        }

        #SetupFieldLabel {
            color: #16a34a;
            font-size: 18px;
            font-weight: 600;
            letter-spacing: 1px;
        }

        #SetupRobotValue, #SetupRobotPicker {
            background-color: transparent;
            border: 2px solid rgba(22, 163, 74, 0.4);
            border-radius: 2px;
            padding: 0 12px;
            color: #E5E7EB;
            font-size: 18px;
            font-weight: 600;
            letter-spacing: 1px;
        }
        #SetupRobotPicker:focus {
            border: 2px solid #16a34a;
        }
        #SetupRobotPicker::drop-down {
            border: none;
            width: 44px;
        }
        #SetupRobotPicker::down-arrow {
            image: url(:/assets/setup/chevron_down.svg);
            width: 20px;
            height: 20px;
        }
        #SetupRobotPicker QAbstractItemView {
            background-color: #18181B;
            color: #E5E7EB;
            selection-background-color: rgba(22, 163, 74, 0.35);
            font-size: 16px;
        }

        #SetupStartButton {
            background-color: #16a34a;
            border: 1px solid #16a34a;
            border-radius: 2px;
            color: #FFFFFF;
            font-size: 18px;
            font-weight: 700;
            letter-spacing: 1px;
        }
        #SetupStartButton:hover:enabled {
            background-color: #22c55e;
            border-color: #22c55e;
        }
        #SetupStartButton:disabled {
            background-color: rgba(22, 163, 74, 0.35);
            border: 1px solid rgba(22, 163, 74, 0.35);
            color: rgba(255, 255, 255, 0.5);
        }

        #SetupZoomOut, #SetupZoomIn {
            background-color: transparent;
            border: 2px solid rgba(22, 163, 74, 0.4);
            border-radius: 2px;
            color: #E5E7EB;
            font-weight: 700;
            padding: 0;
        }
        #SetupZoomOut { font-size: 14px; }
        #SetupZoomIn { font-size: 18px; }
        #SetupZoomOut:hover:enabled, #SetupZoomIn:hover:enabled {
            border: 2px solid #16a34a;
        }
        #SetupZoomOut:disabled, #SetupZoomIn:disabled {
            color: rgba(229, 231, 235, 0.3);
            border: 2px solid rgba(22, 163, 74, 0.15);
        }
        #SetupRestartButton {
            background-color: transparent;
            border: none;
            color: #FBBF24;
            font-size: 13px;
            font-weight: 600;
            text-decoration: underline;
            padding: 0;
        }

        #SetupNotice {
            color: #FBBF24;
            font-size: 12px;
            font-weight: 600;
        }
        #SetupError {
            color: #ef4444;
            font-size: 12px;
        }

        /* Light theme overrides */
        #SetupScreenRoot[theme="light"] #SetupConnectedLabel {
            color: #4A5565;
        }
        #SetupScreenRoot[theme="light"] #SetupConnectedLabel[conn="bad"] {
            color: #ef4444;
        }
        #SetupScreenRoot[theme="light"] #SetupSubtitle {
            color: #4A5565;
        }
        /* White on the brand green is 3.4:1. The fill keeps the brand hue
           and the label goes dark instead — the app-wide `on_accent` rule
           for light mode. */
        #SetupScreenRoot[theme="light"] #SetupStartButton {
            color: #18181B;
        }
        /* Disabled reads as unfilled rather than as faded-white-on-faded-
           green, which measured 1.2:1 and left the operator unable to tell
           what the button said. */
        #SetupScreenRoot[theme="light"] #SetupStartButton:disabled {
            background-color: #DCFCE7;
            border: 1px solid #A7E8BF;
            color: #166534;
        }
        #SetupScreenRoot[theme="light"] #SetupRobotValue,
        #SetupScreenRoot[theme="light"] #SetupRobotPicker {
            color: #1F2937;
        }
        #SetupScreenRoot[theme="light"] #SetupRobotPicker QAbstractItemView {
            background-color: #FFFFFF;
            color: #1F2937;
        }
        #SetupScreenRoot[theme="light"] #SetupZoomOut,
        #SetupScreenRoot[theme="light"] #SetupZoomIn {
            color: #1F2937;
        }
        #SetupScreenRoot[theme="light"] #SetupZoomOut:disabled,
        #SetupScreenRoot[theme="light"] #SetupZoomIn:disabled {
            color: rgba(31, 41, 55, 0.3);
        }
        #SetupScreenRoot[theme="light"] #SetupRestartButton {
            color: #92400E;
        }
        #SetupScreenRoot[theme="light"] #SetupNotice {
            color: #92400E;
        }
        #SetupScreenRoot[theme="light"] #SetupError {
            color: #B91C1C;
        }
    )");
    // The [theme] rules match on this widget's property, which Qt only
    // re-reads for a child when that child is repolished.
    style()->unpolish(this);
    style()->polish(this);
    for (QWidget* w : findChildren<QWidget*>()) {
        w->style()->unpolish(w);
        w->style()->polish(w);
    }
    update();
}

void SetupScreen::updateConnectionUi(bool connected) {
    const bool state_changed =
        !last_connection_state_valid_ || last_connection_state_ != connected;
    if (!state_changed) {
        return;
    }
    last_connection_state_valid_ = true;
    last_connection_state_ = connected;

    if (lbl_connection_) {
        lbl_connection_->setText(connected ? "CONNECTED" : "DISCONNECTED");
        lbl_connection_->setProperty("conn", connected ? "ok" : "bad");
        lbl_connection_->style()->unpolish(lbl_connection_);
        lbl_connection_->style()->polish(lbl_connection_);
        lbl_connection_->update();
    }
    if (lbl_connection_icon_) {
        lbl_connection_icon_->setProperty("conn", connected ? "ok" : "bad");
        lbl_connection_icon_->style()->unpolish(lbl_connection_icon_);
        lbl_connection_icon_->style()->polish(lbl_connection_icon_);
        lbl_connection_icon_->update();

        if (!wireframe_connected_.isNull()) {
            const QSize iconInset(kConnectionIconInset, kConnectionIconInset);
            const QPixmap& px = connected ? wireframe_connected_ : wireframe_disconnected_;
            lbl_connection_icon_->setPixmap(
                px.scaled(lbl_connection_icon_->size() - iconInset,
                          Qt::KeepAspectRatio,
                          Qt::SmoothTransformation));
        }
    }
}

}  // namespace f2c_cpp
