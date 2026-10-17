#pragma once

#include <QDialog>

#include "repo_sync_manager.hpp"
#include "update/power_status.hpp"

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

namespace f2c_cpp {

class RobotSyncDialog : public QDialog {
    Q_OBJECT

public:
    struct GateState {
        bool launch_active = false;
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
    void buildUi();
    void applyStyle();
    void refreshButtons();
    /// Re-read the charger/battery while open, so plugging in re-enables
    /// Sync without closing and reopening the dialog.
    void pollPower();

    RepoSyncSnapshot snap_;
    GateState gate_;
    bool dark_mode_ = true;
    bool busy_ = false;
    QTimer* power_poll_timer_ = nullptr;

    QWidget* container_ = nullptr;
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_headline_ = nullptr;
    QLabel* lbl_detail_ = nullptr;
    QLabel* lbl_stage_ = nullptr;
    QProgressBar* busy_bar_ = nullptr;
    QPushButton* btn_sync_ = nullptr;
    QPushButton* btn_prepare_ = nullptr;
    QPushButton* btn_later_ = nullptr;
    QPushButton* btn_close_ = nullptr;
};

}  // namespace f2c_cpp
