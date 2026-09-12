/**
 * @file satellite_screen.hpp
 * @brief Stage 6 — ROI Coverage planning + autonomous mission screen.
 *
 * Two canvas modes on one screen: Satellite (Esri imagery) and Measured
 * (CAD grid, tape-measurement planning). Opened by the Start New Scan flow
 * (ScanSetupDialog -> MissionMetadataDialog) or the Dashboard "Plan Job"
 * card (planning-only trim).
 *
 * The surface has a Figma frame: 49px top bar, a 55px step header of five
 * chips, then a 320px LEFT rail beside the canvas with a 65px footer bar
 * carrying the gated "Next" action. Zinc palette (#18181b chrome, #27272a
 * inputs, #00BC7D accent) in dark mode, tokens-light equivalents in light
 * mode. The frame specifies Inter; we keep the Arimo per-element typography
 * the other stages use rather than splitting the app across two families.
 *
 * The five steps and the gates they derive from are specified in
 * `docs/SATELLITE_WORKFLOW.md`. The step model is derived, not stored: it
 * reads the same enable-gates the buttons already use, so the header can
 * never claim a step is reachable when its gate disagrees.
 *
 * Robot-side counterpart: `robot_autonomous_coverage_director.launch.py`,
 * which exists only on the pilot_ws `cliff-on-autonomy` branch — that is
 * also where the /coverage/status publisher and the roof-edge setback live.
 * Despite its name, `feature/ocu-satellite-roi` carries the older
 * horizon-manager stack and is NOT what this screen launches.
 */

#pragma once

#include "satellite_job_model.hpp"
#include "satellite_map_capture.hpp"
#include "satellite_tile_service.hpp"
#include "similarity_2d.hpp"

#include <QDate>
#include <QImage>
#include <QRectF>
#include <QSet>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTimer;

namespace f2c_cpp {

class LinkHealthMonitor;
class MissionController;
class PanZoomImageWidget;
class RosLink;
class SatelliteMapWidget;

class SatelliteScreen : public QWidget {
    Q_OBJECT

public:
    enum class PlanMode { Satellite, Measured };

    explicit SatelliteScreen(QWidget* parent = nullptr);
    ~SatelliteScreen() override;

    void setDarkMode(bool dark_mode);

    /** Onsite execution of a saved plan (mode comes from the plan). */
    void configureForScan(const Job& job);
    /** Onsite execution starting from an empty plan of the given mode. */
    void configureForScan(PlanMode mode);
    /** Office preplanning trim: mission/teleop hidden, Send unavailable. */
    void configureForPlanning();

    /** Dev screenshot hook: seeds a demo ROI (roof edges marked) + robot
        marker so canvas rendering can be verified headlessly. */
    void devSeedDemoPlan();
    /** Dev screenshot hook: selects the marker so the rotate handle renders. */
    void devSelectMarker();
    /** Dev shot only: frame the ROI as a finished draw would. */
    void devFitRoi();
    /** Dev screenshot hook: renders the Save Plan confirmation to a PNG. */
    void devRenderPlanConfirm(const QString& png_path);
    /** Dev screenshot hook: fakes a collected map + site image and opens the
        correspondence picker (or, with `review`, a solved alignment). */
    void devSeedDemoAlignment(bool review);
    /** Dev shot only: step 2 before any capture (site + capture CTA). */
    void devSeedDemoAlignmentEmpty();

    /** Mirrors the Dashboard MQTT battery sample onto the top-bar pill
        (same contract as ExplorationScreen/PlannerScreen). */
    void setTopBatteryState(double pct, bool stale);

    /**
     * Attach the app's LinkHealthMonitor (owned by AppShellWindow). The
     * screen stamps it from every ROS callback per the house rule and the
     * BOT pill derives from its layered state while armed; without it (or
     * pre-arm) the pill falls back to raw odometry age.
     */
    void attachLinkHealthMonitor(LinkHealthMonitor* monitor);

    /** True while a mission launch is active (blocks app close, like scans). */
    bool missionActive() const;
    /** Safe teardown for app close: autonomy off, disarm, kill launches. */
    void shutdownMission();

signals:
    void backRequested();
    /** Mission launch lifecycle — AppShell arms/disarms the link monitor
        and reachability probe on this. */
    void missionActiveChanged(bool active);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    /**
     * The five operator-facing steps, in order. Specified in
     * `docs/SATELLITE_WORKFLOW.md`. ROI deliberately follows Alignment: the
     * operator draws against imagery already fitted to the robot's map
     * rather than drawing first and discovering the fit moved everything.
     */
    enum class Step {
        SatelliteMap = 0,
        Alignment = 1,
        RoiDefinition = 2,
        EdgeReview = 3,
        AutonomousScan = 4,
    };
    static constexpr int kStepCount = 5;

    /** Reachable in the current trim. The office has no robot, so the
        robot-dependent steps are unavailable rather than merely incomplete —
        which is what lets planning run 1 -> 3 -> 4. */
    bool stepAvailable(Step step) const;
    /**
     * Whether this step's own work is done. Reads the same expressions the
     * buttons already gate on; there is deliberately no stored duplicate of
     * this, because a copy could drift and advertise a step as passable
     * while the real gate still refuses.
     */
    bool stepComplete(Step step) const;
    /** First available incomplete step, else the last available one. Used to
        seed `selected_step_`, not to move it — advancement is explicit. */
    Step computeStep() const;
    /** Available, and every available step before it is complete. */
    bool stepReachable(Step step) const;
    /** Next available step after `step`, or `step` itself if there is none. */
    Step nextAvailableStep(Step step) const;
    void setSelectedStep(Step step);
    /** Re-renders the step chips, the footer action, and rail visibility. */
    void refreshStepUi();

    QWidget* buildTopBar();
    QWidget* buildStepHeader();
    QWidget* buildFooterBar();
    QWidget* buildLeftRail();
    QWidget* buildPlanCard(QWidget* parent);
    QWidget* buildAlignCard(QWidget* parent);
    QWidget* buildCorrespondPage();
    QWidget* buildAlignReviewPage();
    QWidget* buildMissionCard(QWidget* parent);
    QWidget* buildTeleopCard(QWidget* parent);
    QWidget* buildLogCard(QWidget* parent);
    /** Card carrying a step's acknowledgement checkbox — the whole content
        of steps 3 and 4 until their Figma frames land. */
    QWidget* buildAckCard(QWidget* parent, const QString& object_name,
                          const QString& icon, const QString& title,
                          const QString& description, const QString& check,
                          QCheckBox** out_check);
    void applyTheme();
    void applyModeVisibility();
    /** Shows only the active step's rail cards. Called from
        applyModeVisibility() so every existing call site keeps working. */
    void applyStepVisibility();

    void refreshJobsCombo(const QString& select_id = QString());
    /** Re-reads the store and rebuilds the combo items WITHOUT loading a
        plan into the canvas. Selection is preserved by id. */
    void populateJobsCombo(const QString& select_id);
    /**
     * Mission finalized with data on disk: stamp the current plan
     * `last_executed_at` (moves it to COMPLETED in Scan Setup) and prune
     * completed plans beyond JobStore::kCompletedPlansKept. Called from
     * both Complete Mission paths only on a successful finalize — a failed
     * or watchdog-deferred finalize leaves the plan PLANNED for a re-run.
     */
    void markCurrentPlanCompleted();
    void loadJob(const Job& job);
    void newJob();
    /**
     * Save Plan. In the office trim on the satellite canvas this is the
     * imagery step: it frames the ROI, opens SatellitePlanConfirmDialog, and
     * the download runs as part of saving. Everywhere else (field, measured)
     * it persists geometry only — there is no internet on the roof, and a
     * field edit must never try to fetch.
     */
    void saveJob();
    /** The Job as the rail currently describes it, id allocated if new,
        with the fields the rail does not own carried forward. */
    Job jobFromRail() const;
    /** Writes `job` and re-selects it. False (and a log line) on failure. */
    bool persistJob(const Job& job);
    /** Adopts a fresh imagery manifest into the job record and the canvas. */
    void adoptImageryManifest(Job& job,
                              const TileService::SiteManifest& manifest);
    /** Canvas tool stack: zoom, fit to ROI, ruler. */
    QWidget* buildCanvasTools(QWidget* parent);
    /** Relabels the draw button for the armed tool / drawing state. */
    void refreshDrawButton();

    void onGoToAddress();
    /** Recentres on the best known robot position: confirmed anchor, else
        the map-collection GPS seed, else the saved plan's seed. */
    void onFindRobot();
    /** Points the tile service at a job's offline pyramid + zoom ceiling. */
    void applyImageryManifest(const TileService::SiteManifest& manifest,
                              const QString& assets_dir);

    // ---- Alignment (robot map <-> satellite imagery) ----
    void onCollectMap();
    void onMapCaptured(const MapCapture& capture);
    void showCorrespondPage();
    void onSatellitePicked(QPointF image_pt);
    void onPcdPicked(QPointF image_pt);
    void onUndoCorrespondence();
    void onClearCorrespondences();
    /** Writes the alignment status line to every surface that shows it
        (rail card in measured mode, the point-cloud pane's title in
        satellite mode) and mirrors the capture button labels. */
    void setAlignStatus(const QString& text);
    /** Dev shots: select step 2 while preserving a synthetic site image. */
    void devEnterAlignmentWithDemoSite();
    void onAlignClicked();
    void onReselectAlignment();
    void onConfirmAlignment();
    void refreshCorrespondenceMarkers();
    void updateCorrespondenceUi();
    void updateAlignCardUi();
    /** Loads the job's stitched site.jpg + manifest for picking. Empty on ok. */
    QString loadSiteImage();
    /**
     * Correspondences needed before Align unlocks. A GPS seed independently
     * pins position and (usually) heading, so three well-spread pairs are
     * enough to solve scale; without it the fit has to find everything from
     * the picks alone and needs the extra redundancy.
     */
    int minCorrespondences() const;
    /**
     * Refresh the source-imagery provenance label for the current view.
     * Driven off the existing 1 Hz slow_timer_ and gated by
     * imagery_query_pending_ — viewChanged fires on every wheel notch and
     * drag step, so querying directly from it would be a request storm.
     */
    void refreshImageryInfo();
    void onSendMission();
    /**
     * Operator-facing mission end. Branches on the strict link state: normal
     * path when we can talk to the robot, OfflineFinalizeDialog when we
     * genuinely cannot. Mirrors the Stage 5 contract.
     */
    void onCompleteMission();
    /** Disarm wait -> /dc/finalize_mission -> teardown. */
    void executeCompleteMissionNormalPath();
    /** finalize_mission_local.py over SSH, then teardown. No RPCs. */
    void executeCompleteMissionSshFallback();
    /** Requests IDLE and polls motorsIdle() up to a ceiling, then continues. */
    void beginMotorsIdleWait(std::function<void(bool timed_out)> on_done);
    /** True only in genuine Disconnected — Reconnecting does not count. */
    bool isRobotLinkUnreachable() const;
    void onEstop();

    void publishTeleopTick();
    void setAutonomyEnabled(bool enabled);
    void startAutonomyLatch();
    void stopAutonomyLatch();
    void startMetadataPushLoop();
    void stopMetadataPushLoop();
    void attemptMetadataPush();
    void updateBotPill();
    void updateStatePill();
    void setBotPill(const QString& text, const QColor& color);
    void setStatePill(const QString& text, const QColor& color);
    void setMotorsChip(const QString& text, const QColor& color);
    /** Drives the motors chip from live controller_status. */
    void updateMotorsChip();
    void appendLog(const QString& line);
    bool confirmDialog(const QString& title, const QString& body,
                       const QString& accept_label);

    // Core services.
    TileService* tiles_ = nullptr;
    SatelliteMapWidget* map_ = nullptr;
    RosLink* ros_ = nullptr;
    MissionController* mission_ = nullptr;
    JobStore job_store_;
    QVector<Job> jobs_;
    QString current_job_id_;

    // Top bar.
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_top_battery_ = nullptr;
    QLabel* lbl_bot_dot_ = nullptr;
    QLabel* lbl_bot_text_ = nullptr;
    QLabel* lbl_state_dot_ = nullptr;
    QLabel* lbl_state_text_ = nullptr;
    QWidget* motors_chip_ = nullptr;
    QLabel* lbl_motors_dot_ = nullptr;
    QLabel* lbl_motors_text_ = nullptr;

    // Step header. One chip per step plus the chevron that follows it (null
    // on the last). Chips are styled per-element rather than through QSS
    // state selectors, matching the pill/motors-chip pattern in this file —
    // dynamic-property selectors would need their own repolish on every
    // state change, not just on theme change.
    struct StepChip {
        QPushButton* button = nullptr;
        QLabel* badge = nullptr;
        QLabel* label = nullptr;
        QLabel* detail = nullptr;
        QLabel* chevron = nullptr;
    };
    QWidget* step_header_ = nullptr;
    QVector<StepChip> step_chips_;

    // Footer action bar.
    QWidget* footer_bar_ = nullptr;
    QPushButton* back_button_ = nullptr;
    QPushButton* next_button_ = nullptr;
    // The rail as a whole: hidden on the alignment step, whose frame is a
    // full-width two-pane picker with no side cards.
    QWidget* rail_scroll_ = nullptr;

    // Plan card.
    QWidget* plan_card_ = nullptr;
    QComboBox* jobs_combo_ = nullptr;
    QWidget* jobs_combo_row_ = nullptr;
    QLineEdit* job_name_ = nullptr;
    QLineEdit* job_address_ = nullptr;
    QWidget* geo_tools_host_ = nullptr;  // address search + provenance (geo-only)
    QLineEdit* address_edit_ = nullptr;
    // ROI drawing follows the Stage 5 pattern: pick a shape tool, one button
    // arms it ("Draw ROI" -> "Drawing…" -> "Redraw ROI"). The shape toggles
    // are checkable and mutually exclusive.
    QPushButton* tool_rect_button_ = nullptr;
    QPushButton* tool_polygon_button_ = nullptr;
    QPushButton* draw_button_ = nullptr;
    QPushButton* place_robot_button_ = nullptr;
    QPushButton* clear_roi_button_ = nullptr;
    // Numeric ROI/heading mirror. Hidden in the office trim, where the
    // canvas chips and handles are the whole editing surface.
    QWidget* roi_numeric_host_ = nullptr;
    QDoubleSpinBox* roi_length_ = nullptr;
    QDoubleSpinBox* roi_width_ = nullptr;
    QDoubleSpinBox* roi_heading_ = nullptr;
    QDoubleSpinBox* robot_heading_ = nullptr;
    QLabel* robot_pos_label_ = nullptr;
    QPushButton* find_robot_button_ = nullptr;
    QLabel* imagery_label_ = nullptr;  // source capture date / GSD (geo only)
    QPushButton* save_button_ = nullptr;

    // Step 3 / step 4 acknowledgement cards.
    QWidget* roi_confirm_card_ = nullptr;
    QCheckBox* roi_confirm_check_ = nullptr;
    QWidget* edge_review_card_ = nullptr;
    QCheckBox* edge_review_check_ = nullptr;

    // Align card + pages.
    struct Correspondence {
        QPointF sat_px;  // stitched site.jpg pixel
        QPointF pcd_m;   // robot_init metres
    };

    QWidget* align_card_ = nullptr;
    QPushButton* collect_map_button_ = nullptr;
    QPushButton* correspond_button_ = nullptr;
    QLabel* align_status_ = nullptr;

    QStackedWidget* canvas_stack_ = nullptr;
    // The map page: the map plus the tool stack floated over its right
    // edge. Stack switching targets this, not map_.
    QWidget* map_page_ = nullptr;
    QWidget* canvas_tools_ = nullptr;
    QPushButton* measure_button_ = nullptr;
    struct CanvasTool {
        QPushButton* button = nullptr;
        QString icon;  // resource path; empty for glyph-only buttons
    };
    QVector<CanvasTool> canvas_tool_buttons_;
    /** Re-tints the tool icons for the current palette. */
    void refreshCanvasToolIcons();
    QWidget* correspond_page_ = nullptr;
    QWidget* align_review_page_ = nullptr;
    PanZoomImageWidget* sat_pick_ = nullptr;
    PanZoomImageWidget* pcd_pick_ = nullptr;
    PanZoomImageWidget* review_view_ = nullptr;
    // Instruction bar across the top of the picker: prompt on the left,
    // per-pane pick counts + pair tally and the actions on the right.
    QLabel* corr_instruction_ = nullptr;
    QLabel* corr_legend_ = nullptr;
    QLabel* review_status_ = nullptr;
    QPushButton* corr_undo_button_ = nullptr;
    QPushButton* corr_clear_button_ = nullptr;
    QPushButton* align_button_ = nullptr;
    // Right pane: empty state (capture CTA) until a point cloud exists,
    // then the pick view.
    QStackedWidget* pcd_pane_stack_ = nullptr;
    QWidget* pcd_empty_ = nullptr;
    QLabel* pcd_empty_title_ = nullptr;
    QLabel* pcd_empty_hint_ = nullptr;
    QPushButton* capture_button_ = nullptr;

    QTimer* motors_idle_timer_ = nullptr;
    int motors_idle_ticks_ = 0;
    bool complete_mission_in_flight_ = false;

    MapCaptureRunner* map_capture_ = nullptr;
    QImage sat_image_;
    TileService::SiteManifest site_manifest_;
    QImage pcd_image_;
    QRectF pcd_bounds_m_;
    GpsFix capture_gps_;
    QVector<Correspondence> correspondences_;
    QPointF pending_sat_px_;
    bool have_pending_sat_ = false;
    Similarity2D pcd_to_sat_;
    double align_rmse_m_ = 0.0;
    /**
     * The operator accepted the fit and it became the robot anchor. Distinct
     * from `pcd_to_sat_.valid`, which only means the solve converged: a fit
     * can converge on badly-placed picks, which is exactly what the review
     * page asks the operator to catch. Gating step 2 on the solve would put
     * an enabled "Next" beside "Confirm alignment" and invite skipping the
     * check.
     */
    bool alignment_confirmed_ = false;

    // Last resolved imagery provenance for the current view. Stamped into the
    // Job on save so a plan carries forward what it was drawn against.
    QDate last_imagery_captured_;
    double last_imagery_res_m_ = 0.0;
    int last_imagery_zoom_ = 0;
    bool imagery_query_pending_ = false;

    // Mission card.
    QWidget* mission_card_ = nullptr;
    QLabel* reason_label_ = nullptr;
    QProgressBar* coverage_bar_ = nullptr;
    QLabel* segment_label_ = nullptr;
    QPushButton* send_button_ = nullptr;
    QPushButton* end_button_ = nullptr;
    QPushButton* autonomy_button_ = nullptr;
    QPushButton* arm_button_ = nullptr;
    QPushButton* disarm_button_ = nullptr;
    QPushButton* estop_button_ = nullptr;

    // Teleop card.
    QWidget* teleop_card_ = nullptr;
    QCheckBox* teleop_check_ = nullptr;
    QSlider* teleop_speed_ = nullptr;
    QLabel* teleop_speed_label_ = nullptr;

    QPlainTextEdit* log_view_ = nullptr;

    QTimer* teleop_timer_ = nullptr;
    QTimer* slow_timer_ = nullptr;
    QTimer* metadata_timer_ = nullptr;
    QTimer* autonomy_latch_timer_ = nullptr;
    QTimer* manager_watch_timer_ = nullptr;
    int metadata_attempts_ = 0;
    bool metadata_pushed_ = false;
    bool manager_occupancy_seen_ = false;
    LinkHealthMonitor* link_monitor_ = nullptr;
    QSet<int> pressed_keys_;
    bool autonomy_on_ = false;
    bool dark_mode_ = false;
    bool view_initialized_ = false;

    PlanMode plan_mode_ = PlanMode::Satellite;
    bool planning_only_ = false;

    // Which step the rail is showing. Display state, seeded from
    // computeStep() and moved only by the footer action or a chip click —
    // the gates decide what is *passable*, the operator decides what is
    // on screen. Auto-advancing the moment a gate flips would yank the rail
    // out from under someone still working in a step.
    Step selected_step_ = Step::SatelliteMap;
    /**
     * The canvas has been deliberately aimed at the building: a successful
     * geocode, a Find Robot seed, a placed marker, or a loaded plan that
     * already carries one of those. Panning by hand does not count — a
     * stray drag should not satisfy a gate.
     */
    bool canvas_aimed_ = false;
    /**
     * Vertices as they stood when the operator confirmed the ROI. Needed as
     * its own record because polygon().valid() is already true the moment a
     * saved plan loads, so without it step 3 would arrive pre-completed and
     * the confirm pass that motivated putting ROI after alignment could be
     * walked straight past. Stored as geometry rather than a bool so that
     * marking a roof edge — which also emits roiChanged — does not revoke
     * the confirmation and bounce the operator back a step mid-review.
     */
    QVector<geo::GeoPoint> confirmed_vertices_;
    bool roiMatchesConfirmed() const;
    /**
     * The operator has reviewed the roof edges. Deliberately NOT a count of
     * marked edges: a roof with no fall hazards is a legitimate answer, and
     * gating on a nonzero count would pressure the operator into marking
     * something spurious to advance. Per-session and unpersisted — parapets
     * and skylights are exactly what imagery gets wrong, so an office review
     * does not stand in for looking at the real roof.
     */
    bool edges_reviewed_ = false;

    // Last-rendered pill states, kept so setDarkMode() can re-render every
    // dynamic surface against the new palette.
    double last_batt_pct_ = 0.0;
    bool last_batt_stale_ = true;
    QString bot_text_;
    QColor bot_color_;
    QString state_text_;
    QColor state_color_;
    QString motors_text_;
    QColor motors_color_;
};

}  // namespace f2c_cpp
