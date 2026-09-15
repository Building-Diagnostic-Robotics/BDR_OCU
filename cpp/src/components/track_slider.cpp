#include "components/track_slider.hpp"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace f2c_cpp {

TrackSlider::TrackSlider(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kWidgetHeight);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
}

void TrackSlider::setRange(double minimum, double maximum) {
    minimum_ = minimum;
    maximum_ = std::max(minimum_, maximum);
    setValue(value_);
}

void TrackSlider::setStep(double step) { step_ = std::max(0.0, step); }

void TrackSlider::setDecimals(int decimals) { decimals_ = std::max(0, decimals); }

void TrackSlider::setDarkMode(bool dark_mode) {
    dark_mode_ = dark_mode;
    update();
}

void TrackSlider::setValue(double value) {
    const double snapped = snapValue(value);
    if (std::abs(value_ - snapped) < 1e-9) {
        return;
    }
    value_ = snapped;
    update();
}

void TrackSlider::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const qreal track_height = 8.0;
    const qreal track_radius = track_height / 2.0;
    const qreal track_left = kThumbRadius;
    const qreal track_right = width() - kThumbRadius;
    const qreal track_width = std::max<qreal>(0.0, track_right - track_left);
    const qreal track_y = (height() - track_height) / 2.0;
    const QRectF track_rect(track_left, track_y, track_width, track_height);

    const QColor track_unfilled = isEnabled()
                                      ? (dark_mode_ ? QColor(QStringLiteral("#3F3F47"))
                                                    : QColor(QStringLiteral("#D4D4D8")))
                                      : (dark_mode_ ? QColor(QStringLiteral("#27272A"))
                                                    : QColor(QStringLiteral("#E5E7EB")));
    const QColor accent = isEnabled()
                              ? (dark_mode_ ? QColor(QStringLiteral("#00BC7D"))
                                            : QColor(QStringLiteral("#009966")))
                              : (dark_mode_ ? QColor(QStringLiteral("#3F3F47"))
                                            : QColor(QStringLiteral("#A1A1AA")));

    painter.setPen(Qt::NoPen);
    painter.setBrush(track_unfilled);
    painter.drawRoundedRect(track_rect, track_radius, track_radius);

    const qreal handle_cx = trackValueX(value_, track_left, track_width);
    const qreal handle_cy = track_rect.center().y();

    const qreal filled_right = std::clamp<qreal>(handle_cx, track_left, track_right);
    if (filled_right > track_left) {
        QRectF filled_rect(track_left, track_y, filled_right - track_left, track_height);
        painter.setBrush(accent);
        painter.drawRoundedRect(filled_rect, track_radius, track_radius);
    }

    const qreal handle_radius = dragging_ ? kThumbRadius + 1.0 : kThumbRadius;
    const QPointF handle_center(handle_cx, handle_cy);

    const QColor shadow_color(0, 0, 0, dark_mode_ ? 90 : 60);
    painter.setBrush(shadow_color);
    painter.drawEllipse(handle_center + QPointF(0.0, 2.0),
                        handle_radius + 1.5, handle_radius + 1.5);

    painter.setBrush(accent);
    painter.drawEllipse(handle_center, handle_radius, handle_radius);
}

void TrackSlider::mousePressEvent(QMouseEvent* event) {
    if (!isEnabled() || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    updateFromX(event->pos().x());
    event->accept();
}

void TrackSlider::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ && isEnabled()) {
        updateFromX(event->pos().x());
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
    update();
}

void TrackSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && event->button() == Qt::LeftButton) {
        dragging_ = false;
        update();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TrackSlider::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    if (!dragging_) {
        update();
    }
}

double TrackSlider::clampValue(double value) const {
    return std::min(maximum_, std::max(minimum_, value));
}

double TrackSlider::snapValue(double value) const {
    const double clamped = clampValue(value);
    if (step_ <= 0.0 || maximum_ <= minimum_) {
        return clamped;
    }
    const double steps = std::round((clamped - minimum_) / step_);
    const double snapped = minimum_ + (steps * step_);
    const double factor = std::pow(10.0, decimals_);
    return std::round(clampValue(snapped) * factor) / factor;
}

qreal TrackSlider::trackValueX(double value, qreal track_left, qreal track_width) const {
    if (maximum_ <= minimum_ || track_width <= 0.0) {
        return track_left;
    }
    const double t = (value - minimum_) / (maximum_ - minimum_);
    return track_left + std::clamp<qreal>(t, 0.0, 1.0) * track_width;
}

void TrackSlider::updateFromX(double x) {
    const qreal track_left = kThumbRadius;
    const qreal track_right = width() - kThumbRadius;
    const qreal track_width = std::max<qreal>(0.0, track_right - track_left);
    if (maximum_ <= minimum_ || track_width <= 0.0) {
        return;
    }
    const qreal clamped = std::clamp<qreal>(x, track_left, track_right);
    const double t = (clamped - track_left) / track_width;
    const double next_value = snapValue(minimum_ + t * (maximum_ - minimum_));
    if (std::abs(next_value - value_) < 1e-9) {
        return;
    }
    value_ = next_value;
    if (on_value_changed) {
        on_value_changed(value_);
    }
    update();
}

}  // namespace f2c_cpp
