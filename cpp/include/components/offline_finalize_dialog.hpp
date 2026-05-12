/**
 * @file offline_finalize_dialog.hpp
 * @brief "Robot Offline" Complete Mission modal — data-first finalize path.
 *
 * Shown by `AppShellWindow::onPlannerCompleteMissionRequested` when the
 * operator presses Complete Mission while `LinkHealthMonitor` reports
 * the robot is Disconnected.  Reflects the project's data-first
 * philosophy: never block Complete Mission, always give the operator a
 * way to finalize the data that was already collected on the robot's
 * disk regardless of radio state.
 *
 * Three CTAs:
 *
 *   1. Wait for reconnect (default, primary).  Caller polls the link
 *      monitor; when it transitions back to Healthy, the dialog
 *      auto-accepts with `Choice::WaitReconnected`.  Otherwise after
 *      `kAutoFallbackMs` (5 minutes) it auto-accepts with
 *      `Choice::FinalizeOverSsh` so the bot doesn't sit in
 *      CLOSED_LOOP_CONTROL forever if the operator forgets the modal.
 *      A live countdown is shown.
 *
 *   2. Finalize via SSH (offline).  Operator wants to wrap up now
 *      regardless.  Dialog accepts with `Choice::FinalizeOverSsh`;
 *      caller runs the existing teardown SSH plus the new
 *      `finalize_mission_local.py` helper to land
 *      `mission_finalized_at` + `finalized_via: ssh_offline`.
 *
 *   3. Cancel.  Dialog rejects.  Operator stays on Stage 5; the robot-
 *      side auto-finalize watchdog (10 min idle) will eventually do
 *      the same thing if they walk away.
 *
 * The dialog is frameless to match the rest of the BDR UI and parents
 * to the AppShellWindow with a blur effect on the parent (handled by
 * the caller, mirroring `MissionMetadataDialog`).
 */

#pragma once

#include <QDialog>
#include <QString>
#include <QtGlobal>

#include "link_health_monitor.hpp"

class QLabel;
class QPushButton;
class QTimer;

namespace f2c_cpp {

class OfflineFinalizeDialog : public QDialog {
    Q_OBJECT

public:
    enum class Choice {
        Cancelled = 0,
        WaitReconnected,    // link recovered while operator was waiting
        FinalizeOverSsh,    // operator chose SSH OR 5-min auto-fallback fired
    };

    /// `monitor` is observed for state transitions; if it goes Healthy
    /// while the dialog is open, the dialog auto-accepts as
    /// `WaitReconnected`.
    OfflineFinalizeDialog(LinkHealthMonitor* monitor,
                          QWidget* parent = nullptr);

    Choice chosen() const { return chosen_; }

    /// 5-minute auto-fallback to SSH-offline, locked per the user's
    /// spec.  Exposed as a constant so tests / dev builds can shorten.
    static constexpr int kAutoFallbackMs = 5 * 60 * 1000;

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void onWaitClicked();
    void onSshClicked();
    void onCancelClicked();
    void onLinkStateChanged(LinkHealthMonitor::State old_state,
                            LinkHealthMonitor::State new_state,
                            qint64 since_ms);
    void onTick();

private:
    void buildUi();
    void applyStyle();
    void refreshStatusLabels();
    void enterWaitMode();

    LinkHealthMonitor* monitor_ = nullptr;
    Choice chosen_ = Choice::Cancelled;
    bool wait_mode_ = false;
    qint64 wait_started_at_ms_ = 0;

    QTimer* tick_timer_ = nullptr;

    QLabel* lbl_header_ = nullptr;
    QLabel* lbl_body_ = nullptr;
    QLabel* lbl_link_status_ = nullptr;
    QLabel* lbl_countdown_ = nullptr;
    QPushButton* btn_wait_ = nullptr;
    QPushButton* btn_ssh_ = nullptr;
    QPushButton* btn_cancel_ = nullptr;
};

}  // namespace f2c_cpp
