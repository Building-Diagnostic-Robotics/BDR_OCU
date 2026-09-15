/**
 * @file track_slider.hpp
 * @brief Painted range slider: 8 px track, filled accent to the thumb,
 *        step-snapped values with fixed decimals.
 *
 * Lifted verbatim from the legacy Stage 5 planner (`PlannerTrackSlider`)
 * so the Stage 6 ROI card can reuse the same control. Value changes from
 * the operator fire `on_value_changed`; programmatic `setValue` does not.
 */

#pragma once

#include <QWidget>

#include <functional>

class QMouseEvent;
class QPaintEvent;

namespace f2c_cpp {

class TrackSlider : public QWidget {
public:
    explicit TrackSlider(QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    void setStep(double step);
    double step() const { return step_; }
    void setDecimals(int decimals);
    void setDarkMode(bool dark_mode);

    /** Snaps to the step grid; silent (no callback). */
    void setValue(double value);
    double value() const { return value_; }
    double minimum() const { return minimum_; }
    double maximum() const { return maximum_; }

    std::function<void(double)> on_value_changed;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    static constexpr int kWidgetHeight = 28;
    static constexpr qreal kThumbRadius = 10.0;

    double clampValue(double value) const;
    double snapValue(double value) const;
    qreal trackValueX(double value, qreal track_left, qreal track_width) const;
    void updateFromX(double x);

    double minimum_ = 0.0;
    double maximum_ = 1.0;
    double value_ = 0.0;
    double step_ = 0.01;
    int decimals_ = 2;
    bool dragging_ = false;
    bool dark_mode_ = true;
};

}  // namespace f2c_cpp
