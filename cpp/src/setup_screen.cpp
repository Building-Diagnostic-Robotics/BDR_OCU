#include "setup_screen.hpp"
#include "robot_login.hpp"
#include "robot_registry.hpp"
#include "settings_constants.hpp"

#include <algorithm>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QImage>
#include <QPixmap>
#include <QProcess>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace f2c_cpp {

namespace {

constexpr int kConnectionIconInset = 16;
constexpr int kFieldHeight = 56;
constexpr int kFieldSpacing = 12;

QString trimmed(const QString& s) {
    return s.trimmed();
}

/** Adds a labeled line-edit row to parentLayout; returns the QLineEdit for the caller to store. */
QLineEdit* addLabeledField(QWidget* panel, QVBoxLayout* parentLayout,
                          const QString& labelText, const QString& placeholder,
                          bool isPassword, const QString& initialText) {
    auto* group = new QVBoxLayout();
    group->setContentsMargins(0, 0, 0, 0);
    group->setSpacing(kFieldSpacing);

    auto* lbl = new QLabel(labelText, panel);
    lbl->setObjectName("SetupFieldLabel");
    group->addWidget(lbl);

    auto* edit = new QLineEdit(panel);
    edit->setObjectName("SetupLineEdit");
    edit->setPlaceholderText(placeholder);
    edit->setText(initialText);
    if (isPassword) {
        edit->setEchoMode(QLineEdit::Password);
    }
    edit->setClearButtonEnabled(false);
    edit->setFixedHeight(kFieldHeight);
    group->addWidget(edit);

    parentLayout->addLayout(group);
    return edit;
}

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

}  // namespace

SetupScreen::SetupScreen(QWidget* parent)
    : QWidget(parent) {
    setObjectName("SetupScreenRoot");

    // Root contains a top-left status + centered login panel.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(32, 32, 32, 32);
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

    // Load persisted Robot ID (Access Code is intentionally never persisted).
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    const QString saved_robot_id = settings.value("setup/robot_id", "").toString();

    // Robot ID
    txt_robot_id_ = addLabeledField(panel, v, "ROOFUS(Robot) ID", "Enter Robot ID",
                                    false, saved_robot_id);
    v->addSpacing(32);

    // Access Code (with VIEW/HIDE toggle and 6-digit PIN validation – legacy behaviour)
    auto* code_group = new QVBoxLayout();
    code_group->setContentsMargins(0, 0, 0, 0);
    code_group->setSpacing(kFieldSpacing);
    auto* lbl_code = new QLabel("ACCESS CODE", panel);
    lbl_code->setObjectName("SetupFieldLabel");
    code_group->addWidget(lbl_code);
    auto* code_row = new QHBoxLayout();
    code_row->setContentsMargins(0, 0, 0, 0);
    code_row->setSpacing(10);
    txt_access_code_ = new QLineEdit(panel);
    txt_access_code_->setObjectName("SetupLineEdit");
    txt_access_code_->setPlaceholderText("Enter Access Code");
    txt_access_code_->setEchoMode(QLineEdit::Password);
    txt_access_code_->setClearButtonEnabled(false);
    txt_access_code_->setFixedHeight(kFieldHeight);
    code_row->addWidget(txt_access_code_, 1);
    btn_view_code_ = new QToolButton(panel);
    btn_view_code_->setObjectName("SetupViewButton");
    btn_view_code_->setText("VIEW");
    btn_view_code_->setCheckable(true);
    btn_view_code_->setFocusPolicy(Qt::NoFocus);
    btn_view_code_->setToolTip("Show/hide access code");
    btn_view_code_->setFixedHeight(kFieldHeight);
    code_row->addWidget(btn_view_code_, 0, Qt::AlignVCenter);
    code_group->addLayout(code_row);
    v->addLayout(code_group);
    v->addSpacing(32);

    // Full-width LOGIN button
    btn_login_ = new QPushButton("LOGIN", panel);
    btn_login_->setObjectName("SetupLoginButton");
    btn_login_->setEnabled(false);
    btn_login_->setFixedHeight(kFieldHeight);
    v->addWidget(btn_login_);

    v->addSpacing(8);

    // Error label (hidden unless needed)
    lbl_error_ = new QLabel(panel);
    lbl_error_->setObjectName("SetupError");
    lbl_error_->setVisible(false);
    lbl_error_->setWordWrap(true);
    v->addWidget(lbl_error_);

    root->addWidget(panel, 0, Qt::AlignHCenter);
    root->addStretch(2);

    applyLocalStyle();
    startConnectionMonitor();

    // Wiring
    connect(txt_robot_id_, &QLineEdit::textChanged, this, &SetupScreen::updateUiState);
    connect(txt_access_code_, &QLineEdit::textChanged, this, &SetupScreen::updateUiState);
    connect(btn_view_code_, &QToolButton::clicked, this, &SetupScreen::toggleAccessCodeVisible);
    connect(btn_login_, &QPushButton::clicked, this, &SetupScreen::submit);

    connect(txt_access_code_, &QLineEdit::returnPressed, this, &SetupScreen::submit);
    connect(txt_robot_id_, &QLineEdit::returnPressed, this, &SetupScreen::submit);

    updateUiState();
}

void SetupScreen::setDarkMode(bool dark_mode) {
    if (dark_mode_ == dark_mode) {
        return;
    }
    dark_mode_ = dark_mode;
    applyLocalStyle();
}

void SetupScreen::toggleAccessCodeVisible() {
    const bool visible = btn_view_code_ && btn_view_code_->isChecked();
    if (txt_access_code_) {
        txt_access_code_->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    }
    if (btn_view_code_) {
        btn_view_code_->setText(visible ? "HIDE" : "VIEW");
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
    if (!ping_proc_ || ping_proc_->state() != QProcess::NotRunning) {
        return;
    }

    QSettings settings(kSettingsOrgName, kSettingsAppName);
    const QString robot_ip = settings.value("robot_ip", "").toString().trimmed();
    const QString target = robot_ip.isEmpty() ? "192.168.168.101" : robot_ip;

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

        #SetupLineEdit {
            background-color: transparent;
            border: 2px solid rgba(22, 163, 74, 0.4);
            border-radius: 2px;
            padding: 0 12px;
            color: #E5E7EB;
            font-size: 18px;
        }
        #SetupLineEdit::placeholder {
            color: #9CA3AF;
        }
        #SetupLineEdit:focus {
            border: 2px solid #16a34a;
        }

        #SetupViewButton {
            background-color: transparent;
            border: 2px solid rgba(22, 163, 74, 0.55);
            border-radius: 2px;
            padding: 0 12px;
            color: #16a34a;
            font-size: 14px;
            font-weight: 700;
            letter-spacing: 1px;
        }
        #SetupViewButton:checked {
            background-color: rgba(22, 163, 74, 0.18);
            border: 2px solid #16a34a;
        }

        #SetupLoginButton {
            background-color: #16a34a;
            border: 1px solid #16a34a;
            border-radius: 2px;
            color: #FFFFFF;
            font-size: 18px;
            font-weight: 700;
            letter-spacing: 1px;
        }
        #SetupLoginButton:hover:enabled {
            background-color: #22c55e;
            border-color: #22c55e;
        }
        #SetupLoginButton:disabled {
            background-color: rgba(22, 163, 74, 0.35);
            border: 1px solid rgba(22, 163, 74, 0.35);
            color: rgba(255, 255, 255, 0.5);
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
            color: #6B7280;
        }
        #SetupScreenRoot[theme="light"] #SetupLineEdit {
            color: #1F2937;
        }
        #SetupScreenRoot[theme="light"] #SetupLineEdit::placeholder {
            color: #6B7280;
        }
        #SetupScreenRoot[theme="light"] #SetupViewButton {
            border-color: rgba(22, 163, 74, 0.6);
            color: #16a34a;
        }
        #SetupScreenRoot[theme="light"] #SetupViewButton:checked {
            background-color: rgba(22, 163, 74, 0.12);
            border-color: #16a34a;
        }
    )");
    style()->unpolish(this);
    style()->polish(this);
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

std::pair<QString, QString> SetupScreen::getTrimmedCredentials() const {
    const QString robot_id = trimmed(txt_robot_id_ ? txt_robot_id_->text() : QString());
    const QString access_code = trimmed(txt_access_code_ ? txt_access_code_->text() : QString());
    return {robot_id, access_code};
}

void SetupScreen::updateUiState() {
    const auto [robot_id, access_code] = getTrimmedCredentials();
    const bool ready = !robot_id.isEmpty() && !access_code.isEmpty();
    if (btn_login_) {
        btn_login_->setEnabled(ready);
    }
    if (lbl_error_) {
        lbl_error_->setVisible(false);
        lbl_error_->clear();
    }
}

void SetupScreen::submit() {
    const auto [robot_id, access_code] = getTrimmedCredentials();

    if (robot_id.isEmpty() || access_code.isEmpty()) {
        if (lbl_error_) {
            lbl_error_->setText("Please enter both Robot ID and Access Code.");
            lbl_error_->setVisible(true);
        }
        return;
    }

    // Dev bypass: just proceed to next screen (no SSH/auth). Re-enable real login later.
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    settings.setValue("setup/robot_id", robot_id);

    emit loginSubmitted(robot_id, access_code);
}

}  // namespace f2c_cpp

