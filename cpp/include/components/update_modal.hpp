/**
 * @file update_modal.hpp
 * @brief Frameless OTA "What's New" modal mounted from the update banner.
 *
 * Locked spec for this phase (Phase 6):
 *   - Q1 (X-button) = B  → close-only. No snooze fired on X. The banner stays
 *     visible afterwards and clicking View Details reopens the modal.
 *   - Q2 (battery)  = C  → no /sys/class/power_supply/BAT*  is treated as
 *     "AC" (allow install, render "Battery: AC", no gating).
 *   - Q3 (Install)  = A  → signal-only. Modal emits installRequested() and
 *     stays open; AppShell logs the signal. No real download yet (Phase 7
 *     wires the actual installer takeover).
 *
 * Other locked details:
 *   - "Remind Me Later" snoozes for 4 h via UpdateChecker::setSnoozedUntil()
 *     (wired in AppShellWindow, not here).
 *   - 20 % battery threshold blocks Install Now when a battery is present.
 *   - Active mission / active transfer / active upload also block Install Now
 *     with descriptive tooltip text (gate computed by AppShellWindow and
 *     passed in via GateState).
 *   - Release notes are parsed for "-" / "*" prefixed bullet lines, capped
 *     at 5; if no bullets parse out, we fall back to a muted
 *     "No release notes provided." line.
 *   - The modal is frameless (Qt::Dialog | Qt::FramelessWindowHint), modal
 *     against the application, and centered on its parent.
 *   - All colors come from f2c_cpp::uiThemeTokens to honor the global
 *     dark/light theme toggle (locked spec phase 4 = stick to project UI).
 */

#pragma once

#include <QDialog>
#include <QString>

#include "update/update_types.hpp"

class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace f2c_cpp {

class UpdateModal : public QDialog {
    Q_OBJECT

public:
    /// Snapshot of all install-blocking conditions, computed by AppShell at
    /// the moment the operator clicks View Details. The modal keeps
    /// re-applying these against the live battery reading every 5 s while
    /// visible so the Install Now button can re-enable if the battery
    /// crosses 20 % (e.g. operator plugs in AC).
    struct GateState {
        bool has_active_transfer = false;
        bool has_active_upload = false;
        bool has_active_mission = false;  // stage 5 active proxy
        // -1  → no /sys/class/power_supply/BAT* found → treat as AC, allow.
        // 0..100 → battery present, gate at <20 %.
        int  battery_pct = -1;
    };

    UpdateModal(const update::VersionInfo& info,
                const GateState& gate_state,
                bool dark_mode,
                QWidget* parent = nullptr);

    /// Refresh gate (e.g. after a transfer finished). Re-evaluates the
    /// Install Now enabled/disabled state and tooltip in place.
    void setGateState(const GateState& gate_state);

    /// Read the local battery percentage from
    /// /sys/class/power_supply/BAT*/capacity. Returns -1 if no battery
    /// node is found (AC-only device → install allowed per Q2=C). Returns
    /// -1 on read failure too (unknown is treated as AC, never blocks).
    /// Static so AppShellWindow can call it when computing the initial
    /// GateState before the modal exists.
    static int readBatteryPercent();

signals:
    /// Operator clicked "Remind Me Later". AppShell snoozes for 4 h and
    /// closes the modal.
    void remindMeLaterRequested(const f2c_cpp::update::VersionInfo& info);

    /// Operator clicked "Install Now" and the gate is currently clear.
    /// Phase 6 = signal-only (locked spec Q3=A); AppShell logs and the
    /// modal stays open. Phase 7 will wire the actual download/install.
    void installRequested(const f2c_cpp::update::VersionInfo& info);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void onPollBattery();
    void onInstallClicked();
    void onRemindMeLaterClicked();

private:
    void buildUi();
    void applyStyle();
    void populateContent();
    void refreshInstallButton();

    static QStringList parseReleaseNotesBullets(const QString& body,
                                                int max_bullets = 5);
    static QString formatSizeMb(qint64 bytes);
    static QString formatPublishedDate(const QString& iso8601);

    update::VersionInfo info_;
    GateState gate_;
    bool dark_mode_ = false;

    QTimer* battery_poll_timer_ = nullptr;

    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_subtitle_ = nullptr;
    QLabel* lbl_icon_tile_ = nullptr;
    QPushButton* btn_close_ = nullptr;

    QLabel* lbl_whats_new_title_ = nullptr;
    QVBoxLayout* whats_new_list_layout_ = nullptr;

    QLabel* lbl_warning_ = nullptr;

    QLabel* lbl_size_value_ = nullptr;
    QLabel* lbl_battery_value_ = nullptr;

    QPushButton* btn_remind_later_ = nullptr;
    QPushButton* btn_install_now_ = nullptr;
};

}  // namespace f2c_cpp
