#include "components/mission_finalize_dialog.hpp"

#include "ui_theme_constants.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QVariant>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace f2c_cpp {

MissionFinalizeDialog::MissionFinalizeDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);
    setObjectName("MissionFinalizeDialog");
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(520);
    const UiThemeTokens t = appThemeTokens();
    setStyleSheet(QStringLiteral(R"CSS(
        #MissionFinalizeDialog {
            background: %1; border: 1px solid %2; border-radius: 14px;
        }
        QLabel#FinalizeHeader {
            color: %3; font-family: 'Arimo'; font-size: 20px;
            font-weight: 700; background: transparent;
        }
        QLabel#FinalizePhase {
            color: %4; font-family: 'Arimo'; font-size: 15px;
            font-weight: 600; background: transparent;
        }
        QLabel#FinalizeDetail {
            color: %5; font-family: 'Arimo'; font-size: 13px;
            background: transparent;
        }
        QLabel#FinalizeDetail[error="true"] { color: %6; }
        QProgressBar#FinalizeBusy {
            background: %7; border: none; border-radius: 3px; max-height: 6px;
        }
        QProgressBar#FinalizeBusy::chunk { background: %8; border-radius: 3px; }
        QPushButton {
            background: %2; border: none; border-radius: 8px; color: %3;
            font-family: 'Arimo'; font-size: 14px; font-weight: 600;
            min-height: 40px; padding: 0 18px;
        }
        QPushButton:hover { background: %9; }
        QPushButton#FinalizeSkip { background: %10; color: #FFFFFF; }
        QPushButton#FinalizeSkip:hover { background: %11; }
        QPushButton#FinalizeAbort { background: %12; color: #FFFFFF; }
        QPushButton#FinalizeAbort:hover { background: %13; }
        QPushButton#FinalizeClose { background: %8; color: %14; font-weight: 700; }
        QPushButton#FinalizeClose:hover { background: %15; }
    )CSS")
                      .arg(t.surface, t.raised_border, t.text, t.body, t.muted,
                           t.danger, t.raised, t.accent_green, t.neutral_hover)
                      .arg(t.warning_fill, t.warning_fill_hover, t.danger_fill,
                           t.danger_fill_hover, t.on_accent,
                           t.accent_green_hover));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(12);

    lbl_header_ = new QLabel(QStringLiteral("Completing Mission"), this);
    lbl_header_->setObjectName("FinalizeHeader");
    root->addWidget(lbl_header_);

    lbl_phase_ = new QLabel(QStringLiteral("Stopping autonomy…"), this);
    lbl_phase_->setObjectName("FinalizePhase");
    lbl_phase_->setWordWrap(true);
    root->addWidget(lbl_phase_);

    busy_ = new QProgressBar(this);
    busy_->setObjectName("FinalizeBusy");
    busy_->setRange(0, 0);  // indeterminate
    busy_->setTextVisible(false);
    root->addWidget(busy_);

    lbl_detail_ = new QLabel(this);
    lbl_detail_->setObjectName("FinalizeDetail");
    lbl_detail_->setWordWrap(true);
    lbl_detail_->hide();
    root->addWidget(lbl_detail_);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    buttons->addStretch(1);
    btn_abort_ = new QPushButton(QStringLiteral("Abort & save partial"), this);
    btn_abort_->setObjectName("FinalizeAbort");
    btn_abort_->hide();
    btn_skip_copy_ = new QPushButton(QStringLiteral("Skip thumb-drive copy"), this);
    btn_skip_copy_->setObjectName("FinalizeSkip");
    btn_skip_copy_->hide();
    btn_force_ = new QPushButton(QStringLiteral("Force stop"), this);
    btn_force_->setObjectName("FinalizeAbort");
    btn_force_->hide();
    btn_close_ = new QPushButton(QStringLiteral("Close"), this);
    btn_close_->setObjectName("FinalizeClose");
    btn_close_->hide();
    buttons->addWidget(btn_abort_);
    buttons->addWidget(btn_skip_copy_);
    buttons->addWidget(btn_force_);
    buttons->addWidget(btn_close_);
    root->addLayout(buttons);

    connect(btn_skip_copy_, &QPushButton::clicked, this, [this] {
        btn_skip_copy_->setEnabled(false);
        emit skipCopyRequested();
    });
    connect(btn_abort_, &QPushButton::clicked, this, [this] {
        btn_abort_->setEnabled(false);
        emit abortAndSaveRequested();
    });
    connect(btn_force_, &QPushButton::clicked, this, [this] {
        btn_force_->setEnabled(false);
        emit forceStopRequested();
    });
    connect(btn_close_, &QPushButton::clicked, this, &QDialog::accept);
}

void MissionFinalizeDialog::setTitle(const QString& text) {
    lbl_header_->setText(text);
}

void MissionFinalizeDialog::setPhase(const QString& text) {
    lbl_phase_->setText(text);
}

void MissionFinalizeDialog::setDetail(const QString& text, bool is_error) {
    lbl_detail_->setVisible(!text.isEmpty());
    lbl_detail_->setText(text);
    lbl_detail_->setProperty("error", QVariant(is_error));
    lbl_detail_->style()->unpolish(lbl_detail_);
    lbl_detail_->style()->polish(lbl_detail_);
}

void MissionFinalizeDialog::setSkipCopyAvailable(bool available) {
    btn_skip_copy_->setVisible(available);
    if (available) {
        btn_skip_copy_->setEnabled(true);
    }
}

void MissionFinalizeDialog::setAbortAvailable(bool available) {
    btn_abort_->setVisible(available);
    if (available) {
        btn_abort_->setEnabled(true);
    }
}

void MissionFinalizeDialog::setForceStopAvailable(bool available) {
    btn_force_->setVisible(available);
    if (available) {
        btn_force_->setEnabled(true);
    }
}

void MissionFinalizeDialog::finish(bool ok, const QString& summary) {
    busy_->setRange(0, 1);
    busy_->setValue(ok ? 1 : 0);
    lbl_phase_->setText(ok ? QStringLiteral("Mission complete")
                           : QStringLiteral("Mission ended with warnings"));
    setDetail(summary, !ok);
    btn_skip_copy_->hide();
    btn_abort_->hide();
    btn_force_->hide();
    btn_close_->show();
    btn_close_->setFocus();
}

}  // namespace f2c_cpp
