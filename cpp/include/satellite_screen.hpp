/**
 * @file satellite_screen.hpp
 * @brief Stage 6 — Satellite Coverage: office/onsite ROI planning on Esri
 *        imagery, robot-anchored ROI export, autonomous coverage launch, and
 *        live mission overlay. Self-contained like the other staged screens;
 *        AppShellWindow only routes navigation and dark mode.
 *
 * Robot-side counterpart: `robot_autonomous_coverage.launch.py` on the
 * pilot_ws `spline_tracing` line (see feature/ocu-satellite-roi for the
 * /coverage/status publisher).
 */

#pragma once

#include "satellite_job_model.hpp"

#include <QSet>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QTimer;

namespace f2c_cpp {

class MissionController;
class RosLink;
class SatelliteMapWidget;
class TileService;

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

    /** True while a mission launch is active (blocks app close, like scans). */
    bool missionActive() const;
    /** Safe teardown for app close: autonomy off, disarm, kill launches. */
    void shutdownMission();

signals:
    void backRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QWidget* buildTopBar();
    QWidget* buildToolbar();
    QWidget* buildSidePanel();
    QWidget* buildPlanCard();
    QWidget* buildMissionCard();
    QWidget* buildTeleopCard();
    void applyStyles();

    void refreshJobsCombo(const QString& select_id = QString());
    void loadJob(const Job& job);
    void newJob();
    void saveJob();

    void onGoToAddress();
    void onDownloadArea();
    void onSendMission();
    void onEndMission();
    void onEstop();

    void publishTeleopTick();
    void updateLinkPill();
    void updateStatePill();
    void setStatePill(const QString& text, const QColor& color);
    void setLinkPill(const QString& text, const QColor& color);
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
    QLabel* link_pill_dot_ = nullptr;
    QLabel* link_pill_text_ = nullptr;
    QLabel* state_pill_dot_ = nullptr;
    QLabel* state_pill_text_ = nullptr;

    // Toolbar.
    QComboBox* jobs_combo_ = nullptr;
    QLineEdit* address_edit_ = nullptr;

    // Plan card.
    QLineEdit* job_name_ = nullptr;
    QLineEdit* job_address_ = nullptr;
    QPushButton* add_roi_button_ = nullptr;
    QPushButton* place_robot_button_ = nullptr;
    QDoubleSpinBox* roi_length_ = nullptr;
    QDoubleSpinBox* roi_width_ = nullptr;
    QDoubleSpinBox* roi_heading_ = nullptr;
    QDoubleSpinBox* robot_heading_ = nullptr;
    QLabel* robot_pos_label_ = nullptr;

    // Mission card.
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
    QCheckBox* teleop_check_ = nullptr;
    QSlider* teleop_speed_ = nullptr;
    QLabel* teleop_speed_label_ = nullptr;

    QPlainTextEdit* log_view_ = nullptr;

    QTimer* teleop_timer_ = nullptr;
    QTimer* slow_timer_ = nullptr;
    QSet<int> pressed_keys_;
    bool autonomy_on_ = false;
    bool dark_mode_ = false;
    bool view_initialized_ = false;

    PlanMode plan_mode_ = PlanMode::Satellite;
    bool planning_only_ = false;
    QWidget* mission_card_ = nullptr;
    QWidget* teleop_card_ = nullptr;
    QWidget* geo_tools_host_ = nullptr;  // address search + download (geo-only)

    void applyModeVisibility();
};

}  // namespace f2c_cpp
