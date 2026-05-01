/**
 * @file bdr_progress_dialog.cpp
 * @brief Custom frameless progress dialog matching BDR dark-theme design.
 */

#include "components/bdr_progress_dialog.hpp"

#include <QHBoxLayout>
#include <QTimer>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace f2c_cpp {

BdrProgressDialog::BdrProgressDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowModality(Qt::ApplicationModal);
    buildUi();
    applyStyle();
}

void BdrProgressDialog::buildUi() {
    auto* container = new QWidget(this);
    container->setObjectName("BdrProgressDialogContainer");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(container);

    auto* mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Title bar
    auto* topBar = new QWidget(container);
    topBar->setObjectName("BdrProgressDialogTopBar");
    topBar->setFixedHeight(48);
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(20, 0, 12, 0);
    topLayout->setSpacing(0);

    lbl_title_ = new QLabel(topBar);
    lbl_title_->setObjectName("BdrProgressDialogTitle");
    lbl_title_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    topLayout->addWidget(lbl_title_, 1);

    btn_close_ = new QPushButton(topBar);
    btn_close_->setObjectName("BdrProgressDialogClose");
    btn_close_->setFlat(true);
    btn_close_->setFixedSize(32, 32);
    btn_close_->setText("×");
    btn_close_->setCursor(Qt::PointingHandCursor);
    btn_close_->hide();
    connect(btn_close_, &QPushButton::clicked, this, &QDialog::reject);
    topLayout->addWidget(btn_close_, 0, Qt::AlignRight | Qt::AlignVCenter);

    mainLayout->addWidget(topBar);

    // Content: status label + progress bar
    auto* content = new QWidget(container);
    content->setObjectName("BdrProgressDialogContent");
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(20, 16, 20, 20);
    contentLayout->setSpacing(12);

    lbl_status_ = new QLabel(content);
    lbl_status_->setObjectName("BdrProgressDialogStatus");
    lbl_status_->setWordWrap(true);
    lbl_status_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    contentLayout->addWidget(lbl_status_);

    progress_bar_ = new QProgressBar(content);
    progress_bar_->setObjectName("BdrProgressDialogProgressBar");
    progress_bar_->setRange(0, 0);
    progress_bar_->setValue(0);
    progress_bar_->setTextVisible(false);
    contentLayout->addWidget(progress_bar_);

    mainLayout->addWidget(content);
}

void BdrProgressDialog::applyStyle() {
    setStyleSheet(R"(
        #BdrProgressDialogContainer {
            background-color: #121212;
            border: 1px solid #333333;
            border-radius: 8px;
        }
        #BdrProgressDialogTopBar {
            background-color: #121212;
            border-bottom: 1px solid #333333;
            border-top-left-radius: 8px;
            border-top-right-radius: 8px;
        }
        #BdrProgressDialogTitle {
            color: #e2e8f0;
            font-size: 16px;
            font-weight: 600;
        }
        #BdrProgressDialogClose {
            background: transparent;
            color: #94a3b8;
            font-size: 20px;
            border: none;
        }
        #BdrProgressDialogClose:hover {
            color: #e2e8f0;
            background: #1e1e1e;
            border-radius: 4px;
        }
        #BdrProgressDialogContent {
            background-color: #121212;
            border-bottom-left-radius: 8px;
            border-bottom-right-radius: 8px;
        }
        #BdrProgressDialogStatus {
            color: #e2e8f0;
            font-size: 18px;
            font-weight: bold;
            line-height: 1.5;
        }
        #BdrProgressDialogProgressBar {
            background-color: #1e1e1e;
            border: 1px solid #333333;
            border-radius: 4px;
            height: 8px;
        }
        #BdrProgressDialogProgressBar::chunk {
            background-color: #059669;
            border-radius: 3px;
        }
    )");
}

void BdrProgressDialog::setTitle(const QString& title) {
    if (lbl_title_) {
        lbl_title_->setText(title);
    }
}

void BdrProgressDialog::setLabelText(const QString& text) {
    if (lbl_status_) {
        lbl_status_->setText(text);
    }
}

void BdrProgressDialog::setRange(int minimum, int maximum) {
    if (progress_bar_) {
        progress_bar_->setRange(minimum, maximum);
    }
}

void BdrProgressDialog::setValue(int value) {
    if (progress_bar_) {
        progress_bar_->setValue(value);
    }
}

void BdrProgressDialog::setCancelButton(QPushButton* button) {
    cancel_button_ = button;
}

void BdrProgressDialog::setMinimumDuration(int ms) {
    Q_UNUSED(ms);
}

int BdrProgressDialog::value() const {
    return progress_bar_ ? progress_bar_->value() : 0;
}

int BdrProgressDialog::minimum() const {
    return progress_bar_ ? progress_bar_->minimum() : 0;
}

int BdrProgressDialog::maximum() const {
    return progress_bar_ ? progress_bar_->maximum() : 0;
}

void BdrProgressDialog::showSuccessAndClose(const QString& message) {
    if (lbl_status_) {
        lbl_status_->setText(message);
    }
    if (progress_bar_) {
        progress_bar_->setRange(0, 100);
        progress_bar_->setValue(100);
        progress_bar_->setStyleSheet(
            "QProgressBar { background-color: #1e1e1e; border: 1px solid #333333; "
            "border-radius: 4px; height: 8px; } "
            "QProgressBar::chunk { background-color: #059669; border-radius: 3px; }");
    }
    QTimer::singleShot(2500, this, SLOT(onCloseTimerFired()));
}

void BdrProgressDialog::onCloseTimerFired() {
    accept();
}

}  // namespace f2c_cpp
