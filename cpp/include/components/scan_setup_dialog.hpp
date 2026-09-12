/**
 * @file scan_setup_dialog.hpp
 * @brief "Start New Scan" mode/plan selector modal.
 *
 * Shown by `AppShellWindow::onStartNewScan` BEFORE the New Scan Information
 * modal. Presents the operator's saved plans (unexecuted first) and the two
 * ways to start from scratch — Measured ROI (CAD grid, tape measurements)
 * or Satellite ROI (imagery). Picking a saved plan skips the mode question
 * entirely: the plan carries its own mode.
 *
 * No Figma frame exists for this surface; it is derived value-for-value
 * from MissionMetadataDialog (Figma 194:152 — zinc palette, #00BC7D accent,
 * dark-only regardless of the global theme toggle) and the Dashboard
 * makeActionButton card language (2px brand border, tinted 40px SVG,
 * Arimo 700 18 title / 400 14 description).
 *
 * The Satellite card is gated on imagery reachability. A satellite plan is
 * only worth starting where tiles can be fetched (the office), so the card
 * stays disabled until a periodic probe of `TileService::connectivityProbeUrl`
 * has answered twice in a row — the same debounce shape UploadDialog uses
 * for the cloud API. One failure disables it again.
 *
 * Plan lifecycle surfaces here: SAVED PLANS lists plans not yet scanned;
 * COMPLETED (N) is a collapsed disclosure of plans whose mission finalized
 * (data on disk), newest first, capped by JobStore::kCompletedPlansKept.
 * Every row carries a trash button — the only operator-facing delete path.
 * Deletion goes through JobStore::remove (plan + cached imagery) after a
 * confirm, and the row is dropped in place; the dialog stays open.
 */

#pragma once

#include "satellite_job_model.hpp"

#include <QDialog>
#include <QString>
#include <QVector>

class QLabel;
class QNetworkAccessManager;
class QVBoxLayout;
class QNetworkReply;
class QPushButton;
class QTimer;

namespace f2c_cpp {

class ScanSetupDialog : public QDialog {
    Q_OBJECT

public:
    enum class Choice {
        Cancelled,
        NewMeasuredPlan,
        NewSatellitePlan,
        ExistingPlan,
    };

    explicit ScanSetupDialog(const QVector<Job>& jobs,
                             QWidget* parent = nullptr);
    ~ScanSetupDialog() override;

    Choice choice() const { return choice_; }
    /** Valid only when choice() == ExistingPlan. */
    Job selectedJob() const { return selected_job_; }

    /** Dev shot only: expand/collapse the COMPLETED disclosure. */
    void devSetCompletedOpen(bool open);

    static constexpr int kProbeIntervalMs = 5000;
    static constexpr int kProbeTimeoutMs = 4000;
    static constexpr int kProbeSuccessesToEnable = 2;

signals:
    /** A plan was deleted from disk via the row's trash button. */
    void planDeleted(const QString& job_id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /** One plan list (PLANNED or COMPLETED) with its header and rows. */
    struct PlanSection {
        QWidget* host = nullptr;        // header + body, hidden when empty
        QLabel* header = nullptr;       // "SAVED PLANS" / "COMPLETED (N)"
        QPushButton* toggle = nullptr;  // disclosure; null for SAVED PLANS
        QWidget* body = nullptr;        // the row stack (collapsible)
        QVBoxLayout* rows = nullptr;
        int count = 0;
        bool completed = false;
    };

    void buildUi(const QVector<Job>& jobs);
    void buildPlanSection(PlanSection& section, const QString& title,
                          const QVector<Job>& jobs, bool collapsible,
                          QVBoxLayout* root);
    QWidget* buildPlanRow(const Job& job, QWidget* parent);
    void onDeletePlanClicked(const Job& job, QWidget* row);
    void refreshSectionChrome();
    QPushButton* buildModeCard(QWidget* parent, const QString& object_name,
                               const QString& brand_color,
                               const QString& icon_resource,
                               const QString& title,
                               const QString& description);
    void applyStyle();

    void startImageryProbe();
    void onProbeTick();
    void onProbeFinished(QNetworkReply* reply);
    void applyImageryReachable(bool reachable);

    Choice choice_ = Choice::Cancelled;
    Job selected_job_;

    PlanSection planned_;
    PlanSection completed_;
    QWidget* divider_ = nullptr;  // "or start from scratch"; hidden when
                                  // both sections are empty

    QPushButton* satellite_card_ = nullptr;
    QLabel* satellite_title_ = nullptr;
    QLabel* satellite_description_ = nullptr;
    QString satellite_description_text_;
    QNetworkAccessManager* probe_nam_ = nullptr;
    QTimer* probe_timer_ = nullptr;
    QNetworkReply* probe_inflight_ = nullptr;
    int probe_successes_ = 0;
    bool probe_answered_ = false;
    bool imagery_reachable_ = false;
};

}  // namespace f2c_cpp
