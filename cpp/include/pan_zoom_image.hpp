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

#include <QImage>
#include <QPointF>
#include <QVector>
#include <QWidget>

class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;

namespace f2c_cpp {

class PanZoomImageWidget : public QWidget {
    Q_OBJECT

public:
    explicit PanZoomImageWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    bool hasImage() const { return !image_.isNull(); }
    QSize imageSize() const { return image_.size(); }

    void setPickEnabled(bool enabled);
    /** Greys the pane out and refuses picks — used for "not your turn". */
    void setDimmed(bool dimmed);
    /** Off for the point-cloud raster: smoothing blurs sparse hits away. */
    void setSmoothScaling(bool enabled);
    void setStatusText(const QString& text);
    void setMarkers(const QVector<QPointF>& image_points,
                    const QVector<int>& numbers);
    void setPendingMarker(const QPointF& image_pt, bool visible);
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

private:
    QRectF viewRect() const;
    QPointF imageToScreen(const QPointF& image_pt) const;
    QPointF screenToImage(const QPointF& screen_pt) const;
    void zoomAt(const QPointF& screen_pos, double factor);

    QImage image_;
    double scale_ = 1.0;
    QPointF offset_;
    bool view_fitted_ = false;
    // Set once the operator pans or zooms. Until then the view re-fits on
    // every resize — setImage() usually runs before layout has given the
    // widget its real size, so a one-shot fit leaves the image tiny in the
    // corner of a pane that later grew.
    bool user_adjusted_ = false;

    bool pick_enabled_ = false;
    bool dimmed_ = false;
    bool smooth_scaling_ = true;
    QString status_text_;
    QVector<QPointF> markers_;
    QVector<int> marker_numbers_;
    QPointF pending_marker_;
    bool pending_visible_ = false;
    QPointF robot_origin_;
    double robot_heading_rad_ = 0.0;
    bool robot_visible_ = false;

    bool panning_ = false;
    QPoint last_pan_pos_;
};

}  // namespace f2c_cpp
