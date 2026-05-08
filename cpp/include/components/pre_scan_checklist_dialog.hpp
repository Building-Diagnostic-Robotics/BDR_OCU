/**
 * @file pre_scan_checklist_dialog.hpp
 * @brief Pre-scan operator checklist modal that gates entry into Stage 4
 *        (Scan execution). Forces the operator to acknowledge GPR, Proceq
 *        app state, scan-area clearance, monitoring duty, and obstacle
 *        marking before the Scan stage becomes interactive.
 *
 * Frameless, modal, no close button (no override). Theme via
 * uiThemeTokens(dark_mode). Mirrors the segment-list selection style:
 * QListWidget multi-select rows with a custom delegate that paints a
 * green check when a row is "ticked" (selected).
 *
 * Row 1 ("GPR connected") has an inline "Wake GPR" button that emits
 * wakeGprRequested(); the host wires it to /gpr_line_stop via
 * AppShellWindow. After click the button disables for 5 s with a live
 * countdown label, then re-enables — the GPR may need multiple wake
 * presses.
 */

#pragma once

#include <QDialog>

class QGraphicsBlurEffect;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

namespace f2c_cpp {

class PreScanChecklistDialog : public QDialog {
    Q_OBJECT

public:
    explicit PreScanChecklistDialog(bool dark_mode, QWidget* parent = nullptr);

signals:
    /** Operator pressed Wake GPR. Host should call /gpr_line_stop Trigger. */
    void wakeGprRequested();

protected:
    // Swallow Esc and any other reject path. The dialog is a hard gate —
    // the operator must Confirm to leave. closeEvent ignores window-
    // manager close (Alt+F4 / system menu) for the same reason.
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

public slots:
    // Override QDialog::reject so any code path that calls reject()
    // (e.g. Esc fallthrough, parent destruction) becomes a no-op.
    void reject() override;

private slots:
    void onRowSelectionChanged();
    void onConfirmClicked();
    void onWakeClicked();
    void onWakeCountdownTick();

private:
    void buildUi();
    void applyStyle();
    bool allRowsTicked() const;

    bool dark_mode_ = true;

    QListWidget* list_ = nullptr;
    QPushButton* btn_wake_ = nullptr;
    QPushButton* btn_confirm_ = nullptr;
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_subtitle_ = nullptr;

    QTimer* wake_countdown_timer_ = nullptr;
    int wake_countdown_remaining_s_ = 0;
    static constexpr int kWakeCountdownStartS = 5;
};

}  // namespace f2c_cpp
