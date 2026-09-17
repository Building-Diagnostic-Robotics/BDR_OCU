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
#include "touch_canvas_gestures.hpp"

#include <QElapsedTimer>
#include <QTransform>
#include <QWidget>

class QLineEdit;
class QTouchEvent;

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
     * View bounds: the box the viewport may never leave. The zoom floor
     * becomes the smallest level that still fits inside it and the centre is
     * clamped so the view stays within it. Imagery canvas only — the
     * measured canvas already floors on the collected map's hull.
     *
     * Two callers: a cached site (centre + prefetch radius), so the operator
     * cannot zoom out past the tiles that are on disk, and the unanchored
     * fallback, so a stray gesture cannot leave them looking at an ocean.
     */
    void setViewBounds(const geo::GeoPoint& center, double radius_m);
    void setViewBoundsLatLon(double south, double west, double north,
                             double east);
    void clearViewBounds();

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
    /** Body-frame odom breadcrumbs kept for the trail (scan quality input). */
    const QVector<QPointF>& trail() const { return trail_; }

    /**
     * Backdrop for the measured canvas: a top-down raster of the collected
     * robot map, positioned by its extent in robot_init metres. The measured
     * grid origin IS robot_init (0,0), so world x maps to grid east and world
     * y to grid north with no extra bookkeeping. Ignored on the imagery
     * canvas, where the satellite tiles are the backdrop.
     */
    void setMapRaster(const QImage& image, const QRectF& bounds_m);
    void clearMapRaster();

    /**
     * The canvas paints rather than styles, so the theme cannot reach it
     * through a stylesheet — this stores the flag and repaints. It also
     * re-tints the cached point-cloud raster, whose points are drawn light
     * for a dark canvas and would vanish on a light one.
     */
    void setDarkMode(bool dark_mode);
    bool darkMode() const { return dark_mode_; }

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
     * View floor. Imagery floors on the current view bounds, or `kMinZoom`
     * when none are set. The measured canvas cannot zoom out past the
     * collected map's hull plus an offset (15 m or 25 % of the larger
     * side), so the operator stays on the roof instead of the empty grid.
     * Empty-map fallback is a 100 m disc.
     */
    int minZoomNow() const;
    /** Fetch ceiling: the cached / native limit tiles are requested at. */
    int fetchZoomCeiling() const;

    /**
     * Compass bearing shown at the top of the screen, degrees clockwise
     * from north. Two-finger twist drives it; the canvas compass resets it.
     *
     * This is a VIEW property and nothing else. `RoiPolygon` vertices,
     * `RoiRect::heading_deg` and the robot marker's heading stay
     * geographic, so no exported geometry may ever read it — rotating the
     * map must not rotate the roof. Forced to zero on the measured canvas,
     * whose metric grid and point-cloud raster are axis-aligned blits.
     */
    double bearingDeg() const { return bearing_deg_; }
    void setBearingDeg(double degrees);
    bool bearingSupported() const { return imagery_enabled_; }

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
    /** View bearing changed — lets the canvas compass re-point without
        polling. Fires on reset too. */
    void bearingChanged(double degrees);
    /** A finger tapped the canvas without panning it. Carries no position:
        a tap places nothing here (see touch_gesture_state.hpp), so the only
        legitimate consumer is a screen-level action that is indifferent to
        where it landed — currently the scan step's hand-back from manual
        override, which a mouse gets from any press on the map. */
    void canvasTapped();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    /** Touch pan / pinch. QTouchEvent has no dedicated virtual in Qt 5. */
    bool event(QEvent* event) override;
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
    /** Pans by a screen-pixel delta — trackpad scroll or finger travel. */
    void panByPixels(const QPointF& delta);
    /**
     * Moves `levels` zoom levels while holding the world point under
     * `anchor` fixed, clamped to the current floor / ceiling. Shared by
     * Ctrl+wheel (whole levels, unchanged feel) and pinch (fractional).
     * False when already at the limit.
     */
    bool zoomAtScreenPoint(double levels, const QPointF& anchor);
    /** Touch pan / pinch. True when the event is consumed. */
    bool handleTouchGesture(QTouchEvent* event);
    /** Re-clamps zoom and centre after the bounds change. */
    void applyViewBounds();
    /** Turns a diagonal into the north-up four-gon it spans. */
    void applyRectangleFromDiagonal(const geo::GeoPoint& a,
                                    const geo::GeoPoint& b);
    /** In-progress polygon / rectangle / ruler, drawn above the plan. */
    void paintInteraction(QPainter& painter);

    /**
     * World pixels across the whole normalized Mercator square. The one
     * place zoom becomes a scale, so the fractional part has a single
     * definition — every coordinate helper and the tile painter read it.
     */
    double worldPixels() const;
    /** Axis-aligned screen extent the viewport covers once the content is
        rotated under it — widget size at bearing 0, ~1.41x at 45 deg. */
    QSizeF rotatedViewportPx() const;
    /** Zoom as a real number; `zoom_` is its floor. */
    double continuousZoom() const { return double(zoom_) + zoom_frac_; }
    /**
     * Sets the continuous zoom, splitting it into the integer level tiles
     * are fetched at and the fraction they are painted scaled by. Clamped
     * against the level floor / ceiling as a real number: a pinch must not
     * be able to sit at 19.6 on a pyramid cached to 19, which would be
     * permanent blur with nothing on screen to explain it.
     */
    void setContinuousZoom(double zoom);
    /**
     * Normalized Mercator -> screen: translate by the view centre, scale by
     * worldPixels(), rotate by the bearing about the viewport centre.
     * `screenFromNorm` / `geoFromScreen` are this matrix and its inverse,
     * so the whole overlay layer inherits rotation for free.
     */
    QTransform viewTransform() const;
    /** A screen-pixel delta as a normalized-space delta — the inverse of
        the view's rotation and scale, with the translation divided out. */
    QPointF normDeltaFromScreen(const QPointF& delta) const;

    // Coordinate helpers (valid during paint/mouse handling).
    QPointF screenFromNorm(double nx, double ny) const;
    QPointF screenFromGeo(const geo::GeoPoint& point) const;
    geo::GeoPoint geoFromScreen(const QPointF& pos) const;
    QPointF screenFromBody(const QPointF& body) const;
    double metersPerPixelNow() const;
    void paintMapRaster(QPainter& painter);
    void refreshMapRasterTint();

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
    // Fraction of a level above zoom_, in [0, 1). Tiles are still selected
    // at zoom_ and are simply painted 2^zoom_frac_ wider, which is what
    // turns a pinch from a 2x pop per sqrt(2) of finger travel into
    // something that tracks the fingers.
    double zoom_frac_ = 0.0;
    // Degrees clockwise from north, view-only. See bearingDeg().
    double bearing_deg_ = 0.0;
    // viewTransform()'s memo, plus the inputs it was built from. Validated
    // against those inputs on every call, so no mutator has to invalidate it.
    mutable QTransform xform_;
    mutable double xform_world_px_ = 0.0;
    mutable double xform_bearing_ = 0.0;
    mutable double xform_nx_ = 0.0;
    mutable double xform_ny_ = 0.0;
    mutable QSize xform_size_;
    mutable bool xform_valid_ = false;
    QRectF view_bounds_;  // normalized world box; empty = unbounded
    // Ctrl+wheel notch accumulator. A trackpad emits many small deltas per
    // flick, so zoom steps on accumulated notches instead of one level per
    // event (one flick used to cross ten levels).
    int wheel_accum_ = 0;
    QElapsedTimer wheel_clock_;
    // Touch pan / pinch / twist. A finger never clicks this canvas —
    // placing a polygon corner, toggling a roof edge and selecting the
    // marker are precision acts and stay trackpad work. See
    // touch_gesture_state.hpp.
    touch_gestures::GestureState touch_;
    touch::SynthesizedMouseGuard touch_guard_;

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
    QImage map_raster_painted_;  // map_raster_, tinted for the live canvas
    QRectF map_raster_m_;    // its extent in robot_init metres
    bool dark_mode_ = true;

    Drag drag_ = Drag::None;
    int drag_corner_ = -1;
    int drag_edge_ = -1;
    QPoint drag_press_pos_;
    QPoint drag_last_;
};

}  // namespace f2c_cpp
