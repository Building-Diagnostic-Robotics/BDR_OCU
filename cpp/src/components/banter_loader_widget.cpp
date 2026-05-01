#include "components/banter_loader_widget.hpp"

#include <QParallelAnimationGroup>
#include <QPropertyAnimation>

namespace f2c_cpp {

namespace {

const std::array<qreal, 11> kPercentSteps = {
    1.0 / 11.0, 2.0 / 11.0, 3.0 / 11.0, 4.0 / 11.0, 5.0 / 11.0, 6.0 / 11.0,
    7.0 / 11.0, 8.0 / 11.0, 9.0 / 11.0, 10.0 / 11.0, 1.0};

const std::array<std::array<QPoint, 11>, 9> kTranslations = {{
    // moveBox-1
    {{{-26, 0}, {0, 0}, {0, 0}, {26, 0}, {26, 26}, {26, 26},
      {26, 26}, {26, 0}, {0, 0}, {-26, 0}, {0, 0}}},
    // moveBox-2
    {{{0, 0}, {26, 0}, {0, 0}, {26, 0}, {26, 26}, {26, 26},
      {26, 26}, {26, 26}, {0, 26}, {0, 26}, {0, 0}}},
    // moveBox-3
    {{{-26, 0}, {-26, 0}, {0, 0}, {-26, 0}, {-26, 0}, {-26, 0},
      {-26, 0}, {-26, 0}, {-26, -26}, {0, -26}, {0, 0}}},
    // moveBox-4
    {{{-26, 0}, {-26, 0}, {-26, -26}, {0, -26}, {0, 0}, {0, -26},
      {0, -26}, {0, -26}, {-26, -26}, {-26, 0}, {0, 0}}},
    // moveBox-5
    {{{0, 0}, {0, 0}, {0, 0}, {26, 0}, {26, 0}, {26, 0},
      {26, 0}, {26, 0}, {26, -26}, {0, -26}, {0, 0}}},
    // moveBox-6
    {{{0, 0}, {-26, 0}, {-26, 0}, {0, 0}, {0, 0}, {0, 0},
      {0, 0}, {0, 26}, {-26, 26}, {-26, 0}, {0, 0}}},
    // moveBox-7
    {{{26, 0}, {26, 0}, {26, 0}, {0, 0}, {0, -26}, {26, -26},
      {0, -26}, {0, -26}, {0, 0}, {26, 0}, {0, 0}}},
    // moveBox-8
    {{{0, 0}, {-26, 0}, {-26, -26}, {0, -26}, {0, -26}, {0, -26},
      {0, -26}, {0, -26}, {26, -26}, {26, 0}, {0, 0}}},
    // moveBox-9
    {{{-26, 0}, {-26, 0}, {0, 0}, {-26, 0}, {0, 0}, {0, 0},
      {-26, 0}, {-26, 0}, {-52, 0}, {-26, 0}, {0, 0}}},
}};

}  // namespace

BanterLoaderWidget::BanterLoaderWidget(QWidget* parent)
    : QWidget(parent) {
    setObjectName("BanterLoader");
    setFixedSize(72, 72);
    setAttribute(Qt::WA_StyledBackground, true);
    buildUi();
    buildAnimations();
    applyBoxStyles();
}

void BanterLoaderWidget::setDarkMode(bool dark_mode) {
    if (dark_mode_ == dark_mode) {
        return;
    }
    dark_mode_ = dark_mode;
    applyBoxStyles();
}

void BanterLoaderWidget::start() {
    if (!animation_group_) {
        return;
    }
    if (running_) {
        return;
    }
    animation_group_->start();
    running_ = true;
}

void BanterLoaderWidget::stop() {
    if (!animation_group_) {
        return;
    }
    animation_group_->stop();
    for (int i = 0; i < static_cast<int>(boxes_.size()); ++i) {
        if (boxes_[i]) {
            boxes_[i]->move(baseVisiblePositionForIndex(i));
        }
    }
    running_ = false;
}

void BanterLoaderWidget::buildUi() {
    for (int i = 0; i < static_cast<int>(boxes_.size()); ++i) {
        auto* box = new QWidget(this);
        box->setObjectName(QString("BanterLoaderBox%1").arg(i + 1));
        box->setFixedSize(20, 20);
        box->move(baseVisiblePositionForIndex(i));
        box->show();
        boxes_[i] = box;
    }
}

void BanterLoaderWidget::buildAnimations() {
    animation_group_ = new QParallelAnimationGroup(this);

    for (int i = 0; i < static_cast<int>(boxes_.size()); ++i) {
        auto* box = boxes_[i];
        if (!box) {
            continue;
        }

        const QPoint base = baseVisiblePositionForIndex(i);
        auto* anim = new QPropertyAnimation(box, "pos", animation_group_);
        anim->setDuration(4000);
        anim->setLoopCount(-1);
        anim->setStartValue(base);
        for (int step = 0; step < static_cast<int>(kPercentSteps.size()); ++step) {
            anim->setKeyValueAt(kPercentSteps[step], base + kTranslations[i][step]);
        }
        animation_group_->addAnimation(anim);
    }
}

void BanterLoaderWidget::applyBoxStyles() {
    const QString fill = dark_mode_ ? QStringLiteral("#FFFFFF") : QStringLiteral("#93C5FD");
    const QString style =
        QString("background: %1; border-radius: 2px;").arg(fill);
    for (auto* box : boxes_) {
        if (box) {
            box->setStyleSheet(style);
        }
    }
}

QPoint BanterLoaderWidget::baseVisiblePositionForIndex(int index) const {
    const int row = index / 3;
    const int col = index % 3;
    QPoint pos(col * 26, row * 26);

    // Match CSS pseudo-element offsets:
    // 1st and 4th boxes shifted right by one step, 3rd shifted down by two steps.
    if (index == 0 || index == 3) {
        pos.rx() += 26;
    } else if (index == 2) {
        pos.ry() += 52;
    }

    return pos;
}

}  // namespace f2c_cpp
