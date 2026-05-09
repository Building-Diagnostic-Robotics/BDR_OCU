/**
 * @file update_banner.hpp
 * @brief Slim "System Update Available" banner shown above the stage stack.
 *
 * Mounted by AppShellWindow at index 0 of its central root layout, so it
 * persists across stage transitions. Visibility is driven by
 * UpdateChecker::updateAvailable / noUpdateAvailable.
 *
 * This widget is dumb: it renders the VersionInfo it's handed and emits a
 * signal when the operator clicks "View Details". All state (snooze, last
 * seen SHA, etc.) lives on UpdateChecker.
 */

#pragma once

#include <QWidget>

#include "update/update_types.hpp"

class QLabel;
class QPushButton;

namespace f2c_cpp {

class UpdateBanner : public QWidget {
    Q_OBJECT

public:
    explicit UpdateBanner(QWidget* parent = nullptr);

    /// Apply the project's dark/light theme tokens to all sub-widgets.
    void setDarkMode(bool dark_mode);
    bool isDarkMode() const { return dark_mode_; }

    /// Refresh the banner's text content from a freshly polled release.
    /// Does NOT toggle visibility — call show()/hide() explicitly.
    void setVersionInfo(const update::VersionInfo& info);

    const update::VersionInfo& versionInfo() const { return version_info_; }

signals:
    /// Operator clicked "View Details". Phase 4 has no consumer for the
    /// signal (per locked spec Q1=B); phase 6 wires this to the modal.
    void viewDetailsRequested(const f2c_cpp::update::VersionInfo& info);

private slots:
    void onViewDetailsClicked();

private:
    void buildUi();
    void applyStyle();
    QString formatSizeMb(qint64 bytes) const;

    update::VersionInfo version_info_;
    bool dark_mode_ = false;

    QLabel* icon_tile_ = nullptr;
    QLabel* lbl_title_ = nullptr;
    QLabel* lbl_version_pill_ = nullptr;
    QLabel* lbl_subtitle_ = nullptr;
    QPushButton* btn_view_details_ = nullptr;
};

}  // namespace f2c_cpp
