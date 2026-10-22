#pragma once

#include <QRegularExpression>
#include <QString>
#include <QtGlobal>
#include <cmath>

#include "settings_constants.hpp"

namespace f2c_cpp {

/// Text size steps for the OTA update modal. The operator can change them
/// with A- / A+ on the modal; the choice is kept in QSettings.
inline constexpr double kUpdateTextScaleMin = 0.9;
inline constexpr double kUpdateTextScaleMax = 1.6;
inline constexpr double kUpdateTextScaleStep = 0.1;
inline constexpr double kUpdateTextScaleDefault = 1.0;

/// Clamp to the allowed range and snap to the 0.1 grid, so repeated
/// A- / A+ presses never drift (0.1 + 0.1 + ... stays exact).
inline double clampUpdateTextScale(double scale) {
    if (!std::isfinite(scale)) return kUpdateTextScaleDefault;
    const double snapped = std::round(scale * 10.0) / 10.0;
    return qBound(kUpdateTextScaleMin, snapped, kUpdateTextScaleMax);
}

/// Multiplies every `font-size: <n>px` in a style sheet by `scale`, rounded
/// to whole pixels, and leaves everything else untouched. Sizes are written
/// once at their base value, so the modal's look at scale 1.0 is the one
/// that was designed.
inline QString scaleCssFontSizes(const QString& css, double scale) {
    static const QRegularExpression kFontSize(
        QStringLiteral("font-size:\\s*(\\d+)px"));
    QString out;
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = kFontSize.globalMatch(css);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += css.mid(last, m.capturedStart(0) - last);
        const int base = m.captured(1).toInt();
        out += QStringLiteral("font-size: %1px")
                   .arg(static_cast<int>(std::lround(base * scale)));
        last = m.capturedEnd(0);
    }
    out += css.mid(last);
    return out;
}

}  // namespace f2c_cpp
