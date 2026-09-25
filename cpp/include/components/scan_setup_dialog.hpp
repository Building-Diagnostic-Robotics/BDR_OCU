/**
 * @file scan_setup_dialog.hpp
 * @brief "Start New Scan" mode/plan selector modal.
 *
 * Shown by `AppShellWindow::onStartNewScan` BEFORE the New Scan Information
 * modal. The first screen offers Saved Scan or New Scan. Saved Scan is the
 * plan list (unexecuted first, completed collapsed). New Scan is the two
 * mode cards. With no saved plans the dialog opens on New Scan.
 *
 * No Figma frame exists for this surface; it is derived value-for-value
 * from MissionMetadataDialog (Figma 194:152 — zinc palette, #00BC7D accent,
 * dark-only regardless of the global theme toggle) and the Dashboard
 * makeActionButton card language (2px brand border, tinted 40px SVG,
 * Arimo 700 18 title / 400 14 description).
 *
 * The Satellite card reads `ImageryReachabilityProbe`, which AppShell
 * starts on the dashboard. The card is disabled until that probe has
 * answered twice in a row. One failure disables it again.
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
class QVBoxLayout;
class QNetworkReply;
class QPushButton;
class QStackedWidget;

namespace f2c_cpp {

class ImageryReachabilityProbe;

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
                             QWidget* parent = nullptr,
                             ImageryReachabilityProbe* imagery = nullptr);
    ~ScanSetupDialog() override = default;

    Choice choice() const { return choice_; }
    /** Valid only when choice() == ExistingPlan. */
    Job selectedJob() const { return selected_job_; }

    /** Dev shot only: expand/collapse the COMPLETED disclosure. */
    void devSetCompletedOpen(bool open);
    /** Dev shot only: show the saved-plan list or the new-scan cards. */
    void devShowSaved();
    void devShowNew();

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
    void showPage(int page);
    void recenter();
    bool hasSavedPlans() const;
    void buildPlanSection(PlanSection& section, const QString& title,
                          const QVector<Job>& jobs, bool collapsible,
                          QVBoxLayout* root);
    QWidget* buildPlanRow(const Job& job, QWidget* parent);
    void onDeletePlanClicked(const Job& job, QWidget* row);
    void refreshSectionChrome();
    QPushButton* buildModeCard(QWidget* parent, const QString& object_name,
                               const QString& brand_color,
                               const QString& icon_resource,
                               const QString& title);
    void applyImageryReachable(bool reachable);
    void applyStyle();

    Choice choice_ = Choice::Cancelled;
    Job selected_job_;

    PlanSection planned_;
    PlanSection completed_;

    QLabel* title_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QPushButton* back_button_ = nullptr;

    QPushButton* satellite_card_ = nullptr;
    QLabel* satellite_title_ = nullptr;
    QLabel* satellite_status_ = nullptr;
    ImageryReachabilityProbe* imagery_ = nullptr;
    bool imagery_reachable_ = false;
};

}  // namespace f2c_cpp
