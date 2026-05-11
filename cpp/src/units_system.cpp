/**
 * @file units_system.cpp
 * @brief Display-only unit system implementation.
 */

#include "units_system.hpp"

#include "settings_constants.hpp"

#include <QCoreApplication>
#include <QSettings>
#include <QString>

namespace f2c_cpp {

// ---------------------------------------------------------------------------
// UnitsProvider — process-wide singleton
// ---------------------------------------------------------------------------

UnitsProvider* UnitsProvider::instance() {
    // Tied to QCoreApplication's lifetime so QSettings is always valid
    // while we exist. `Q_GLOBAL_STATIC` would also work but a static
    // pointer makes the parent ownership explicit.
    static UnitsProvider* s_instance = nullptr;
    if (!s_instance) {
        s_instance = new UnitsProvider(QCoreApplication::instance());
    }
    return s_instance;
}

UnitsProvider::UnitsProvider(QObject* parent) : QObject(parent) {
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    units_ = units::fromString(settings.value(kSettingsUnitsKey,
                                              units::toString(Units::Metric))
                                   .toString());
}

void UnitsProvider::setUnits(Units u) {
    if (u == units_) {
        return;
    }
    units_ = u;
    {
        QSettings settings(kSettingsOrgName, kSettingsAppName);
        settings.setValue(kSettingsUnitsKey, units::toString(u));
    }
    emit unitsChanged(units_);
}

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

namespace units {

QString toString(Units u) {
    return u == Units::Ansi ? QStringLiteral("ansi") : QStringLiteral("metric");
}

Units fromString(const QString& s) {
    return s.trimmed().compare(QStringLiteral("ansi"), Qt::CaseInsensitive) == 0
               ? Units::Ansi
               : Units::Metric;
}

namespace {

QString formatNumber(double value, int decimals) {
    return QString::number(value, 'f', decimals < 0 ? 0 : decimals);
}

}  // namespace

QString formatLength(double meters, int decimals) {
    if (UnitsProvider::instance()->isMetric()) {
        return QStringLiteral("%1 m").arg(formatNumber(meters, decimals));
    }
    return QStringLiteral("%1 ft").arg(formatNumber(metersToFeet(meters), decimals));
}

QString formatSpeed(double meters_per_second, int decimals) {
    if (UnitsProvider::instance()->isMetric()) {
        return QStringLiteral("%1 m/s").arg(formatNumber(meters_per_second, decimals));
    }
    return QStringLiteral("%1 ft/s")
        .arg(formatNumber(metersToFeet(meters_per_second), decimals));
}

QString formatArea(double square_meters, int decimals) {
    if (UnitsProvider::instance()->isMetric()) {
        return QStringLiteral("%1 m²").arg(formatNumber(square_meters, decimals));
    }
    return QStringLiteral("%1 ft²")
        .arg(formatNumber(square_meters * kSqFeetPerSqMeter, decimals));
}

QString lengthUnitSuffix() {
    return UnitsProvider::instance()->isMetric() ? QStringLiteral(" m")
                                                  : QStringLiteral(" ft");
}

QString speedUnitSuffix() {
    return UnitsProvider::instance()->isMetric() ? QStringLiteral(" m/s")
                                                  : QStringLiteral(" ft/s");
}

QString areaUnitSuffix() {
    return UnitsProvider::instance()->isMetric() ? QStringLiteral(" m²")
                                                  : QStringLiteral(" ft²");
}

}  // namespace units

}  // namespace f2c_cpp
