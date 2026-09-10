/**
 * @file satellite_screen.hpp
 * @brief Stage 6 — ROI Coverage planning + autonomous mission screen.
 *
 * Two canvas modes on one screen: Satellite (Esri imagery) and Measured
 * (CAD grid, tape-measurement planning). Opened by the Start New Scan flow
 * (ScanSetupDialog -> MissionMetadataDialog) or the Dashboard "Plan Job"
 * card (planning-only trim).
 *
 * No Figma frame exists for this surface. Construction follows the staged
 * screens' vocabulary: 49px top bar with the Stage 4/5 SVG back button and
 * makePlannerStatusItem-style pills (Battery / BOT / state / motors chip),
 * a 320px LEFT rail of cards, zinc palette (#18181b cards, #27272a inputs,
 * #00BC7D accent) in dark mode per MissionMetadataDialog, tokens-light
 * equivalents in light mode, Arimo per-element typography throughout.
 *
 * Robot-side counterpart: `robot_autonomous_coverage.launch.py` on the
 * pilot_ws `spline_tracing` line (see feature/ocu-satellite-roi for the
 * /coverage/status publisher).
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
    /** Dev screenshot hook: fakes a collected map + site image and opens the
        correspondence picker (or, with `review`, a solved alignment). */
    void devSeedDemoAlignment(bool review);

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
    QWidget* buildTopBar();
    QWidget* buildLeftRail();
    QWidget* buildPlanCard(QWidget* parent);
    QWidget* buildAlignCard(QWidget* parent);
    QWidget* buildCorrespondPage();
    QWidget* buildAlignReviewPage();
    QWidget* buildMissionCard(QWidget* parent);
    QWidget* buildTeleopCard(QWidget* parent);
    QWidget* buildLogCard(QWidget* parent);
    void applyTheme();
    void applyModeVisibility();

    void refreshJobsCombo(const QString& select_id = QString());
    void loadJob(const Job& job);
    void newJob();
    void saveJob();

    void onGoToAddress();
    /** Recentres on the best known robot position: confirmed anchor, else
        the map-collection GPS seed, else the saved plan's seed. */
    void onFindRobot();
    void onDownloadArea();
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
    void onDeleteSelectedCorrespondence();
    void onClearCorrespondences();
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

    // Plan card.
    QComboBox* jobs_combo_ = nullptr;
    QWidget* jobs_combo_row_ = nullptr;
    QLineEdit* job_name_ = nullptr;
    QLineEdit* job_address_ = nullptr;
    QWidget* geo_tools_host_ = nullptr;  // address search + download (geo-only)
    QLineEdit* address_edit_ = nullptr;
    QPushButton* add_roi_button_ = nullptr;
    QPushButton* draw_polygon_button_ = nullptr;
    QPushButton* place_robot_button_ = nullptr;
    QPushButton* clear_roi_button_ = nullptr;
    QDoubleSpinBox* roi_length_ = nullptr;
    QDoubleSpinBox* roi_width_ = nullptr;
    QDoubleSpinBox* roi_heading_ = nullptr;
    QDoubleSpinBox* robot_heading_ = nullptr;
    QLabel* robot_pos_label_ = nullptr;
    QPushButton* find_robot_button_ = nullptr;
    QLabel* imagery_label_ = nullptr;  // source capture date / GSD (geo only)
    QPushButton* save_button_ = nullptr;

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
    QWidget* correspond_page_ = nullptr;
    QWidget* align_review_page_ = nullptr;
    PanZoomImageWidget* sat_pick_ = nullptr;
    PanZoomImageWidget* pcd_pick_ = nullptr;
    PanZoomImageWidget* review_view_ = nullptr;
    QListWidget* corr_list_ = nullptr;
    QLabel* corr_status_ = nullptr;
    QLabel* review_status_ = nullptr;
    QPushButton* corr_undo_button_ = nullptr;
    QPushButton* corr_delete_button_ = nullptr;
    QPushButton* corr_clear_button_ = nullptr;
    QPushButton* align_button_ = nullptr;

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
