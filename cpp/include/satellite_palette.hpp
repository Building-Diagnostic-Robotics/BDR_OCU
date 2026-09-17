/**
 * @file satellite_palette.hpp
 * @brief Paint colors for the satellite / measured canvases and the
 *        alignment picker. These are QPainter colors, not QSS — the widgets
 *        that use them draw rather than style, so they cannot be reached by
 *        a stylesheet.
 *
 * The split here is deliberate and is the rule for anything new:
 *
 *  - **Brand hues are fixed.** `accent`, `danger`, `warning` and `info` mark
 *    up the operator's geometry — ROI edges, roof-edge hazards, the ruler,
 *    the robot marker. Their job is to stand out against *satellite
 *    imagery*, which is the same photograph in either theme, so theming them
 *    would only make them worse. They are saturated enough to read on a
 *    light drafting surface too.
 *  - **Neutrals follow the theme.** `text`, `cardBg` and `border` are the
 *    chip plates, handle fills and label colors — chrome, not content. In
 *    light mode they have to invert or the operator reads white-on-white.
 *    They are views over `uiThemeTokens` so there is exactly one definition
 *    of the zinc ramp in the codebase.
 *
 * Chrome follows the theme even over imagery. A near-black chip is the more
 * obvious choice against an aerial photo, but the operator works on a roof
 * in direct sun, where the screen's black is grey with glare and light-on-
 * dark is the harder read. Light plates with dark text win there, and an
 * opaque plate plus a hairline keeps them off the photo regardless.
 */

#pragma once

#include <QColor>

#include "ui_theme_constants.hpp"

namespace f2c_cpp {
namespace satpal {

// Fixed brand hues — see the file comment before changing one.
inline QColor accent() { return QColor(0x00, 0xb3, 0x5a); }
inline QColor danger() { return QColor(0xff, 0x6b, 0x6b); }
inline QColor warning() { return QColor(0xF5, 0x9E, 0x0B); }
inline QColor info() { return QColor(0x38, 0x8B, 0xFD); }

// Theme-following neutrals.
inline QColor text() { return QColor(appThemeTokens().text); }
inline QColor cardBg() { return QColor(appThemeTokens().surface); }
inline QColor border() { return QColor(appThemeTokens().raised_border); }

/** Chip plate for a floating overlay — opaque enough to sit on imagery. */
inline QColor chipBg() {
    QColor c = cardBg();
    c.setAlpha(235);
    return c;
}

}  // namespace satpal
}  // namespace f2c_cpp
