#pragma once

namespace f2c_cpp {

// Geometry of the frameless window controls (theme toggle, minimize,
// maximize, close) that AppShellWindow floats over the top-right corner
// of every stage. Screens whose top bar runs under them reserve
// kWindowControlsFootprint so their right-most chip is not covered.
inline constexpr int kWindowThemeToggleWidth = 48;
inline constexpr int kWindowThemeToggleHeight = 24;
inline constexpr int kWindowButtonSize = 32;
inline constexpr int kWindowButtonCount = 3;
inline constexpr int kWindowControlsSpacing = 4;
inline constexpr int kWindowToggleToButtonsGap = 32;
inline constexpr int kWindowControlsRightMargin = 24;

// Layout items: toggle, gap spacer, three buttons. The row spacing sits
// between each neighbouring pair, including around the spacer.
inline constexpr int kWindowControlsWidth =
    kWindowThemeToggleWidth + kWindowToggleToButtonsGap +
    kWindowButtonCount * kWindowButtonSize +
    (kWindowButtonCount + 1) * kWindowControlsSpacing;

// Distance from the window's right edge to the controls' left edge.
inline constexpr int kWindowControlsFootprint =
    kWindowControlsWidth + kWindowControlsRightMargin;

// Clear space between a top bar's last chip and the theme toggle. Stage
// 4/5 already land at this through their 24 px top-right spacing.
inline constexpr int kWindowControlsClearance = 16;

}  // namespace f2c_cpp
