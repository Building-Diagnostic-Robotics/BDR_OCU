/**
 * @file rollback_banner.hpp
 * @brief Persistent advisory banner shown when Phase 9 rolled the OCU
 *        back to a previous version (or attempted to).
 *
 * Lives alongside `UpdateBanner` in `AppShellWindow`'s top-of-stack
 * region. Amber styling per `uiThemeTokens.warning` so it reads as an
 * advisory, not as a recoverable "tap to install" call-to-action.
 *
 * Visibility lifecycle:
 *   - Shown by `AppShellWindow::showRolledBackBanner()` when the
 *     startup-time marker dispatch reads `UpdateStage::RolledBack`.
 *   - Dismissed by the operator clicking "Dismiss" → emits
 *     `dismissRequested()`. Wired by `AppShellWindow` to clear the
 *     `update_state.json` marker and hide the banner.
 *
 * The widget is dumb: no marker IO, no checker plumbing.
 */

#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

namespace f2c_cpp {

class RollbackBanner : public QWidget {
    Q_OBJECT

public:
    explicit RollbackBanner(QWidget* parent = nullptr);

    /// Apply the project's dark/light theme tokens.
    void setDarkMode(bool dark_mode);
    bool isDarkMode() const { return dark_mode_; }

    /// Optional one-line context. Empty string falls back to the default
    /// "Update was rolled back. Contact support if this persists."
    void setMessage(const QString& message);

signals:
    /// Operator clicked "Dismiss". The owner is expected to clear the
    /// `update_state.json` marker and `hide()` this widget.
    void dismissRequested();

private slots:
    void onDismissClicked();

private:
    void buildUi();
    void applyStyle();

    bool dark_mode_ = false;

    QLabel* icon_tile_  = nullptr;
    QLabel* lbl_title_   = nullptr;
    QLabel* lbl_subtitle_ = nullptr;
    QPushButton* btn_dismiss_ = nullptr;
};

}  // namespace f2c_cpp
