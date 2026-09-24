/**
 * @file mission_metadata_dialog.hpp
 * @brief "New Scan Information" modal — Building / Operator capture.
 *
 * Frameless modal shown by `AppShellWindow::onStartNewScan` after the
 * operator clicks "Start New Scan" on the Dashboard, BEFORE the Stage 4
 * (Exploration) transition. Collects two things, both session-wide:
 *
 *   - Building name (free text, slugified for the on-disk path)
 *   - Operator name (free text)
 *
 * Display units are feet, fixed in `UnitsProvider`. This dialog does not
 * ask. Robot payloads stay SI.
 *
 * Visual reference: Figma node 194:152 (`Untitled` file, key
 * `I9tRcFEniAqnXD0yiMlo6W`). Background blur on the Dashboard stage is
 * applied by the caller (mirrors the `PlannerScreen::showScanPreflightDialog`
 * pattern in `planner_screen.cpp:5202`).
 *
 * On Accepted (Proceed to Scan), the building and operator names are
 * persisted to QSettings via the keys in `settings_constants.hpp`.
 *
 * Cancel paths: the X close button in the header AND the Cancel button in
 * the footer both call `reject()` — both leave the operator on the
 * Dashboard with no transition and no robot contact.
 */

#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;

namespace f2c_cpp {

class MissionMetadataDialog : public QDialog {
    Q_OBJECT

public:
    explicit MissionMetadataDialog(QWidget* parent = nullptr);

    /**
     * Prefill the building name (used when a saved plan was selected in the
     * ScanSetupDialog — the plan's name seeds the building field, remaining
     * fully editable). Overrides the QSettings-restored value.
     */
    void setInitialBuildingName(const QString& name);

    /** Building name as typed by the operator (raw, untrimmed of leading/trailing space). */
    QString buildingName() const;

    /** Path-safe slug derived from `buildingName()`. Capped at 64 chars. */
    QString buildingSlug() const;

    /** Operator name as typed by the operator. */
    QString operatorName() const;

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
    void onProceedClicked();
    void onCancelClicked();
    void onCloseClicked();
    void onAnyTextChanged();

private:
    void buildUi();
    void applyStyle();
    void refreshProceedEnabled();
    void loadDefaults();

    QLabel* lbl_header_title_ = nullptr;
    QLabel* lbl_header_subtitle_ = nullptr;
    QPushButton* btn_close_ = nullptr;

    QLineEdit* edit_building_ = nullptr;
    QLabel* lbl_slug_preview_ = nullptr;

    QLineEdit* edit_operator_ = nullptr;

    QPushButton* btn_cancel_ = nullptr;
    QPushButton* btn_proceed_ = nullptr;
};

}  // namespace f2c_cpp
