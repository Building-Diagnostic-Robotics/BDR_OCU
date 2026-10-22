#pragma once

#include <QString>

namespace f2c_cpp {

/**
 * One tooltip look for the whole OCU, from the theme tokens: card fill,
 * hairline border, body text. Replaces Fusion's yellow box.
 *
 * Call once the QApplication exists and on every light/dark switch
 * (main() seeds it, AppShellWindow::setDarkMode keeps it current).
 *
 * Every tooltip gets the rule as its own style sheet, so a catch-all rule on
 * the widget it was shown for (a label's bare `color: white`, a dialog's
 * `QLabel { background: transparent }`) cannot restyle it.
 */
void applyTooltipTheme(bool dark_mode);

/// The rule each tooltip carries for `dark_mode`.
QString tooltipStyleSheet(bool dark_mode);

}  // namespace f2c_cpp
