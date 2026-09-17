/**
 * @file ui_theme_constants.hpp
 * @brief Canonical design tokens for the staged AppShell flow.
 *
 * Design ideology: Safety-First Hierarchy, modern Figma-style dark mode,
 * heavily utilizing custom QSS. All scan workflow stages (Exploration, Planning,
 * Execution) must use these tokens for visual consistency.
 *
 * Light mode is the outdoor theme — the operator reads it on a washed-out
 * screen in direct sun. Every light text value here clears 7:1 against the
 * surface it is specified for, because the labels that use them are 12-14 px.
 * The three text tiers (text / body / muted) are 16:1 / 10:1 / 7.7:1 on white,
 * so the hierarchy survives the higher floor. `accent_green` is brand-locked
 * and only reaches 2.5:1 against white, which is why `on_accent` goes dark in
 * light mode instead of the green being darkened.
 */

#pragma once

#include <QCoreApplication>
#include <QString>
#include <QVariant>

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
    QString warning; // amber for non-blocking advisory callouts (OTA modal etc.)

    // Zinc ramp — the design language the staged screens and the frameless
    // dialogs actually ship. The fields above predate it and still carry the
    // older slate/blue values; they are migrated per phase, not all at once.
    QString surface;         // dialog / card fill sitting on `bg`
    QString surface_border;  // hairline around `surface`
    QString raised;          // input / table-row / header fill above `surface`
    QString raised_border;   // hairline on `raised`, and neutral button fill
    QString neutral_hover;   // hover for `raised` rows and neutral buttons
    QString hover_wash;      // translucent hover for transparent/outlined
                             // controls, where an opaque fill would be loud
    QString body;            // body copy — one tier above `muted`
    // Tertiary text: hints, unit suffixes, section headers. Dark mode has
    // room for a fourth grey below `muted`; light mode does not — anything
    // lighter than `muted` on white drops under the readable-in-sun bar. So
    // in light mode `faint` IS `muted`, and the hierarchy those labels need
    // comes from size and weight instead. Do not lighten it to "get the
    // tier back".
    QString faint;
    // A label sitting ON a disabled button fill. Weak enough to read as
    // unavailable, strong enough that the operator can still tell what the
    // button they cannot press says.
    QString disabled_text;
    // A label on a disabled *transparent* control (ghost buttons). Needs a
    // different value from `disabled_text`: with no fill under it, the same
    // color would look enabled.
    QString disabled_ghost;
    QString overlay_bg;      // chips/tools floating over imagery or video
    QString overlay_border;  // hairline that keeps an overlay's edge legible
    QString accent_green;    // brand green; identical in both themes
    QString accent_green_hover;
    QString on_accent;       // text/icon color that sits ON accent_green
    QString accent_text;     // green as TEXT on a surface — accent_green is
                             // a fill and only reaches 2.5:1 unfilled

    // Destructive / cautionary BUTTON FILLS. Distinct from `danger` and
    // `warning`, which are text colors: a fill carries a white label, so it
    // has to get darker in light mode, where a text color gets darker for
    // the opposite reason. Using one value for both roles fails one of them.
    QString danger_fill;
    QString danger_fill_hover;
    QString warning_fill;
    QString warning_fill_hover;
    QString info;            // informational text (blue); not `accent`, which
                             // is green in dark mode and would change meaning
    QString info_fill;
    QString info_fill_hover;
};

inline UiThemeTokens uiThemeTokens(bool dark_mode) {
    if (dark_mode) {
        return {
            QStringLiteral("#0b0b0b"),   // bg
            QStringLiteral("#111827"),   // card_bg
            QStringLiteral("#374151"),   // border
            QStringLiteral("#F3F4F6"),   // text
            QStringLiteral("#9F9FA9"),   // muted
            QStringLiteral("#00b35a"),   // accent (green)
            QStringLiteral("#00cc66"),   // accent_hover
            QStringLiteral("#ff6b6b"),   // danger (red)
            QStringLiteral("#0f172a"),   // log_bg
            QStringLiteral("#F59E0B"),   // warning (amber)

            QStringLiteral("#18181b"),               // surface
            QStringLiteral("#27272a"),               // surface_border
            QStringLiteral("#27272a"),               // raised
            QStringLiteral("#3f3f47"),               // raised_border
            QStringLiteral("#4a4a52"),               // neutral_hover
            QStringLiteral("rgba(255, 255, 255, 0.05)"), // hover_wash
            QStringLiteral("#D4D4D8"),               // body
            QStringLiteral("#71717B"),               // faint
            QStringLiteral("rgba(255, 255, 255, 0.40)"), // disabled_text
            QStringLiteral("rgba(113, 113, 123, 0.40)"), // disabled_ghost
            QStringLiteral("rgba(24, 24, 27, 0.90)"),// overlay_bg
            QStringLiteral("#3f3f47"),               // overlay_border
            QStringLiteral("#00BC7D"),               // accent_green
            QStringLiteral("#0ACB8B"),               // accent_green_hover
            QStringLiteral("#FFFFFF"),               // on_accent
            QStringLiteral("#00D492"),               // accent_text

            QStringLiteral("#B91C1C"),               // danger_fill
            QStringLiteral("#DC2626"),               // danger_fill_hover
            QStringLiteral("#B45309"),               // warning_fill
            QStringLiteral("#D97706"),               // warning_fill_hover
            QStringLiteral("#51A2FF"),               // info
            QStringLiteral("#2563EB"),               // info_fill
            QStringLiteral("#3B82F6"),               // info_fill_hover
        };
    } else {
        return {
            QStringLiteral("#F9FAFB"),   // bg
            QStringLiteral("#FFFFFF"),   // card_bg
            QStringLiteral("#D1D5DC"),   // border
            QStringLiteral("#111827"),   // text
            QStringLiteral("#52525B"),   // muted
            QStringLiteral("#155DFC"),   // accent (blue)
            QStringLiteral("#1D4ED8"),   // accent_hover
            QStringLiteral("#991B1B"),   // danger
            QStringLiteral("#F1F5F9"),   // log_bg
            QStringLiteral("#92400E"),   // warning (amber, deeper for light bg)

            QStringLiteral("#FFFFFF"),                  // surface
            QStringLiteral("#E4E4E7"),                  // surface_border
            QStringLiteral("#F4F4F5"),                  // raised
            QStringLiteral("#D4D4D8"),                  // raised_border
            QStringLiteral("#E4E4E7"),                  // neutral_hover
            QStringLiteral("rgba(0, 0, 0, 0.05)"),      // hover_wash
            QStringLiteral("#3F3F46"),                  // body
            QStringLiteral("#52525B"),                  // faint (== muted)
            QStringLiteral("#5F5F6A"),                  // disabled_text
            QStringLiteral("#A1A1AA"),                  // disabled_ghost
            QStringLiteral("rgba(255, 255, 255, 0.95)"),// overlay_bg
            QStringLiteral("#D4D4D8"),                  // overlay_border
            QStringLiteral("#00BC7D"),                  // accent_green
            QStringLiteral("#00A86D"),                  // accent_green_hover
            QStringLiteral("#18181B"),                  // on_accent
            QStringLiteral("#065F46"),                  // accent_text

            QStringLiteral("#991B1B"),                  // danger_fill
            QStringLiteral("#B91C1C"),                  // danger_fill_hover
            QStringLiteral("#92400E"),                  // warning_fill
            QStringLiteral("#B45309"),                  // warning_fill_hover
            QStringLiteral("#1E40AF"),                  // info
            QStringLiteral("#1E40AF"),                  // info_fill
            QStringLiteral("#2563EB"),                  // info_fill_hover
        };
    }
}

/**
 * Process-wide current theme, so a widget can style itself without the
 * construction site having to know or pass the flag. Carried on the
 * QApplication instance rather than a singleton object because it needs no
 * signal: the surfaces that read it are application-modal dialogs, and the
 * theme toggle lives in the main window's title bar, so the theme cannot
 * change while one of them is on screen.
 *
 * `main()` seeds this from QSettings before the shell exists;
 * `AppShellWindow::setDarkMode` keeps it in sync afterwards.
 */
inline void setAppDarkMode(bool dark) {
    if (auto* app = QCoreApplication::instance()) {
        app->setProperty("bdrDarkMode", dark);
    }
}

inline bool appDarkMode() {
    auto* app = QCoreApplication::instance();
    return app && app->property("bdrDarkMode").toBool();
}

inline UiThemeTokens appThemeTokens() { return uiThemeTokens(appDarkMode()); }

}  // namespace f2c_cpp
