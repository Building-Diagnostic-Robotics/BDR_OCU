/**
 * @file touch_canvas_gestures.hpp
 * @brief Qt glue shared by the touch-enabled canvases.
 *
 * The gesture maths lives in touch_gesture_state.hpp (pure, tested). This
 * header carries the two pieces of Qt plumbing that would otherwise be
 * triplicated across SatelliteMapWidget, PlotWidget and PanZoomImageWidget:
 * reading the live touch points out of a QTouchEvent, and keeping the
 * platform's synthesized mouse events out.
 */

#pragma once

#include "touch_gesture_state.hpp"

#include <QElapsedTimer>
#include <QEvent>
#include <QMouseEvent>
#include <QPair>
#include <QPointF>
#include <QTouchEvent>
#include <QVector>

#include <algorithm>

namespace f2c_cpp {
namespace touch {

inline QPointF asQPointF(const touch_gestures::Point& point) {
    return QPointF(point.x, point.y);
}

inline bool isTouchEventType(QEvent::Type type) {
    return type == QEvent::TouchBegin || type == QEvent::TouchUpdate ||
           type == QEvent::TouchEnd || type == QEvent::TouchCancel;
}

/** Still-pressed points, ordered by touch id so a pinch's two fingers keep
    their slots from one event to the next — Qt guarantees no ordering. */
inline QVector<touch_gestures::Point> activePoints(const QTouchEvent* event) {
    QVector<QPair<int, QPointF>> raw;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    for (const QEventPoint& point : event->points()) {
        if (point.state() == QEventPoint::State::Released) {
            continue;
        }
        raw.append(qMakePair(point.id(), point.position()));
    }
#else
    for (const QTouchEvent::TouchPoint& point : event->touchPoints()) {
        if (point.state() == Qt::TouchPointReleased) {
            continue;
        }
        raw.append(qMakePair(point.id(), point.pos()));
    }
#endif
    std::sort(raw.begin(), raw.end(),
              [](const QPair<int, QPointF>& a, const QPair<int, QPointF>& b) {
                  return a.first < b.first;
              });
    QVector<touch_gestures::Point> points;
    points.reserve(raw.size());
    for (const QPair<int, QPointF>& entry : raw) {
        points.append(
            touch_gestures::Point{entry.second.x(), entry.second.y()});
    }
    return points;
}

/**
 * Keeps platform-synthesized mouse events out of a canvas that is handling
 * the touch itself.
 *
 * Three layers, because on this stack one is not enough. Accepting
 * TouchBegin stops Qt's own AA_SynthesizeMouseForUnhandledTouchEvents
 * synthesis; the source() test catches what Qt does tag; the tail window
 * catches the rest. Under XWayland the X server emulates a core pointer
 * from the first touch point (the "emulating" flag in `xinput test-xi2`)
 * and Qt 5.15's xcb plugin does not tag every one of those as
 * MouseEventSynthesizedBySystem, so without the window a pinch also lands
 * a click.
 */
class SynthesizedMouseGuard {
public:
    /** How long after the last finger lifts a mouse event is still assumed
        to be the platform's echo of it. */
    static constexpr int kTailMs = 100;

    void beginTouch() {
        touch_active_ = true;
        since_touch_.restart();
    }

    void endTouch() {
        touch_active_ = false;
        since_touch_.restart();
    }

    bool shouldIgnore(const QMouseEvent* event) const {
        if (event->source() == Qt::MouseEventSynthesizedBySystem) {
            return true;
        }
        if (touch_active_) {
            return true;
        }
        return since_touch_.isValid() && since_touch_.elapsed() < kTailMs;
    }

private:
    bool touch_active_ = false;
    QElapsedTimer since_touch_;
};

}  // namespace touch
}  // namespace f2c_cpp
