#include "components/auto_hide_scroll_bar.hpp"

#include <QAbstractScrollArea>
#include <QCursor>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QPoint>
#include <QPropertyAnimation>
#include <QRect>
#include <QScrollBar>
#include <QString>
#include <QStringLiteral>

namespace f2c_cpp {

namespace {
constexpr int kFadeDurationMs = 300;
constexpr int kScrollBarWidth = 8;
}  // namespace

void AutoHideScrollBar::install(QAbstractScrollArea* area, bool dark_mode) {
    if (!area) {
        return;
    }
    auto* helper = new AutoHideScrollBar(area);
    helper->setDarkMode(dark_mode);
}

AutoHideScrollBar::AutoHideScrollBar(QAbstractScrollArea* area)
    : QObject(area), area_(area) {
    if (!area) {
        return;
    }

    QScrollBar* bar = area->verticalScrollBar();
    if (!bar) {
        return;
    }

    opacity_effect_ = new QGraphicsOpacityEffect(bar);
    opacity_effect_->setOpacity(0.0);
    bar->setGraphicsEffect(opacity_effect_);

    animation_ = new QPropertyAnimation(opacity_effect_, "opacity", this);
    animation_->setDuration(kFadeDurationMs);
    animation_->setEasingCurve(QEasingCurve::OutCubic);

    area->installEventFilter(this);
    if (area->viewport()) {
        area->viewport()->installEventFilter(this);
    }
    bar->installEventFilter(this);
}

void AutoHideScrollBar::setDarkMode(bool dark_mode) {
    dark_mode_ = dark_mode;
    applyStyleSheet();
}

void AutoHideScrollBar::applyStyleSheet() {
    if (!area_) {
        return;
    }
    QScrollBar* bar = area_->verticalScrollBar();
    if (!bar) {
        return;
    }

    const QString track_hover =
        dark_mode_ ? QStringLiteral("#27272A") : QStringLiteral("#F4F4F5");
    const QString thumb_resting = QStringLiteral("#71717A");
    const QString thumb_pressed = QStringLiteral("#F59E0B");

    const QString style = QStringLiteral(
        "QScrollBar:vertical {"
        "  background: transparent;"
        "  width: %1px;"
        "  margin: 0;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: %2;"
        "  border-radius: 4px;"
        "  min-height: 20px;"
        "}"
        "QScrollBar::handle:vertical:pressed {"
        "  background: %3;"
        "}"
        "QScrollBar:vertical:hover {"
        "  background: %4;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "  height: 0; background: transparent;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "  background: transparent;"
        "}"
        "QScrollBar:horizontal { height: 0; background: transparent; }"
        ).arg(QString::number(kScrollBarWidth), thumb_resting, thumb_pressed,
              track_hover);

    bar->setStyleSheet(style);
}

void AutoHideScrollBar::fade(double target) {
    if (!opacity_effect_ || !animation_) {
        return;
    }
    if (animation_->state() == QAbstractAnimation::Running) {
        animation_->stop();
    }
    animation_->setStartValue(opacity_effect_->opacity());
    animation_->setEndValue(target);
    animation_->start();
}

bool AutoHideScrollBar::eventFilter(QObject* watched, QEvent* event) {
    Q_UNUSED(watched);
    if (!area_) {
        return false;
    }

    const QEvent::Type type = event->type();
    if (type == QEvent::Enter) {
        fade(1.0);
        return false;
    }

    if (type == QEvent::Leave) {
        // Keep visible if user is mid-drag on the slider.
        QScrollBar* bar = area_->verticalScrollBar();
        if (bar && bar->isSliderDown()) {
            return false;
        }

        // Cursor may bounce between area, viewport, and scrollbar without
        // actually leaving the scroll area's geometry. Only fade out when
        // the cursor is outside the area entirely.
        const QPoint global = QCursor::pos();
        const QPoint local = area_->mapFromGlobal(global);
        if (area_->rect().contains(local)) {
            return false;
        }

        fade(0.0);
        return false;
    }

    return false;
}

}  // namespace f2c_cpp
