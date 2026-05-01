#include "setup_screen.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace f2c_cpp {

namespace {

constexpr const char* kOrgName = "PilotControl";
constexpr const char* kAppName = "BDRCoveragePlanner";

QString trimmed(const QString& s) {
    return s.trimmed();
}

}  // namespace

SetupScreen::SetupScreen(QWidget* parent)
    : QWidget(parent) {
    setObjectName("SetupScreenRoot");

    // Root centers the login panel.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addStretch(1);

    auto* panel = new QWidget(this);
    panel->setObjectName("SetupPanel");
    panel->setFixedWidth(520);

    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(14);

    // Logo
    auto* logo = new QLabel(panel);
    logo->setObjectName("SetupLogo");
    logo->setAlignment(Qt::AlignHCenter);
    const QPixmap px(":/assets/bdr_logo.png");
    if (!px.isNull()) {
        logo->setPixmap(px.scaled(220, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        logo->setText("BDR");
    }
    v->addWidget(logo, 0, Qt::AlignHCenter);

    // Subtitle
    auto* subtitle = new QLabel("COVERAGE PLANNING SUITE", panel);
    subtitle->setObjectName("SetupSubtitle");
    subtitle->setAlignment(Qt::AlignHCenter);
    v->addWidget(subtitle);

    v->addSpacing(18);

    // Load persisted Robot ID (Access Code is intentionally never persisted).
    QSettings settings(kOrgName, kAppName);
    const QString saved_robot_id = settings.value("setup/robot_id", "").toString();

    // Robot ID
    auto* lbl_robot = new QLabel("ROOFUS (ROBOT) ID", panel);
    lbl_robot->setObjectName("SetupFieldLabel");
    v->addWidget(lbl_robot);

    txt_robot_id_ = new QLineEdit(panel);
    txt_robot_id_->setObjectName("SetupLineEdit");
    txt_robot_id_->setPlaceholderText("Enter Robot ID");
    txt_robot_id_->setText(saved_robot_id);
    txt_robot_id_->setClearButtonEnabled(true);
    v->addWidget(txt_robot_id_);

    v->addSpacing(10);

    // Access Code (+ view toggle)
    auto* lbl_code = new QLabel("ACCESS CODE", panel);
    lbl_code->setObjectName("SetupFieldLabel");
    v->addWidget(lbl_code);

    auto* code_row = new QHBoxLayout();
    code_row->setContentsMargins(0, 0, 0, 0);
    code_row->setSpacing(10);

    txt_access_code_ = new QLineEdit(panel);
    txt_access_code_->setObjectName("SetupLineEdit");
    txt_access_code_->setPlaceholderText("Enter Access Code");
    txt_access_code_->setEchoMode(QLineEdit::Password);
    txt_access_code_->setClearButtonEnabled(true);
    code_row->addWidget(txt_access_code_, 1);

    btn_view_code_ = new QToolButton(panel);
    btn_view_code_->setObjectName("SetupViewButton");
    btn_view_code_->setText("VIEW");
    btn_view_code_->setCheckable(true);
    btn_view_code_->setFocusPolicy(Qt::NoFocus);
    btn_view_code_->setToolTip("Show/hide access code");
    code_row->addWidget(btn_view_code_, 0, Qt::AlignVCenter);

    v->addLayout(code_row);

    // Error label (hidden unless needed)
    lbl_error_ = new QLabel(panel);
    lbl_error_->setObjectName("SetupError");
    lbl_error_->setVisible(false);
    lbl_error_->setWordWrap(true);
    v->addWidget(lbl_error_);

    v->addSpacing(10);

    // Arrow-only login/continue
    auto* arrow_row = new QHBoxLayout();
    arrow_row->setContentsMargins(0, 0, 0, 0);
    arrow_row->setSpacing(0);
    arrow_row->addStretch(1);

    btn_arrow_login_ = new QToolButton(panel);
    btn_arrow_login_->setObjectName("SetupArrowButton");
    btn_arrow_login_->setText("→");
    btn_arrow_login_->setToolTip("Continue");
    btn_arrow_login_->setEnabled(false);
    btn_arrow_login_->setFixedSize(56, 44);
    arrow_row->addWidget(btn_arrow_login_, 0, Qt::AlignHCenter);

    arrow_row->addStretch(1);
    v->addLayout(arrow_row);

    root->addWidget(panel, 0, Qt::AlignHCenter);
    root->addStretch(2);

    applyLocalStyle();

    // Wiring
    connect(txt_robot_id_, &QLineEdit::textChanged, this, &SetupScreen::updateUiState);
    connect(txt_access_code_, &QLineEdit::textChanged, this, &SetupScreen::updateUiState);
    connect(btn_view_code_, &QToolButton::clicked, this, &SetupScreen::toggleAccessCodeVisible);
    connect(btn_arrow_login_, &QToolButton::clicked, this, &SetupScreen::submit);

    connect(txt_access_code_, &QLineEdit::returnPressed, this, &SetupScreen::submit);
    connect(txt_robot_id_, &QLineEdit::returnPressed, this, &SetupScreen::submit);

    updateUiState();
}

void SetupScreen::applyLocalStyle() {
    // Apply styling locally so we don't affect the existing planner theming.
    // (We intentionally do NOT touch qApp->setStyleSheet here.)
    setStyleSheet(R"(
        #SetupScreenRoot {
            background-color: #060606;
        }

        #SetupPanel {
            background: transparent;
        }

        #SetupSubtitle {
            color: #a9b0b6;
            font-size: 12px;
            letter-spacing: 2px;
        }

        #SetupFieldLabel {
            color: #00b35a;
            font-size: 11px;
            font-weight: 600;
            letter-spacing: 1px;
        }

        #SetupLineEdit {
            background-color: #0b0b0b;
            border: 1px solid rgba(0, 179, 90, 0.55);
            border-radius: 2px;
            padding: 10px 12px;
            color: #d7dde3;
            font-size: 13px;
        }
        #SetupLineEdit:focus {
            border: 1px solid rgba(0, 179, 90, 1.0);
        }

        #SetupViewButton {
            background-color: transparent;
            border: 1px solid rgba(0, 179, 90, 0.55);
            border-radius: 2px;
            padding: 10px 12px;
            color: #00b35a;
            font-size: 11px;
            font-weight: 700;
            letter-spacing: 1px;
        }
        #SetupViewButton:checked {
            background-color: rgba(0, 179, 90, 0.18);
            border: 1px solid rgba(0, 179, 90, 1.0);
        }

        #SetupArrowButton {
            background-color: transparent;
            border: 1px solid rgba(0, 179, 90, 0.35);
            border-radius: 2px;
            color: rgba(0, 179, 90, 0.55);
            font-size: 18px;
            font-weight: 800;
        }
        #SetupArrowButton:enabled {
            background-color: #00b35a;
            border: 1px solid #00b35a;
            color: #0b0b0b;
        }

        #SetupError {
            color: #ff6b6b;
            font-size: 12px;
        }
    )");
}

void SetupScreen::updateUiState() {
    const QString robot_id = trimmed(txt_robot_id_ ? txt_robot_id_->text() : QString());
    const QString access_code = trimmed(txt_access_code_ ? txt_access_code_->text() : QString());

    const bool ready = !robot_id.isEmpty() && !access_code.isEmpty();
    if (btn_arrow_login_) {
        btn_arrow_login_->setEnabled(ready);
    }
    if (lbl_error_) {
        lbl_error_->setVisible(false);
        lbl_error_->clear();
    }
}

void SetupScreen::toggleAccessCodeVisible() {
    access_code_visible_ = btn_view_code_ && btn_view_code_->isChecked();

    if (txt_access_code_) {
        txt_access_code_->setEchoMode(access_code_visible_ ? QLineEdit::Normal : QLineEdit::Password);
    }
    if (btn_view_code_) {
        btn_view_code_->setText(access_code_visible_ ? "HIDE" : "VIEW");
    }
}

void SetupScreen::submit() {
    const QString robot_id = trimmed(txt_robot_id_ ? txt_robot_id_->text() : QString());
    const QString access_code = trimmed(txt_access_code_ ? txt_access_code_->text() : QString());

    if (robot_id.isEmpty() || access_code.isEmpty()) {
        if (lbl_error_) {
            lbl_error_->setText("Please enter both Robot ID and Access Code.");
            lbl_error_->setVisible(true);
        }
        return;
    }

    // Persist Robot ID for convenience.
    QSettings settings(kOrgName, kAppName);
    settings.setValue("setup/robot_id", robot_id);

    emit loginSubmitted(robot_id, access_code);
}

}  // namespace f2c_cpp

