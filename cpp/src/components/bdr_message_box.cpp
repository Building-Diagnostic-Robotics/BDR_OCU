/**
 * @file bdr_message_box.cpp
 * @brief Custom frameless message box matching BDR dark-theme design.
 */

#include "components/bdr_message_box.hpp"

#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QSvgRenderer>
#else
#include <QtSvg/QSvgRenderer>
#endif

namespace f2c_cpp {

namespace {

QPixmap loadSvgPixmap(const QString& resourcePath, int w, int h,
                      const QString& strokeColor = QString()) {
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }
    QByteArray data = f.readAll();
    f.close();

    if (!strokeColor.isEmpty()) {
        int strokeStart = data.indexOf("stroke=\"");
        if (strokeStart >= 0) {
            int valueStart = strokeStart + 8;
            int valueEnd = data.indexOf('"', valueStart);
            if (valueEnd > valueStart) {
                QByteArray newColor = strokeColor.toUtf8();
                if (!newColor.startsWith('#')) {
                    newColor.prepend('#');
                }
                data = data.left(valueStart) + newColor + data.mid(valueEnd);
            }
        }
    }

    QSvgRenderer renderer(data);
    if (!renderer.isValid()) {
        return QPixmap();
    }
    QPixmap pix(w, h);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    renderer.render(&painter);
    return pix;
}

}  // namespace

BdrMessageBox::BdrMessageBox(QWidget* parent)
    : QDialog(parent) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowModality(Qt::ApplicationModal);
    buildUi();
    applyStyle();
}

void BdrMessageBox::buildUi() {
    auto* container = new QWidget(this);
    container->setObjectName("BdrMessageBoxContainer");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(container);

    auto* mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Top bar
    auto* topBar = new QWidget(container);
    topBar->setObjectName("BdrMessageBoxTopBar");
    topBar->setFixedHeight(48);
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(20, 0, 12, 0);
    topLayout->setSpacing(0);

    lbl_title_ = new QLabel(topBar);
    lbl_title_->setObjectName("BdrMessageBoxTitle");
    lbl_title_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    topLayout->addWidget(lbl_title_, 1);

    btn_close_ = new QPushButton(topBar);
    btn_close_->setObjectName("BdrMessageBoxClose");
    btn_close_->setFlat(true);
    btn_close_->setFixedSize(32, 32);
    btn_close_->setText("×");
    btn_close_->setCursor(Qt::PointingHandCursor);
    connect(btn_close_, &QPushButton::clicked, this, [this]() {
        result_ = static_cast<int>(Cancel);
        reject();
    });
    topLayout->addWidget(btn_close_, 0, Qt::AlignRight | Qt::AlignVCenter);

    mainLayout->addWidget(topBar);

    // Content area (icon + text)
    auto* content = new QWidget(container);
    content->setObjectName("BdrMessageBoxContent");
    auto* contentLayout = new QHBoxLayout(content);
    contentLayout->setContentsMargins(20, 20, 20, 16);
    contentLayout->setSpacing(16);

    lbl_icon_ = new QLabel(content);
    lbl_icon_->setObjectName("BdrMessageBoxIcon");
    lbl_icon_->setFixedSize(40, 40);
    lbl_icon_->setAlignment(Qt::AlignCenter);
    lbl_icon_->setScaledContents(false);
    contentLayout->addWidget(lbl_icon_, 0, Qt::AlignTop);

    lbl_text_ = new QLabel(content);
    lbl_text_->setObjectName("BdrMessageBoxText");
    lbl_text_->setWordWrap(true);
    lbl_text_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    contentLayout->addWidget(lbl_text_, 1);

    mainLayout->addWidget(content);

    // Button box
    button_box_ = new QWidget(container);
    button_box_->setObjectName("BdrMessageBoxButtonBox");
    button_layout_ = new QHBoxLayout(button_box_);
    button_layout_->setContentsMargins(20, 0, 20, 20);
    button_layout_->setSpacing(12);
    button_layout_->addStretch();
    mainLayout->addWidget(button_box_);
}

void BdrMessageBox::applyStyle() {
    setStyleSheet(R"(
        #BdrMessageBoxContainer {
            background-color: #121212;
            border: 1px solid #333333;
            border-radius: 8px;
        }
        #BdrMessageBoxTopBar {
            background-color: #121212;
            border-bottom: 1px solid #333333;
            border-top-left-radius: 8px;
            border-top-right-radius: 8px;
        }
        #BdrMessageBoxTitle {
            color: #e2e8f0;
            font-size: 16px;
            font-weight: 600;
        }
        #BdrMessageBoxClose {
            background: transparent;
            color: #94a3b8;
            font-size: 20px;
            border: none;
        }
        #BdrMessageBoxClose:hover {
            color: #e2e8f0;
            background: #1e1e1e;
            border-radius: 4px;
        }
        #BdrMessageBoxContent {
            background-color: #121212;
        }
        #BdrMessageBoxText {
            color: #e2e8f0;
            font-size: 14px;
            line-height: 1.5;
        }
        #BdrMessageBoxButtonBox {
            background-color: #121212;
            border-bottom-left-radius: 8px;
            border-bottom-right-radius: 8px;
        }
        #BdrMessageBoxButtonBox QPushButton {
            background-color: #1e1e1e;
            color: #ffffff;
            border: 1px solid #444444;
            border-radius: 4px;
            padding: 8px 16px;
            font-size: 14px;
        }
        #BdrMessageBoxButtonBox QPushButton:hover {
            background-color: #2a2a2a;
        }
        #BdrMessageBoxButtonBox QPushButton[primary="true"] {
            background-color: #059669;
            border: none;
        }
        #BdrMessageBoxButtonBox QPushButton[primary="true"]:hover {
            background-color: #047857;
        }
    )");
}

void BdrMessageBox::setTitle(const QString& title) {
    if (lbl_title_) {
        lbl_title_->setText(title);
    }
}

void BdrMessageBox::setText(const QString& text) {
    if (lbl_text_) {
        QString full = text;
        if (!informative_text_.isEmpty()) {
            full += "\n\n" + informative_text_;
        }
        lbl_text_->setText(full);
    }
}

void BdrMessageBox::setInformativeText(const QString& text) {
    informative_text_ = text;
}

void BdrMessageBox::setIconPath(const QString& path, const QString& strokeColor) {
    if (!lbl_icon_) return;
    QPixmap pix = loadSvgPixmap(path, 40, 40, strokeColor);
    if (!pix.isNull()) {
        lbl_icon_->setPixmap(pix);
        lbl_icon_->show();
    } else {
        lbl_icon_->hide();
    }
}

int BdrMessageBox::addButton(const QString& label, bool isPrimary) {
    if (!button_layout_) return -1;

    auto* btn = new QPushButton(button_box_);
    btn->setText(label);
    if (isPrimary) {
        btn->setProperty("primary", QVariant(true));
    }
    btn->setCursor(Qt::PointingHandCursor);
    const int index = buttons_.size();
    connect(btn, &QPushButton::clicked, this, [this, index]() { onButtonClicked(index); });
    buttons_.append(btn);
    button_layout_->insertWidget(button_layout_->count() - 1, btn);
    return index;
}

void BdrMessageBox::onCloseClicked() {
    result_ = static_cast<int>(Cancel);
    reject();
}

void BdrMessageBox::onButtonClicked(int index) {
    if (index >= 0 && index < buttons_.size()) {
        result_ = index;
        accept();
    }
}

int BdrMessageBox::information(QWidget* parent, const QString& title, const QString& text) {
    BdrMessageBox box(parent);
    box.setWindowTitle(title);
    box.setTitle(title);
    box.setText(text);
    box.setIconPath(":/assets/dashboard/battery.svg", "#3b82f6");
    box.addButton(QObject::tr("OK"), true);
    box.exec();
    return static_cast<int>(Ok);
}

int BdrMessageBox::warning(QWidget* parent, const QString& title, const QString& text) {
    BdrMessageBox box(parent);
    box.setWindowTitle(title);
    box.setTitle(title);
    box.setText(text);
    box.setIconPath(":/assets/dashboard/settings.svg", "#f59e0b");
    box.addButton(QObject::tr("OK"), true);
    box.exec();
    return static_cast<int>(Ok);
}

int BdrMessageBox::critical(QWidget* parent, const QString& title, const QString& text) {
    BdrMessageBox box(parent);
    box.setWindowTitle(title);
    box.setTitle(title);
    box.setText(text);
    box.setIconPath(":/assets/dashboard/settings.svg", "#ef4444");
    box.addButton(QObject::tr("OK"), true);
    box.exec();
    return static_cast<int>(Ok);
}

int BdrMessageBox::question(QWidget* parent, const QString& title, const QString& text,
                             int defaultButton) {
    BdrMessageBox box(parent);
    box.setWindowTitle(title);
    box.setTitle(title);
    box.setText(text);
    box.setIconPath(":/assets/dashboard/location_pin.svg", "#94a3b8");
    const bool no_default = defaultButton == static_cast<int>(No);
    box.addButton(QObject::tr("No"), no_default);
    box.addButton(QObject::tr("Yes"), !no_default);
    if (box.buttons_.size() >= 2) {
        box.buttons_[0]->setObjectName(QStringLiteral("BdrMessageBoxNo"));
        box.buttons_[1]->setObjectName(QStringLiteral("BdrMessageBoxYes"));
        box.buttons_[0]->setAutoDefault(no_default);
        box.buttons_[0]->setDefault(no_default);
        box.buttons_[1]->setAutoDefault(!no_default);
        box.buttons_[1]->setDefault(!no_default);
        if (no_default) {
            box.buttons_[0]->setFocus(Qt::OtherFocusReason);
        } else {
            box.buttons_[1]->setFocus(Qt::OtherFocusReason);
        }
    }
    box.exec();

    if (box.result() == QDialog::Accepted) {
        const int idx = box.clickedButtonIndex();
        if (idx >= 0) {
            return idx == 1 ? static_cast<int>(Yes) : static_cast<int>(No);
        }
    }
    return defaultButton;
}

int BdrMessageBox::custom(QWidget* parent, const QString& title, const QString& text,
                          const QStringList& buttonLabels, int defaultIndex,
                          const QString& informativeText) {
    BdrMessageBox box(parent);
    box.setWindowTitle(title);
    box.setTitle(title);
    if (!informativeText.isEmpty()) {
        box.setInformativeText(informativeText);
    }
    box.setText(text);
    box.setIconPath(":/assets/dashboard/settings.svg", "#94a3b8");

    for (int i = 0; i < buttonLabels.size(); ++i) {
        box.addButton(buttonLabels.at(i), (i == buttonLabels.size() - 1));
    }

    box.exec();

    if (box.result() == QDialog::Accepted) {
        const int idx = box.clickedButtonIndex();
        if (idx >= 0) return idx;
    }
    return defaultIndex >= 0 ? defaultIndex : -1;
}

}  // namespace f2c_cpp
