/**
 * @file bdr_input_dialog.cpp
 */

#include "components/bdr_input_dialog.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVariant>

namespace f2c_cpp {

BdrInputDialog::BdrInputDialog(QWidget* parent, bool dark_mode)
    : QDialog(parent), dark_mode_(dark_mode) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowModality(Qt::ApplicationModal);
    buildUi();
    applyStyle();
}

void BdrInputDialog::buildUi() {
    auto* container = new QWidget(this);
    container->setObjectName("BdrInputDialogContainer");
    container->setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(container);

    auto* main_layout = new QVBoxLayout(container);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    // Top bar (title + close).
    auto* top_bar = new QWidget(container);
    top_bar->setObjectName("BdrInputDialogTopBar");
    top_bar->setAttribute(Qt::WA_StyledBackground, true);
    top_bar->setFixedHeight(48);
    auto* top_layout = new QHBoxLayout(top_bar);
    top_layout->setContentsMargins(20, 0, 12, 0);
    top_layout->setSpacing(0);

    lbl_title_ = new QLabel(top_bar);
    lbl_title_->setObjectName("BdrInputDialogTitle");
    lbl_title_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    top_layout->addWidget(lbl_title_, 1);

    btn_close_ = new QPushButton(top_bar);
    btn_close_->setObjectName("BdrInputDialogClose");
    btn_close_->setFlat(true);
    btn_close_->setFixedSize(32, 32);
    btn_close_->setText("\u00d7");
    btn_close_->setCursor(Qt::PointingHandCursor);
    connect(btn_close_, &QPushButton::clicked, this, &BdrInputDialog::onReject);
    top_layout->addWidget(btn_close_, 0, Qt::AlignRight | Qt::AlignVCenter);

    main_layout->addWidget(top_bar);

    // Content (prompt + line edit + error).
    auto* content = new QWidget(container);
    content->setObjectName("BdrInputDialogContent");
    content->setAttribute(Qt::WA_StyledBackground, true);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(20, 16, 20, 16);
    content_layout->setSpacing(8);

    lbl_prompt_ = new QLabel(content);
    lbl_prompt_->setObjectName("BdrInputDialogPrompt");
    lbl_prompt_->setWordWrap(true);
    content_layout->addWidget(lbl_prompt_);

    edit_ = new QLineEdit(content);
    edit_->setObjectName("BdrInputDialogEdit");
    edit_->setMinimumWidth(320);
    connect(edit_, &QLineEdit::returnPressed, this, &BdrInputDialog::onAccept);
    content_layout->addWidget(edit_);

    lbl_error_ = new QLabel(content);
    lbl_error_->setObjectName("BdrInputDialogError");
    lbl_error_->setWordWrap(true);
    lbl_error_->hide();
    content_layout->addWidget(lbl_error_);

    main_layout->addWidget(content);

    // Buttons.
    auto* button_box = new QWidget(container);
    button_box->setObjectName("BdrInputDialogButtonBox");
    button_box->setAttribute(Qt::WA_StyledBackground, true);
    auto* button_layout = new QHBoxLayout(button_box);
    button_layout->setContentsMargins(20, 0, 20, 20);
    button_layout->setSpacing(12);
    button_layout->addStretch();

    btn_cancel_ = new QPushButton(button_box);
    btn_cancel_->setText(tr("Cancel"));
    btn_cancel_->setCursor(Qt::PointingHandCursor);
    connect(btn_cancel_, &QPushButton::clicked, this, &BdrInputDialog::onReject);
    button_layout->addWidget(btn_cancel_);

    btn_ok_ = new QPushButton(button_box);
    btn_ok_->setText(tr("OK"));
    btn_ok_->setCursor(Qt::PointingHandCursor);
    btn_ok_->setProperty("primary", QVariant(true));
    btn_ok_->setDefault(true);
    connect(btn_ok_, &QPushButton::clicked, this, &BdrInputDialog::onAccept);
    button_layout->addWidget(btn_ok_);

    main_layout->addWidget(button_box);
}

void BdrInputDialog::applyStyle() {
    // Two palettes — dark mirrors BdrMessageBox; light is the codebase's
    // standard zinc-ish surface used in PlannerScreen light mode.
    const QString container_bg = dark_mode_ ? QStringLiteral("#121212")
                                            : QStringLiteral("#FFFFFF");
    const QString border = dark_mode_ ? QStringLiteral("#333333")
                                      : QStringLiteral("#E4E4E7");
    const QString title_color = dark_mode_ ? QStringLiteral("#e2e8f0")
                                           : QStringLiteral("#1F2937");
    const QString text_color = dark_mode_ ? QStringLiteral("#e2e8f0")
                                          : QStringLiteral("#374151");
    const QString muted = dark_mode_ ? QStringLiteral("#94a3b8")
                                     : QStringLiteral("#6B7280");
    const QString edit_bg = dark_mode_ ? QStringLiteral("#1e1e1e")
                                       : QStringLiteral("#F9FAFB");
    const QString edit_border = dark_mode_ ? QStringLiteral("#3f3f46")
                                           : QStringLiteral("#D4D4D8");
    const QString edit_focus = QStringLiteral("#F59E0B");
    const QString error = dark_mode_ ? QStringLiteral("#F87171")
                                     : QStringLiteral("#DC2626");
    const QString btn_bg = dark_mode_ ? QStringLiteral("#1e1e1e")
                                      : QStringLiteral("#F4F4F5");
    const QString btn_border = dark_mode_ ? QStringLiteral("#444444")
                                          : QStringLiteral("#D4D4D8");
    const QString btn_hover = dark_mode_ ? QStringLiteral("#2a2a2a")
                                         : QStringLiteral("#E4E4E7");
    const QString btn_text = dark_mode_ ? QStringLiteral("#FFFFFF")
                                        : QStringLiteral("#1F2937");
    const QString primary = QStringLiteral("#059669");
    const QString primary_hover = QStringLiteral("#047857");

    setStyleSheet(QStringLiteral(
        "#BdrInputDialogContainer {"
        "    background-color: %1;"
        "    border: 1px solid %2;"
        "    border-radius: 8px;"
        "}"
        "#BdrInputDialogTopBar {"
        "    background-color: %1;"
        "    border-bottom: 1px solid %2;"
        "    border-top-left-radius: 8px;"
        "    border-top-right-radius: 8px;"
        "}"
        "#BdrInputDialogTitle {"
        "    background: transparent;"
        "    color: %3;"
        "    font-size: 16px;"
        "    font-weight: 600;"
        "}"
        "#BdrInputDialogClose {"
        "    background: transparent;"
        "    color: %5;"
        "    font-size: 20px;"
        "    border: none;"
        "}"
        "#BdrInputDialogClose:hover {"
        "    color: %3;"
        "    background: %6;"
        "    border-radius: 4px;"
        "}"
        "#BdrInputDialogContent {"
        "    background-color: %1;"
        "}"
        "#BdrInputDialogPrompt {"
        "    background: transparent;"
        "    color: %4;"
        "    font-size: 13px;"
        "}"
        "#BdrInputDialogEdit {"
        "    background-color: %6;"
        "    color: %4;"
        "    border: 1px solid %7;"
        "    border-radius: 6px;"
        "    padding: 8px 10px;"
        "    font-size: 13px;"
        "}"
        "#BdrInputDialogEdit:focus {"
        "    border: 1px solid %8;"
        "}"
        "#BdrInputDialogError {"
        "    background: transparent;"
        "    color: %9;"
        "    font-size: 12px;"
        "    font-weight: 500;"
        "}"
        "#BdrInputDialogButtonBox {"
        "    background-color: %1;"
        "    border-bottom-left-radius: 8px;"
        "    border-bottom-right-radius: 8px;"
        "}"
        "#BdrInputDialogButtonBox QPushButton {"
        "    background-color: %10;"
        "    color: %13;"
        "    border: 1px solid %11;"
        "    border-radius: 4px;"
        "    padding: 8px 16px;"
        "    font-size: 13px;"
        "}"
        "#BdrInputDialogButtonBox QPushButton:hover {"
        "    background-color: %12;"
        "}"
        "#BdrInputDialogButtonBox QPushButton[primary=\"true\"] {"
        "    background-color: %14;"
        "    color: #FFFFFF;"
        "    border: none;"
        "}"
        "#BdrInputDialogButtonBox QPushButton[primary=\"true\"]:hover {"
        "    background-color: %15;"
        "}")
        .arg(container_bg, border, title_color, text_color, muted, edit_bg,
             edit_border, edit_focus, error)
        .arg(btn_bg, btn_border, btn_hover, btn_text, primary, primary_hover));
}

void BdrInputDialog::setTitle(const QString& title) {
    if (lbl_title_) lbl_title_->setText(title);
}

void BdrInputDialog::setPrompt(const QString& prompt) {
    if (lbl_prompt_) lbl_prompt_->setText(prompt);
}

void BdrInputDialog::setDefaultText(const QString& text) {
    if (edit_) {
        edit_->setText(text);
        edit_->selectAll();
    }
}

void BdrInputDialog::setValidator(Validator v) {
    validator_ = std::move(v);
}

void BdrInputDialog::setDarkMode(bool dark_mode) {
    if (dark_mode_ == dark_mode) return;
    dark_mode_ = dark_mode;
    applyStyle();
}

QString BdrInputDialog::text() const {
    return edit_ ? edit_->text().trimmed() : QString();
}

void BdrInputDialog::onAccept() {
    const QString candidate = text();
    if (validator_) {
        const QString err = validator_(candidate);
        if (!err.isEmpty()) {
            if (lbl_error_) {
                lbl_error_->setText(err);
                lbl_error_->show();
            }
            if (edit_) {
                edit_->setFocus(Qt::OtherFocusReason);
                edit_->selectAll();
            }
            return;
        }
    } else if (candidate.isEmpty()) {
        if (lbl_error_) {
            lbl_error_->setText(tr("Name cannot be empty."));
            lbl_error_->show();
        }
        return;
    }
    accept();
}

void BdrInputDialog::onReject() {
    reject();
}

QString BdrInputDialog::getText(QWidget* parent,
                                const QString& title,
                                const QString& prompt,
                                const QString& default_text,
                                bool dark_mode,
                                Validator validator,
                                bool* accepted) {
    BdrInputDialog dlg(parent, dark_mode);
    dlg.setWindowTitle(title);
    dlg.setTitle(title);
    dlg.setPrompt(prompt);
    dlg.setDefaultText(default_text);
    if (validator) dlg.setValidator(std::move(validator));
    const int rc = dlg.exec();
    const bool ok = (rc == QDialog::Accepted);
    if (accepted) *accepted = ok;
    return ok ? dlg.text() : QString();
}

}  // namespace f2c_cpp
