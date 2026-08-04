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
 */

#pragma once

#include "satellite_job_model.hpp"

#include <QDialog>
#include <QString>
#include <QVector>

class QLabel;
class QPushButton;

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

    Choice choice() const { return choice_; }
    /** Valid only when choice() == ExistingPlan. */
    Job selectedJob() const { return selected_job_; }

private:
    void buildUi(const QVector<Job>& jobs);
    QWidget* buildPlanRow(const Job& job, QWidget* parent);
    QPushButton* buildModeCard(QWidget* parent, const QString& object_name,
                               const QString& brand_color,
                               const QString& icon_resource,
                               const QString& title,
                               const QString& description);
    void applyStyle();

    Choice choice_ = Choice::Cancelled;
    Job selected_job_;
};

}  // namespace f2c_cpp
