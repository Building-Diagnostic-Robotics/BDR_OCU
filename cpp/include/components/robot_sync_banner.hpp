#pragma once

#include <QWidget>

#include "repo_sync_manager.hpp"

class QLabel;
class QPushButton;

namespace f2c_cpp {

class RobotSyncBanner : public QWidget {
    Q_OBJECT

public:
    explicit RobotSyncBanner(QWidget* parent = nullptr);

    void setDarkMode(bool dark_mode);
    void setSnapshot(const RepoSyncSnapshot& snap);
    const RepoSyncSnapshot& snapshot() const { return snap_; }

signals:
    void viewDetailsRequested();

private:
    void buildUi();
    void applyStyle();

    RepoSyncSnapshot snap_;
    bool dark_mode_ = false;
    QLabel* icon_tile_ = nullptr;
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_pill_ = nullptr;
    QLabel* lbl_subtitle_ = nullptr;
    QPushButton* btn_view_ = nullptr;
};

}  // namespace f2c_cpp
