#include "pan_zoom_image.hpp"

#include "satellite_palette.hpp"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace f2c_cpp {

namespace {

// Correspondences are identified by colour as well as number so the operator
// can match a pair across the two panes at a glance.
const QColor kMarkerColors[] = {
    QColor("#00d492"), QColor("#38bdf8"), QColor("#fbbf24"),
    QColor("#f472b6"), QColor("#a78bfa"), QColor("#fb923c"),
    QColor("#34d399"), QColor("#f87171"),
};

QColor markerColor(int number) {
    const int idx = std::max(0, number - 1) % 8;
    return kMarkerColors[idx];
}

}  // namespace

PanZoomImageWidget::PanZoomImageWidget(QWidget* parent) : QWidget(parent) {
    setObjectName("PanZoomImage");
    setMouseTracking(true);
    setMinimumSize(240, 180);
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::OpenHandCursor);
}

void PanZoomImageWidget::setImage(const QImage& image) {
    // A same-size replacement is the same picture recolored — the light-mode
    // point-cloud tint. The operator's pan and zoom have to survive it: they
    // may be a third of the way through picking correspondences when the
    // theme flips, and re-fitting would throw their framing away.
    const bool same_frame =
        !image_.isNull() && !image.isNull() && image_.size() == image.size();
    image_ = image;
    if (same_frame && view_fitted_) {
        update();
        return;
    }
    user_adjusted_ = false;
    view_fitted_ = false;
    fitToView();
    update();
}

void PanZoomImageWidget::setPickEnabled(bool enabled) {
    pick_enabled_ = enabled;
    setCursor(enabled ? Qt::CrossCursor : Qt::OpenHandCursor);
    update();
}

void PanZoomImageWidget::setDimmed(bool dimmed) {
    dimmed_ = dimmed;
    update();
}

void PanZoomImageWidget::setTurn(Turn turn, const QColor& accent,
                                 const QString& hint) {
    const bool became_active = turn == Turn::Active && turn_ != Turn::Active;
    turn_ = turn;
    turn_accent_ = accent;
    turn_hint_ = hint;
    if (became_active) {
        // Hand-off pulse: the ring flashes bright then settles, so the eye
        // is pulled to the pane that just became clickable.
        if (!turn_anim_) {
            turn_anim_ = new QVariantAnimation(this);
            turn_anim_->setDuration(700);
            turn_anim_->setStartValue(0.0);
            turn_anim_->setEndValue(1.0);
            connect(turn_anim_, &QVariantAnimation::valueChanged, this,
                    [this](const QVariant& v) {
                        // Two pulses: alpha rides |sin| so it peaks twice
                        // before resting at the steady ring.
                        const double t = v.toDouble();
                        turn_pulse_ =
                            t >= 1.0 ? 1.0
                                     : 0.45 + 0.55 * std::abs(std::sin(t * M_PI * 2.0));
                        update();
                    });
        }
        turn_anim_->stop();
        turn_anim_->start();
    } else if (turn != Turn::Active) {
        if (turn_anim_) {
            turn_anim_->stop();
        }
        turn_pulse_ = 1.0;
    }
    update();
}

void PanZoomImageWidget::setSmoothScaling(bool enabled) {
    smooth_scaling_ = enabled;
    update();
}

void PanZoomImageWidget::setStatusText(const QString& text) {
    status_text_ = text;
    update();
}

void PanZoomImageWidget::setCornerTag(const QString& tag) {
    corner_tag_ = tag;
    update();
}

void PanZoomImageWidget::setEmptyText(const QString& text) {
    empty_text_ = text;
    update();
}

void PanZoomImageWidget::setDarkMode(bool dark_mode) {
    if (dark_mode_ == dark_mode) {
        return;
    }
    dark_mode_ = dark_mode;
    update();
}

void PanZoomImageWidget::setMarkers(const QVector<QPointF>& image_points,
                                    const QVector<int>& numbers) {
    markers_ = image_points;
    marker_numbers_ = numbers;
    update();
}

void PanZoomImageWidget::setPendingMarker(const QPointF& image_pt,
                                          bool visible) {
    pending_marker_ = image_pt;
    pending_visible_ = visible;
    update();
}

void PanZoomImageWidget::setRobotPose(const QPointF& origin_px,
                                      double heading_rad, bool visible) {
    robot_origin_ = origin_px;
    robot_heading_rad_ = heading_rad;
    robot_visible_ = visible;
    update();
}

void PanZoomImageWidget::clearOverlays() {
    markers_.clear();
    marker_numbers_.clear();
    pending_visible_ = false;
    robot_visible_ = false;
    status_text_.clear();
    update();
}

QRectF PanZoomImageWidget::viewRect() const {
    return QRectF(0, 0, std::max(1, width()), std::max(1, height()));
}

void PanZoomImageWidget::fitToView() {
    user_adjusted_ = false;
    if (image_.isNull()) {
        return;
    }
    const QRectF view = viewRect().adjusted(8, 8, -8, -8);
    if (view.width() < 2 || view.height() < 2) {
        view_fitted_ = false;
        return;
    }
    const double sx = view.width() / std::max(1, image_.width());
    const double sy = view.height() / std::max(1, image_.height());
    scale_ = std::min(sx, sy);
    offset_ = QPointF(view.center().x() - 0.5 * image_.width() * scale_,
                      view.center().y() - 0.5 * image_.height() * scale_);
    view_fitted_ = true;
}

QPointF PanZoomImageWidget::imageToScreen(const QPointF& image_pt) const {
    return QPointF(offset_.x() + image_pt.x() * scale_,
                   offset_.y() + image_pt.y() * scale_);
}

QPointF PanZoomImageWidget::screenToImage(const QPointF& screen_pt) const {
    if (scale_ < 1e-12) {
        return QPointF();
    }
    return QPointF((screen_pt.x() - offset_.x()) / scale_,
                   (screen_pt.y() - offset_.y()) / scale_);
}

void PanZoomImageWidget::zoomAt(const QPointF& screen_pos, double factor) {
    const QPointF img = screenToImage(screen_pos);
    scale_ = std::clamp(scale_ * factor, 0.02, 40.0);
    offset_ = QPointF(screen_pos.x() - img.x() * scale_,
                      screen_pos.y() - img.y() * scale_);
}

void PanZoomImageWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth_scaling_);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Matte around the image. Stays a shade off the surface so the image's
    // own edge is readable when it does not fill the pane.
    painter.fillRect(rect(), dark_mode_ ? QColor(0x0b, 0x0b, 0x0b)
                                        : QColor(0xE4, 0xE4, 0xE7));

    if (!view_fitted_ && !image_.isNull()) {
        fitToView();
    }

    if (!image_.isNull()) {
        const QRectF dest(
            offset_,
            QSizeF(image_.width() * scale_, image_.height() * scale_));
        painter.drawImage(dest, image_);
    } else if (!empty_text_.isEmpty()) {
        painter.setPen(QColor(appThemeTokens().muted));
        painter.setFont(QFont(QStringLiteral("Arimo"), 11));
        painter.drawText(rect().adjusted(48, 0, -48, 0),
                         Qt::AlignCenter | Qt::TextWordWrap, empty_text_);
    }

    auto drawMarker = [&](const QPointF& img_pt, int number, bool pending) {
        const QPointF sp = imageToScreen(img_pt);
        const QColor color = markerColor(number);
        painter.setPen(QPen(color, pending ? 2.0 : 2.4));
        painter.setBrush(
            pending ? QColor(color.red(), color.green(), color.blue(), 40)
                    : QColor(color.red(), color.green(), color.blue(), 180));
        painter.drawEllipse(sp, 10, 10);
        painter.setPen(Qt::white);
        painter.setFont(QFont(QStringLiteral("Arimo"), 9, QFont::Bold));
        painter.drawText(QRectF(sp.x() - 10, sp.y() - 10, 20, 20),
                         Qt::AlignCenter, QString::number(number));
    };

    for (int i = 0; i < markers_.size(); ++i) {
        const int number =
            (i < marker_numbers_.size()) ? marker_numbers_[i] : (i + 1);
        drawMarker(markers_[i], number, false);
    }
    if (pending_visible_) {
        drawMarker(pending_marker_, markers_.size() + 1, true);
    }

    if (robot_visible_) {
        const QPointF origin = imageToScreen(robot_origin_);
        const double c = std::cos(robot_heading_rad_);
        const double s = std::sin(robot_heading_rad_);
        auto rot = [&](double x, double y) {
            return origin + QPointF(c * x - s * y, s * x + c * y);
        };
        QPolygonF body;
        body << rot(22, 0) << rot(-12, 12) << rot(-12, -12);
        painter.setPen(QPen(satpal::accent(), 2));
        painter.setBrush(QColor(0, 179, 90, 200));
        painter.drawPolygon(body);
        painter.setPen(QPen(satpal::text(), 2));
        painter.drawLine(origin, rot(36, 0));
        painter.setPen(satpal::accent());
        painter.setFont(QFont(QStringLiteral("Arimo"), 8, QFont::Bold));
        painter.drawText(origin + QPointF(10, -18),
                         QStringLiteral("Robot origin"));
        painter.setFont(QFont(QStringLiteral("Arimo"), 8));
        painter.drawText(origin + QPointF(10, -6), QStringLiteral("+X"));
    }

    if (dimmed_ || turn_ == Turn::Waiting) {
        // Waiting is a hard veil: the pane must read as "not now" from
        // across the roof, not as a slightly darker image. The veil takes
        // the theme's own direction — a near-black pane beside a light UI
        // reads as a dead feed rather than an inactive one.
        const int alpha = turn_ == Turn::Waiting ? 175 : 140;
        painter.fillRect(rect(), dark_mode_ ? QColor(0, 0, 0, alpha)
                                            : QColor(255, 255, 255, alpha));
    }

    if (turn_ == Turn::Active && turn_accent_.isValid()) {
        // Accent ring + inner glow around the whole pane. The glow is three
        // widening translucent strokes inside the ring.
        painter.setBrush(Qt::NoBrush);
        for (int i = 3; i >= 1; --i) {
            QColor glow = turn_accent_;
            glow.setAlphaF(0.10 * turn_pulse_ * (4 - i) / 3.0);
            painter.setPen(QPen(glow, 6.0 * i));
            painter.drawRect(rect().adjusted(3 * i, 3 * i, -3 * i, -3 * i));
        }
        QColor ring = turn_accent_;
        ring.setAlphaF(0.55 + 0.45 * turn_pulse_);
        painter.setPen(QPen(ring, 3.0));
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }

    if (!corner_tag_.isEmpty()) {
        // Figma 235:2407: 12px mono, 10/4 padding, chip on a hairline,
        // 12 px in and 8 px down from the corner. The frame's plate colors
        // are the dark-mode end of satpal's theme-following neutrals.
        QFont tag_font(QStringLiteral("Liberation Mono"));
        tag_font.setPixelSize(12);
        painter.setFont(tag_font);
        const QFontMetrics fm(tag_font);
        const QRectF chip(12, 8, fm.horizontalAdvance(corner_tag_) + 20,
                          fm.height() + 8);
        painter.setPen(QPen(satpal::border(), 1));
        painter.setBrush(satpal::chipBg());
        painter.drawRoundedRect(chip, 4, 4);
        painter.setPen(QColor(appThemeTokens().body));
        painter.drawText(chip, Qt::AlignCenter, corner_tag_);

        // Turn hint chip right after the tag: accent-filled when it is this
        // pane's turn, grey outline while waiting on the other pane.
        if (turn_ != Turn::None && !turn_hint_.isEmpty()) {
            QFont hint_font(QStringLiteral("Arimo"), 10, QFont::Bold);
            painter.setFont(hint_font);
            const QFontMetrics hfm(hint_font);
            const double hint_w = hfm.horizontalAdvance(turn_hint_) + 20;
            // Beside the tag when it fits, otherwise on its own row under
            // it — the narrow point-cloud pane cannot hold both in one line.
            const bool beside = chip.right() + 8 + hint_w <= width() - 12;
            const QRectF hint(beside ? chip.right() + 8 : chip.left(),
                              beside ? chip.top() : chip.bottom() + 6,
                              hint_w, chip.height());
            if (turn_ == Turn::Active) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(turn_accent_);
                painter.drawRoundedRect(hint, 4, 4);
                painter.setPen(QColor(0x0b, 0x0b, 0x0b));
            } else {
                painter.setPen(QPen(satpal::border(), 1));
                painter.setBrush(satpal::chipBg());
                painter.drawRoundedRect(hint, 4, 4);
                painter.setPen(QColor(appThemeTokens().muted));
            }
            painter.drawText(hint, Qt::AlignCenter, turn_hint_);
        }
    }

    if (!status_text_.isEmpty()) {
        // Turn prompt as a bottom-centre pill, clear of the corner tag and
        // of whatever the operator is about to click near the top.
        QFont status_font(QStringLiteral("Arimo"), 10, QFont::DemiBold);
        painter.setFont(status_font);
        const QFontMetrics fm(status_font);
        const double w = fm.horizontalAdvance(status_text_) + 28;
        const QRectF pill((width() - w) / 2.0, height() - 40, w, 28);
        painter.setPen(QPen(satpal::border(), 1));
        painter.setBrush(satpal::chipBg());
        painter.drawRoundedRect(pill, 14, 14);
        painter.setPen(satpal::text());
        painter.drawText(pill, Qt::AlignCenter, status_text_);
    }
}

void PanZoomImageWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!view_fitted_ || !user_adjusted_) {
        fitToView();
    }
}

void PanZoomImageWidget::mousePressEvent(QMouseEvent* event) {
    // Shift-drag pans even while picking is armed, so the operator can
    // reposition without burning a correspondence.
    if (event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton &&
         ((event->modifiers() & Qt::ShiftModifier) || !pick_enabled_))) {
        panning_ = true;
        user_adjusted_ = true;
        last_pan_pos_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && pick_enabled_ &&
        !image_.isNull() && !dimmed_ && turn_ != Turn::Waiting) {
        const QPointF img = screenToImage(event->pos());
        if (img.x() >= 0 && img.y() >= 0 && img.x() < image_.width() &&
            img.y() < image_.height()) {
            emit pointPicked(img);
        }
        event->accept();
    }
}

void PanZoomImageWidget::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        const QPoint delta = event->pos() - last_pan_pos_;
        offset_ += delta;
        last_pan_pos_ = event->pos();
        update();
        event->accept();
    }
}

void PanZoomImageWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (panning_ && (event->button() == Qt::MiddleButton ||
                     event->button() == Qt::LeftButton)) {
        panning_ = false;
        setCursor(pick_enabled_ ? Qt::CrossCursor : Qt::OpenHandCursor);
        event->accept();
    }
}

void PanZoomImageWidget::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / 120.0;
    if (std::abs(steps) < 1e-6 || image_.isNull()) {
        return;
    }
    user_adjusted_ = true;
    zoomAt(event->position(), std::pow(1.15, steps));
    update();
    event->accept();
}

}  // namespace f2c_cpp
