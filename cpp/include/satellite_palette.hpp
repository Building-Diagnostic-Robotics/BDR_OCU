/**
 * @file satellite_palette.hpp
 * @brief Fixed overlay colors for the satellite map stage. These paint ON
 *        TOP of satellite imagery (always dark), so they are intentionally
 *        theme-independent — aligned with the dark-mode uiThemeTokens.
 */

#pragma once

#include <QColor>

namespace f2c_cpp {
namespace satpal {

inline QColor accent() { return QColor(0x00, 0xb3, 0x5a); }
inline QColor danger() { return QColor(0xff, 0x6b, 0x6b); }
inline QColor warning() { return QColor(0xF5, 0x9E, 0x0B); }
inline QColor info() { return QColor(0x38, 0x8B, 0xFD); }
inline QColor text() { return QColor(0xF3, 0xF4, 0xF6); }
inline QColor cardBg() { return QColor(0x11, 0x18, 0x27); }
inline QColor border() { return QColor(0x37, 0x41, 0x51); }

}  // namespace satpal
}  // namespace f2c_cpp
