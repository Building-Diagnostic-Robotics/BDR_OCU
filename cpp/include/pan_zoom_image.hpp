/**
 * @file pan_zoom_image.hpp
 * @brief Pan/zoom image canvas with optional sequential point picking.
 *
 * Used by the Stage 6 alignment flow: one instance shows the stitched
 * satellite site image, another the top-down point-cloud raster, and the
 * operator alternates clicks between them to build correspondences. Points
 * are emitted in IMAGE pixel coordinates, so callers never deal with the
 * pan/zoom transform themselves.
 */

#pragma once

#include "touch_canvas_gestures.hpp"

#include <QColor>
#include <QImage>
#include <QLineF>
#include <QPointF>
#include <QVector>
#include <QWidget>

#include <QTransform>

class QMouseEvent;
class QPushButton;
class QVariantAnimation;
class QPaintEvent;
class QResizeEvent;
class QTouchEvent;
class QWheelEvent;

namespace f2c_cpp {

class PanZoomImageWidget : public QWidget {
    Q_OBJECT

public:
    explicit PanZoomImageWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    bool hasImage() const { return !image_.isNull(); }
    QSize imageSize() const { return image_.size(); }
    /**
     * Screen pixels per image pixel, as currently displayed. Read by the
     * alignment pick-precision model: a pick made zoomed out is worth less
     * than one made zoomed in, because the operator could not have placed it
     * as finely. See SatelliteScreen::pairSigmaM.
     */
    double scale() const { return scale_; }

    /**
     * View rotation in degrees, driven by a two-finger twist. The cloud and
     * the stitch arrive at different orientations, so being able to spin one
     * to roughly match the other is what makes the same building corner
     * findable in both panes.
     *
     * Purely a view property: `pointPicked` keeps emitting IMAGE pixels, so
     * the similarity fit and both pick-precision sigmas are untouched by it.
     */
    double bearingDeg() const { return bearing_deg_; }
    void setBearingDeg(double degrees);

    /** Image pixel currently under the centre of the pane. */
    QPointF viewCenterImagePt() const;
    /**
     * Put `image_center` at the pane centre at `scale` screen px per image px.
     * Used to carry the operator's framing across a re-projection, where the
     * image is replaced by a differently-sized canvas of the same ground.
     */
    void setView(const QPointF& image_center, double scale);

    void setPickEnabled(bool enabled);
    /** Greys the pane out and refuses picks — used for "not your turn". */
    void setDimmed(bool dimmed);
    /**
     * Whose turn it is in the alternating pick flow. `Active` draws an
     * accent ring + glow around the pane and an accent hint chip beside the
     * corner tag (with a short pulse on hand-off); `Waiting` dims the pane
     * hard and greys the hint. `None` clears both.
     */
    enum class Turn { None, Active, Waiting };
    void setTurn(Turn turn, const QColor& accent, const QString& hint);
    /** Off for the point-cloud raster: smoothing blurs sparse hits away. */
    void setSmoothScaling(bool enabled);
    /** Turn prompt, drawn as a pill along the bottom edge. */
    void setStatusText(const QString& text);
    /** Pane identity ("SATELLITE MAP"), a small mono tag in the top-left
        corner per the Stage 6 alignment frame. */
    void setCornerTag(const QString& tag);
    /** Shown centred when no image is loaded. */
    void setEmptyText(const QString& text);
    /**
     * Themes the pane's own chrome — backdrop, corner tag, hint and status
     * chips. The pane paints rather than styles, so a stylesheet cannot
     * reach it. The *image* is the caller's business: a satellite stitch is
     * a photograph and must not be recolored, while the point-cloud raster
     * has to be re-tinted, so `SatelliteScreen` owns that.
     */
    void setDarkMode(bool dark_mode);
    void setMarkers(const QVector<QPointF>& image_points,
                    const QVector<int>& numbers);
    void setPendingMarker(const QPointF& image_pt, bool visible);
    /**
     * Clicked-point -> fitted-point spurs, in image pixels. Each is one pair's
     * miss under the current fit, so the operator can see WHERE the alignment
     * disagrees rather than only that its RMSE is large.
     */
    void setResidualLines(const QVector<QLineF>& lines);
    /**
     * Marker number to ring as suspect, or -1 for none. Advisory only: the
     * pane draws attention to it and nothing is blocked or removed.
     */
    void setFlaggedMarker(int number);
    /** Robot glyph at an image pixel, arrow along `heading_rad`. */
    void setRobotPose(const QPointF& origin_px, double heading_rad,
                      bool visible);
    void clearOverlays();
    void fitToView();

signals:
    void pointPicked(QPointF image_pt);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    /** Touch pan / pinch. QTouchEvent has no dedicated virtual in Qt 5. */
    bool event(QEvent* event) override;

private:
    QRectF viewRect() const;
    /**
     * Image pixels -> screen: scale, translate by `offset_`, then rotate
     * about the pane centre. `offset_` therefore lives in the PRE-rotation
     * frame, which is why pan deltas and zoom anchors have to be un-rotated
     * before they can be solved against it.
     */
    QTransform viewTransform() const;
    /** Just the rotation half of viewTransform(), for turning a screen
        point or delta back into the frame `offset_` is expressed in. */
    QTransform rotationTransform() const;
    QPointF preRotationDelta(const QPointF& screen_delta) const;
    QPointF imageToScreen(const QPointF& image_pt) const;
    QPointF screenToImage(const QPointF& screen_pt) const;
    void zoomAt(const QPointF& screen_pos, double factor);
    /** Touch pan / pinch / twist. True when the event is consumed. */
    bool handleTouchGesture(QTouchEvent* event);
    /** Places and shows/hides the reset-rotation chip. */
    void layoutResetRotation();
    /** Re-sheets the chip for the current palette. */
    void applyResetRotationStyle();

    QImage image_;
    bool dark_mode_ = true;
    double scale_ = 1.0;
    QPointF offset_;
    // Degrees clockwise. A twist is easy to trigger by accident while
    // pinching, so the reset chip below is not optional.
    double bearing_deg_ = 0.0;
    QPushButton* reset_rotation_ = nullptr;
    bool view_fitted_ = false;
    // Set once the operator pans or zooms. Until then the view re-fits on
    // every resize — setImage() usually runs before layout has given the
    // widget its real size, so a one-shot fit leaves the image tiny in the
    // corner of a pane that later grew.
    bool user_adjusted_ = false;

    bool pick_enabled_ = false;
    bool dimmed_ = false;
    Turn turn_ = Turn::None;
    QColor turn_accent_;
    QString turn_hint_;
    double turn_pulse_ = 1.0;  // 0..1, ring alpha during the hand-off pulse
    QVariantAnimation* turn_anim_ = nullptr;
    bool smooth_scaling_ = true;
    QString status_text_;
    QString corner_tag_;
    QString empty_text_ = QStringLiteral("No image loaded");
    QVector<QPointF> markers_;
    QVector<int> marker_numbers_;
    QVector<QLineF> residual_lines_;
    int flagged_number_ = -1;
    QPointF pending_marker_;
    bool pending_visible_ = false;
    QPointF robot_origin_;
    double robot_heading_rad_ = 0.0;
    bool robot_visible_ = false;

    bool panning_ = false;
    QPoint last_pan_pos_;

    // Touch pan / pinch / twist. A finger never clicks this pane: a
    // misplaced correspondence silently biases the alignment fit, and a
    // fingertip cannot be aimed well enough to risk it. Fingers frame the
    // two panes, the trackpad picks. See touch_gesture_state.hpp.
    touch_gestures::GestureState touch_;
    touch::SynthesizedMouseGuard touch_guard_;
};

}  // namespace f2c_cpp
