/**
 * @file mission_metadata_dialog.hpp
 * @brief "New Scan Information" modal — Building / Operator / Units capture.
 *
 * Frameless modal shown by `AppShellWindow::onStartNewScan` after the
 * operator clicks "Start New Scan" on the Dashboard, BEFORE the Stage 4
 * (Exploration) transition. Collects three things, all session-wide:
 *
 *   - Building name (free text, slugified for the on-disk path)
 *   - Operator name (free text)
 *   - Unit system (Metric (m) / ANSI (ft)) — display-only, robot stays SI
 *
 * Visual reference: Figma node 194:152 (`Untitled` file, key
 * `I9tRcFEniAqnXD0yiMlo6W`). Background blur on the Dashboard stage is
 * applied by the caller (mirrors the `PlannerScreen::showScanPreflightDialog`
 * pattern in `planner_screen.cpp:5202`).
 *
 * On Accepted (Proceed to Scan), the values are persisted to QSettings via
 * the keys in `settings_constants.hpp` and `UnitsProvider::setUnits()` is
 * called so subsequent display sites pick up the choice immediately.
 *
 * Cancel paths: the X close button in the header AND the Cancel button in
 * the footer both call `reject()` — both leave the operator on the
 * Dashboard with no transition and no robot contact.
 */

#pragma once

#include <QDialog>
#include <QString>

#include "units_system.hpp"

class QLabel;
class QLineEdit;
class QPushButton;

namespace f2c_cpp {

class MissionMetadataDialog : public QDialog {
    Q_OBJECT

public:
    explicit MissionMetadataDialog(QWidget* parent = nullptr);

    /** Building name as typed by the operator (raw, untrimmed of leading/trailing space). */
    QString buildingName() const;

    /** Path-safe slug derived from `buildingName()`. Capped at 64 chars. */
    QString buildingSlug() const;

    /** Operator name as typed by the operator. */
    QString operatorName() const;

    /** Selected unit system. */
    Units units() const { return selected_units_; }

    /**
     * @brief Convert an arbitrary string into a path-safe slug.
     *
     * Replaces runs of non-`[A-Za-z0-9._-]` characters with `_`, strips
     * leading/trailing `_` and `.`, and truncates to 64 chars. An empty
     * input or one that slugifies to nothing returns an empty string —
     * the dialog uses that to keep Proceed disabled.
     */
    static QString slugify(const QString& raw);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void onMetricClicked();
    void onAnsiClicked();
    void onProceedClicked();
    void onCancelClicked();
    void onCloseClicked();
    void onAnyTextChanged();

private:
    void buildUi();
    void applyStyle();
    void refreshUnitToggleVisuals();
    void refreshInfoBanner();
    void refreshProceedEnabled();
    void loadDefaults();

    QLabel* lbl_header_title_ = nullptr;
    QLabel* lbl_header_subtitle_ = nullptr;
    QPushButton* btn_close_ = nullptr;

    QLineEdit* edit_building_ = nullptr;
    QLabel* lbl_slug_preview_ = nullptr;

    QLineEdit* edit_operator_ = nullptr;

    QPushButton* btn_unit_metric_ = nullptr;
    QPushButton* btn_unit_ansi_ = nullptr;

    QLabel* lbl_info_banner_ = nullptr;

    QPushButton* btn_cancel_ = nullptr;
    QPushButton* btn_proceed_ = nullptr;

    Units selected_units_ = Units::Metric;
};

}  // namespace f2c_cpp
