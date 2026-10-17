#pragma once

#include <QDialog>

#include "components/robot_update_copy.hpp"
#include "repo_sync_manager.hpp"
#include "update/power_status.hpp"

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

namespace f2c_cpp {

/**
 * "ROOFUS Update" — the operator's view of a robot software sync.
 *
 * Plain words only (robot_update_copy.hpp); RepoSyncManager's technical
 * headline/detail appear only behind "Details for BDR support" after a
 * failure. A robot that needs the one-time deploy helpers is prepared and
 * then synced from the one Update Now press.
 */
class RobotSyncDialog : public QDialog {
    Q_OBJECT

public:
    /// Only the laptop battery can block here: the dialog opens from the
    /// Dashboard banner, which never shows while a scan is launching or
    /// running (AppShell's sync handlers refuse one anyway).
    struct GateState {
        update::PowerStatus power;
    };

    explicit RobotSyncDialog(QWidget* parent = nullptr);

    void setDarkMode(bool dark);
    void setSnapshot(const RepoSyncSnapshot& snap);
    void setGateState(const GateState& gate);
    void setBusy(bool busy, const QString& stage = QString());
    void setFinished(int level, const QString& headline, const QString& detail);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

signals:
    void syncRequested();
    void prepareRequested();
    void laterRequested();

private:
    enum class Phase { Offer, Busy, Finished };

    void buildUi();
    void applyStyle();
    void render();
    /// Showing "ROOFUS is up to date." (primary closes).
    bool isDoneView() const;
    void refreshButtons();
    void onPrimary();
    /// Re-read the charger/battery while open, so plugging in re-enables
    /// Update Now without closing and reopening the dialog.
    void pollPower();

    RepoSyncSnapshot snap_;
    GateState gate_;
    bool dark_mode_ = true;
    Phase phase_ = Phase::Offer;
    QString stage_;
    int finished_level_ = 0;
    QString tech_headline_;
    QString tech_detail_;
    /// Update Now on a robot without deploy helpers: prepare, then sync.
    bool sync_after_prepare_ = false;
    /// The run in progress / just finished was started here (Update Now),
    /// not a background status check.
    bool operator_run_ = false;
    QTimer* power_poll_timer_ = nullptr;

    QWidget* container_ = nullptr;
    QLabel* lbl_icon_ = nullptr;
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_subtitle_ = nullptr;
    QPushButton* btn_x_ = nullptr;
    QLabel* lbl_message_ = nullptr;
    QLabel* lbl_notice_ = nullptr;
    QLabel* lbl_step_ = nullptr;
    QProgressBar* busy_bar_ = nullptr;
    QPushButton* btn_details_ = nullptr;
    QLabel* lbl_details_ = nullptr;
    QPushButton* btn_secondary_ = nullptr;
    QPushButton* btn_primary_ = nullptr;
};

}  // namespace f2c_cpp
