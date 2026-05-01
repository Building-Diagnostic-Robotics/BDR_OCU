/**
 * @file ui_theme_constants.hpp
 * @brief Canonical design tokens for the staged AppShell flow.
 *
 * Design ideology: Safety-First Hierarchy, modern Figma-style dark mode,
 * heavily utilizing custom QSS. All scan workflow stages (Exploration, Planning,
 * Execution) must use these tokens for visual consistency.
 */

#pragma once

#include <QString>

namespace f2c_cpp {

struct UiThemeTokens {
    QString bg;
    QString card_bg;
    QString border;
    QString text;
    QString muted;
    QString accent;
    QString accent_hover;
    QString danger;
    QString log_bg;  // for log viewers, FPV placeholder backgrounds
};

inline UiThemeTokens uiThemeTokens(bool dark_mode) {
    if (dark_mode) {
        return {
            QStringLiteral("#0b0b0b"),   // bg
            QStringLiteral("#111827"),   // card_bg
            QStringLiteral("#374151"),   // border
            QStringLiteral("#F3F4F6"),   // text
            QStringLiteral("#9CA3AF"),   // muted
            QStringLiteral("#00b35a"),   // accent (green)
            QStringLiteral("#00cc66"),   // accent_hover
            QStringLiteral("#ff6b6b"),   // danger (red)
            QStringLiteral("#0f172a"),   // log_bg
        };
    } else {
        return {
            QStringLiteral("#F9FAFB"),   // bg
            QStringLiteral("#FFFFFF"),   // card_bg
            QStringLiteral("#D1D5DC"),   // border
            QStringLiteral("#111827"),   // text
            QStringLiteral("#6B7280"),   // muted
            QStringLiteral("#155DFC"),   // accent (blue)
            QStringLiteral("#1D4ED8"),   // accent_hover
            QStringLiteral("#EF4444"),   // danger
            QStringLiteral("#F1F5F9"),   // log_bg
        };
    }
}

}  // namespace f2c_cpp
