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

class QLineEdit;

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
    /** One level about the view centre (the wheel zooms about the cursor). */
    void zoomIn();
    void zoomOut();
    /**
     * Frames the ROI (polygon, else rectangle) at the largest zoom that
     * fits it inside the viewport with `margin_px` to spare. Returns false
     * and leaves the view alone when there is no ROI.
     */
    bool fitToRoi(int margin_px = 56);

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

    RoiPolygon polygon() const { return polygon_; }
    void setPolygon(const RoiPolygon& poly);
    /** Next left-clicks append polygon vertices; right-click / Finish closes. */
    void armPolygonDraw();
    /**
     * Next press-drag-release draws a north-up rectangle whose diagonal is
     * the drag. Produces the same four-vertex RoiPolygon the polygon tool
     * would, so everything downstream (chips, pins, roof edges, Send) is
     * indifferent to which tool drew it.
     */
    void armRectangleDraw();
    /** Drops any armed draw / placement / ruler without touching the plan. */
    void cancelInteraction();
    bool isDrawing() const { return draw_polygon_armed_ || draw_rect_armed_; }
    bool isPlacingMarker() const { return place_marker_armed_; }
    void clearPolygon();
    /**
     * Ground-space summary of the ROI: its centroid and the radius of the
     * smallest centroid-centred circle that contains every vertex. This is
     * what sizes the imagery prefetch. False when there is no ROI.
     */
    bool roiExtent(geo::GeoPoint* centroid, double* radius_m) const;

    /**
     * Two-click ruler. First click anchors, the line follows the cursor,
     * second click fixes it, a third starts over. Right-click or Escape
     * clears. Purely visual — nothing in the plan changes.
     */
    void startMeasure();
    void clearMeasure();
    bool isMeasuring() const { return measure_state_ != Measure::Off; }

    /**
     * Selection gates the rotate handle: an unselected marker only ever
     * translates, so a careless drag near the arrow cannot spin the robot's
     * heading. Click the marker to select, click anywhere else to clear.
     */
    bool markerSelected() const { return marker_selected_; }
    void setMarkerSelected(bool selected);

    /**
     * Slides the edge's far vertex so the edge measures `meters`. With
     * `pin`, the length is also recorded as a constraint that later vertex
     * drags must honour.
     */
    bool setEdgeLength(int edge, double meters, bool pin = false);
    /** Releases a pinned edge length. */
    void clearEdgeLock(int edge);
    /** Current edge lengths in metres, edge i = vertex i -> i+1. Empty
        when there is no ROI. Drives the step-3 rail's Edge Dimensions list. */
    QVector<double> edgeLengthsM() const;
    /** Opens the inline editor over `edge`'s dimension chip (the rail's
        value buttons call this so the chip and the row edit together). */
    void beginEdgeLengthEdit(int edge);
    /** Paints `edge`'s chip in the green editing look without opening the
        editor — rail row hover. -1 clears. */
    void setHighlightedEdge(int edge);
    /** Step 1 shows imagery only: skips the ROI + marker paint and ignores
        their hit-testing. Drawing / measuring are unaffected. */
    void setOverlaysHidden(bool hidden);
    bool overlaysHidden() const { return overlays_hidden_; }

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

    /**
     * Backdrop for the measured canvas: a top-down raster of the collected
     * robot map, positioned by its extent in robot_init metres. The measured
     * grid origin IS robot_init (0,0), so world x maps to grid east and world
     * y to grid north with no extra bookkeeping. Ignored on the imagery
     * canvas, where the satellite tiles are the backdrop.
     */
    void setMapRaster(const QImage& image, const QRectF& bounds_m);
    void clearMapRaster();

    static constexpr int kMinZoom = 3;
    /** Highest level tiles are ever requested at (Esri's native ceiling). */
    static constexpr int kMaxZoom = 20;
    /** The grid canvas has no imagery-resolution ceiling — allow zooming to
        centimeter scale for small roofs. */
    static constexpr int kMaxZoomGrid = 26;
    /**
     * Levels the imagery canvas may zoom PAST the fetch ceiling, painting
     * the deepest available tiles scaled up. A 16 m roof at z19 is ~70 px
     * wide, too small to place a vertex on; three extra levels make it
     * ~550 px. The imagery goes soft, the handles and chips do not.
     */
    static constexpr int kOverzoomLevels = 3;
    /** View ceiling: what the wheel, zoomIn and fitToRoi clamp to. */
    int maxZoomNow() const;
    /**
     * View floor. Imagery keeps `kMinZoom`. The measured canvas cannot
     * zoom out past the collected map's hull plus an offset (15 m or 25 %
     * of the larger side), so the operator stays on the roof instead of
     * the empty grid. Empty-map fallback is a 100 m disc.
     */
    int minZoomNow() const;
    /** Fetch ceiling: the cached / native limit tiles are requested at. */
    int fetchZoomCeiling() const;

signals:
    void viewChanged(double lat, double lon, int zoom);
    void roiChanged();
    void markerChanged();
    /** Inline edge editor opened on `edge` (>= 0) or closed (-1). */
    void edgeEditChanged(int edge);
    /** An armed draw / placement / ruler started or ended. Lets the rail
        relabel its tool buttons ("Drawing…") without polling. */
    void interactionChanged();
    /** A Draw ROI gesture produced a valid shape (rectangle released or
        polygon closed). Emitted after roiChanged(); the screen frames it. */
    void drawFinished();
    void markerSelectionChanged(bool selected);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    /** Escape / focus-out handling for the inline dimension editor. */
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Drag {
        None,
        Pan,
        MoveRoi,
        ResizeRoiCorner,
        RotateRoi,
        MoveMarker,
        RotateMarker,
        MoveVertex,
        EdgeTogglePending,
        DimBadge,
        DrawRect,
    };

    enum class Measure { Off, WantFirst, WantSecond, Fixed };

    /** Zoom about the view centre, clamped to the current ceiling. */
    void zoomBy(int delta);
    /** Turns a diagonal into the north-up four-gon it spans. */
    void applyRectangleFromDiagonal(const geo::GeoPoint& a,
                                    const geo::GeoPoint& b);
    /** In-progress polygon / rectangle / ruler, drawn above the plan. */
    void paintInteraction(QPainter& painter);

    // Coordinate helpers (valid during paint/mouse handling).
    QPointF screenFromNorm(double nx, double ny) const;
    QPointF screenFromGeo(const geo::GeoPoint& point) const;
    geo::GeoPoint geoFromScreen(const QPointF& pos) const;
    QPointF screenFromBody(const QPointF& body) const;
    double metersPerPixelNow() const;
    void paintMapRaster(QPainter& painter);

    // Overlay geometry in screen space.
    QVector<QPointF> roiCornerScreenPoints() const;
    QVector<QPointF> polygonScreenPoints() const;
    QPointF roiRotateHandleScreen() const;
    QPointF markerScreenPos() const;
    QPointF markerArrowTipScreen() const;

    /**
     * Moves `vertex` to `desired` and relaxes the rest of the ring so every
     * pinned edge keeps its pinned length. Returns false if the pins cannot
     * be satisfied, in which case the drag is ignored.
     */
    bool dragVertexWithLocks(int vertex, const geo::GeoPoint& desired);

    /**
     * Relaxes `verts` until every edge with a pinned length in `locks` is at
     * that length, holding vertex `held` exactly where it is. Returns false
     * if the pins are geometrically unsatisfiable.
     */
    bool solveEdgeLocks(int held, const QVector<double>& locks,
                        QVector<geo::GeoPoint>& verts) const;

    void commitEdgeLengthEdit();
    void cancelEdgeLengthEdit();

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
    RoiPolygon polygon_;
    geo::GeoPose marker_;
    bool edit_locked_ = false;
    bool overlays_hidden_ = false;  // step 1: imagery only, no ROI / marker
    bool place_marker_armed_ = false;
    bool draw_polygon_armed_ = false;
    bool draw_rect_armed_ = false;
    bool marker_selected_ = false;
    geo::GeoPoint rect_anchor_;      // first corner of an in-progress rectangle
    QPointF hover_pos_;              // last cursor position, for previews
    bool hover_valid_ = false;
    Measure measure_state_ = Measure::Off;
    geo::GeoPoint measure_a_;
    geo::GeoPoint measure_b_;
    int drag_dim_edge_ = -1;
    QVector<QRectF> dim_boxes_;

    // Inline dimension editing. One reusable QLineEdit overlaid on the chip
    // rect, rather than one per edge: only one can be active, and the vertex
    // count changes as the operator edits the polygon.
    QLineEdit* dim_edit_ = nullptr;
    int dim_edit_edge_ = -1;
    int dim_hover_edge_ = -1;

    geo::GeoPose mission_anchor_;
    GridSnapshot grid_;
    PolylineSet path_;
    PolylineSet swaths_;
    OdomSnapshot odom_;
    QVector<QPointF> trail_;  // body-frame breadcrumbs

    QImage map_raster_;      // collected robot map, top-down
    QRectF map_raster_m_;    // its extent in robot_init metres

    Drag drag_ = Drag::None;
    int drag_corner_ = -1;
    int drag_edge_ = -1;
    QPoint drag_press_pos_;
    QPoint drag_last_;
};

}  // namespace f2c_cpp
