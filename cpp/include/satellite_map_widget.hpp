/**
 * @file satellite_map_widget.hpp
 * @brief Slippy-map widget: Esri imagery + geo-anchored mission overlays.
 *
 * Overlays (all stored in ground/geo coordinates, never pixels, so they stay
 * glued to the imagery at every zoom):
 *  - ROI rectangle (measured meters, rotatable, corner-resizable)
 *  - robot placement marker (position + compass heading, the Send anchor)
 *  - live mission telemetry (occupancy grid, planned path, swaths, odometry
 *    trail) rendered in the frozen mission anchor frame after Send.
 */

#pragma once

#include "satellite_geo_math.hpp"
#include "satellite_job_model.hpp"
#include "satellite_ros_link.hpp"

#include <QWidget>

namespace f2c_cpp {

class TileService;

class SatelliteMapWidget : public QWidget {
    Q_OBJECT

public:
    explicit SatelliteMapWidget(TileService* tiles, QWidget* parent = nullptr);

    void setView(double lat, double lon, int zoom);
    double centerLat() const;
    double centerLon() const;
    int zoom() const { return zoom_; }

    /**
     * Measured (grid) mode: disables tile fetching/painting and the Esri
     * attribution — the canvas becomes a plain metric surface. ROI/marker
     * editing, overlays, and the export math are unaffected (they operate
     * on ground meters either way).
     */
    void setImageryEnabled(bool enabled);

    // ---- Plan objects ----
    RoiRect roi() const { return roi_; }
    void setRoi(const RoiRect& roi);
    /** Creates/re-centers the ROI in the current view. */
    void addRoiAtViewCenter();

    geo::GeoPose marker() const { return marker_; }
    void setMarker(const geo::GeoPose& marker);
    /** The next left click places the robot marker. */
    void armMarkerPlacement();

    /** Mission lock: freezes ROI/marker editing while a mission is active. */
    void setEditLocked(bool locked);
    bool editLocked() const { return edit_locked_; }

    // ---- Live telemetry (anchor = marker pose snapshot at Send) ----
    void setMissionAnchor(const geo::GeoPose& anchor);
    void clearMissionAnchor();
    void setGrid(const GridSnapshot& grid);
    void setPath(const PolylineSet& path);
    void setSwaths(const PolylineSet& swaths);
    void setOdom(const OdomSnapshot& odom);
    void clearTelemetry();

    static constexpr int kMinZoom = 3;
    static constexpr int kMaxZoom = 20;
    /** The grid canvas has no imagery-resolution ceiling — allow zooming to
        centimeter scale for small roofs. */
    static constexpr int kMaxZoomGrid = 23;
    int maxZoomNow() const { return imagery_enabled_ ? kMaxZoom : kMaxZoomGrid; }

signals:
    void viewChanged(double lat, double lon, int zoom);
    void roiChanged();
    void markerChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    enum class Drag {
        None,
        Pan,
        MoveRoi,
        ResizeRoiCorner,
        RotateRoi,
        MoveMarker,
        RotateMarker,
        EdgeTogglePending,  // pressed on an ROI edge; toggles on release
    };

    // Coordinate helpers (valid during paint/mouse handling).
    QPointF screenFromNorm(double nx, double ny) const;
    QPointF screenFromGeo(const geo::GeoPoint& point) const;
    geo::GeoPoint geoFromScreen(const QPointF& pos) const;
    QPointF screenFromBody(const QPointF& body) const;
    double metersPerPixelNow() const;

    // Overlay geometry in screen space.
    QVector<QPointF> roiCornerScreenPoints() const;
    QPointF roiRotateHandleScreen() const;
    QPointF markerScreenPos() const;
    QPointF markerArrowTipScreen() const;

    Drag hitTest(const QPointF& pos, int* corner_index,
                 int* edge_index = nullptr) const;
    void updateCursorShape(const QPointF& pos);
    /** 0.1 m position snapping — measured (grid) canvas only. */
    geo::GeoPoint maybeSnap(const geo::GeoPoint& point) const;

    void paintTiles(QPainter& painter);
    void paintGrid(QPainter& painter);
    void paintTelemetry(QPainter& painter);
    void paintRoi(QPainter& painter);
    void paintMarker(QPainter& painter);
    void paintChrome(QPainter& painter);

    void clampCenter();
    void emitViewChanged();

    TileService* tiles_;
    bool imagery_enabled_ = true;
    double center_nx_ = 0.5;
    double center_ny_ = 0.5;
    int zoom_ = 5;

    RoiRect roi_;
    geo::GeoPose marker_;
    bool edit_locked_ = false;
    bool place_marker_armed_ = false;

    geo::GeoPose mission_anchor_;
    GridSnapshot grid_;
    PolylineSet path_;
    PolylineSet swaths_;
    OdomSnapshot odom_;
    QVector<QPointF> trail_;  // body-frame breadcrumbs

    Drag drag_ = Drag::None;
    int drag_corner_ = -1;
    int drag_edge_ = -1;
    QPoint drag_press_pos_;
    QPoint drag_last_;
};

}  // namespace f2c_cpp
