#include "components/mission_finalize_dialog.hpp"

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
    setStyleSheet(QStringLiteral(R"CSS(
        #MissionFinalizeDialog {
            background: #18181B; border: 1px solid #3F3F46; border-radius: 14px;
        }
        QLabel#FinalizeHeader {
            color: #F4F4F5; font-family: 'Arimo'; font-size: 20px;
            font-weight: 700; background: transparent;
        }
        QLabel#FinalizePhase {
            color: #E4E4E7; font-family: 'Arimo'; font-size: 15px;
            font-weight: 600; background: transparent;
        }
        QLabel#FinalizeDetail {
            color: #A1A1AA; font-family: 'Arimo'; font-size: 13px;
            background: transparent;
        }
        QLabel#FinalizeDetail[error="true"] { color: #FCA5A5; }
        QProgressBar#FinalizeBusy {
            background: #27272A; border: none; border-radius: 3px; max-height: 6px;
        }
        QProgressBar#FinalizeBusy::chunk { background: #00BC7D; border-radius: 3px; }
        QPushButton {
            background: #3F3F47; border: none; border-radius: 8px; color: #F4F4F5;
            font-family: 'Arimo'; font-size: 14px; font-weight: 600;
            min-height: 40px; padding: 0 18px;
        }
        QPushButton:hover { background: #4A4A52; }
        QPushButton#FinalizeSkip { background: #B45309; }
        QPushButton#FinalizeSkip:hover { background: #D97706; }
        QPushButton#FinalizeAbort { background: #B91C1C; }
        QPushButton#FinalizeAbort:hover { background: #DC2626; }
        QPushButton#FinalizeClose { background: #00BC7D; color: #FFFFFF; font-weight: 700; }
        QPushButton#FinalizeClose:hover { background: #00A86D; }
    )CSS"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(12);

    auto* header = new QLabel(QStringLiteral("Completing Mission"), this);
    header->setObjectName("FinalizeHeader");
    root->addWidget(header);

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
    btn_close_ = new QPushButton(QStringLiteral("Close"), this);
    btn_close_->setObjectName("FinalizeClose");
    btn_close_->hide();
    buttons->addWidget(btn_abort_);
    buttons->addWidget(btn_skip_copy_);
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
    connect(btn_close_, &QPushButton::clicked, this, &QDialog::accept);
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

void MissionFinalizeDialog::finish(bool ok, const QString& summary) {
    busy_->setRange(0, 1);
    busy_->setValue(ok ? 1 : 0);
    lbl_phase_->setText(ok ? QStringLiteral("Mission complete")
                           : QStringLiteral("Mission ended with warnings"));
    setDetail(summary, !ok);
    btn_skip_copy_->hide();
    btn_abort_->hide();
    btn_close_->show();
    btn_close_->setFocus();
}

}  // namespace f2c_cpp
