/**
 * @file ui_zoom.hpp
 * @brief App-wide zoom (A- / A+ on Get Started).
 *
 * Text sizes are written in px across the app's style sheets and many
 * widgets have fixed sizes, so text-only scaling would clip. Instead the
 * whole UI is scaled with Qt's QT_SCALE_FACTOR: text, buttons, icons and
 * spacing grow together and every layout keeps its shape. Qt reads the
 * factor once when the QApplication is created, so a change applies after
 * a restart (Get Started shows "Restart to apply").
 *
 * The ceiling is set by the field laptop (14", 1920x1080, ~1046 px work
 * area under the GNOME top bar). The scan page's right rail needs a ~905 px
 * tall window (see the dev screenshot note in app_shell.cpp): 1.15 leaves
 * ~910 px, anything larger squeezes it. Raise kUiZoomLevels only after that
 * page handles less height.
 */

#pragma once

#include <QByteArray>
#include <QSettings>
#include <QString>
#include <QVariant>
#include <QtGlobal>

#include <array>
#include <cmath>

#include "settings_constants.hpp"

namespace f2c_cpp {

struct UiZoomLevel {
    double factor;
    const char* label;  // operator-facing
};

inline constexpr std::array<UiZoomLevel, 3> kUiZoomLevels{{
    {1.0, "Normal"},
    {1.1, "Large"},
    {1.15, "Larger"},
}};

inline constexpr double kUiZoomDefault = 1.0;

/// Set next to QT_SCALE_FACTOR when the app sets it, so a process started
/// from this one (Restart to apply, the update runner's relaunch) re-reads the
/// saved choice instead of mistaking the inherited value for a dev override.
inline constexpr const char* kUiZoomAppliedEnv = "BDR_UI_ZOOM_APPLIED";

/// QApplication dynamic property: the zoom this process started with.
inline constexpr const char* kUiZoomActiveProperty = "bdr_ui_zoom_active";

/// QApplication dynamic property: set when the operator pressed Restart to apply;
/// main() starts a new copy after the event loop ends.
inline constexpr const char* kRestartRequestedProperty = "bdr_restart_requested";

/// Nearest allowed level; anything unreadable is the default.
inline double snapUiZoom(double factor) {
    if (!std::isfinite(factor)) return kUiZoomDefault;
    double best = kUiZoomDefault;
    double best_dist = 1e9;
    for (const UiZoomLevel& level : kUiZoomLevels) {
        const double dist = std::fabs(level.factor - factor);
        if (dist < best_dist) {
            best_dist = dist;
            best = level.factor;
        }
    }
    return best;
}

/// The zoom to start with. A QT_SCALE_FACTOR set outside the app (a dev
/// shell) wins; otherwise the saved choice.
struct UiZoomStartup {
    double factor = kUiZoomDefault;
    bool external_override = false;
};

inline UiZoomStartup resolveStartupUiZoom(const QByteArray& env_scale_factor,
                                          bool env_set_by_app,
                                          const QVariant& saved) {
    UiZoomStartup out;
    if (!env_scale_factor.trimmed().isEmpty() && !env_set_by_app) {
        bool ok = false;
        const double f = env_scale_factor.trimmed().toDouble(&ok);
        out.factor = (ok && f > 0.0) ? f : kUiZoomDefault;
        out.external_override = true;
        return out;
    }
    bool ok = false;
    const double f = saved.toDouble(&ok);
    out.factor = ok ? snapUiZoom(f) : kUiZoomDefault;
    return out;
}

inline double savedUiZoom() {
    const QSettings settings(QString::fromLatin1(kSettingsOrgName),
                             QString::fromLatin1(kSettingsAppName));
    return snapUiZoom(settings.value(QString::fromLatin1(kSettingsUiZoomKey),
                                     kUiZoomDefault).toDouble());
}

inline void saveUiZoom(double factor) {
    QSettings settings(QString::fromLatin1(kSettingsOrgName),
                       QString::fromLatin1(kSettingsAppName));
    settings.setValue(QString::fromLatin1(kSettingsUiZoomKey), snapUiZoom(factor));
}

/// Call in main() before the QApplication is created. Returns the zoom
/// the process will run at.
inline double applyUiZoomBeforeQApplication() {
    const QSettings settings(QString::fromLatin1(kSettingsOrgName),
                             QString::fromLatin1(kSettingsAppName));
    const UiZoomStartup z = resolveStartupUiZoom(
        qgetenv("QT_SCALE_FACTOR"),
        qEnvironmentVariableIsSet(kUiZoomAppliedEnv),
        settings.value(QString::fromLatin1(kSettingsUiZoomKey)));
    if (z.external_override) return z.factor;
    if (qFuzzyCompare(z.factor, 1.0)) {
        qunsetenv("QT_SCALE_FACTOR");
    } else {
        qputenv("QT_SCALE_FACTOR", QByteArray::number(z.factor, 'f', 2));
    }
    qputenv(kUiZoomAppliedEnv, QByteArrayLiteral("1"));
    return z.factor;
}

}  // namespace f2c_cpp
