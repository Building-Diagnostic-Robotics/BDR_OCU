/**
 * @file units_system.hpp
 * @brief Display-only unit system (Metric / ANSI) and string formatters.
 *
 * Single source of truth for whether the OCU shows lengths/speeds/areas in
 * meters or feet. EVERYTHING on the wire stays SI — this header only affects
 * `QString` building for labels, value cards, spinbox suffixes, etc.
 *
 * Architecture:
 *   - `Units` enum: Metric or Ansi.
 *   - `UnitsProvider`: thread-affine `QObject` singleton (`instance()`).
 *     Owns the current selection, persists it to QSettings under
 *     `f2c_cpp::kSettingsUnitsKey`, and emits `unitsChanged()` whenever it
 *     flips. Widgets connect to that signal to relabel live.
 *   - Free helpers in the `units` namespace: `formatLength`, `formatSpeed`,
 *     `formatArea`, plus the inverse `feetToMeters`/`metersToFeet` for
 *     operator-edited spinbox values.
 *
 * Rules for callers:
 *   - Display sites: read `UnitsProvider::instance()->units()` and call
 *     `units::formatXxx(meters)`. Subscribe to `unitsChanged()` for refresh.
 *   - Input sites (operator edits a spinbox in feet/ft, etc.): convert with
 *     `units::feetToMeters()` at the read site BEFORE the value reaches
 *     `CoverageConfig`, ROS, presets, or anything persisted. Stored values
 *     are always SI.
 *   - Never call the formatter inside `app_shell.cpp` ROS publish paths,
 *     `coverage_pipeline.cpp`, or `preset_manager.cpp` — those layers must
 *     stay unit-system-agnostic.
 */

#pragma once

#include <QObject>
#include <QString>

namespace f2c_cpp {

enum class Units {
    Metric,  ///< meters, m/s, m^2 — default on fresh QSettings.
    Ansi,    ///< feet, ft/s, ft^2 — operator-facing convenience only.
};

/**
 * @brief Process-wide selector for display units.
 *
 * Lazily constructed singleton. Owned by `QCoreApplication` so it tears
 * down with the event loop. Safe to access from any GUI-thread context;
 * not designed for cross-thread mutation (use `Qt::QueuedConnection` to
 * touch it from a ROS callback).
 */
class UnitsProvider : public QObject {
    Q_OBJECT

public:
    /** Returns the process-wide singleton. */
    static UnitsProvider* instance();

    /** Current selection. */
    Units units() const { return units_; }

    /** Convenience predicate. */
    bool isMetric() const { return units_ == Units::Metric; }

    /**
     * @brief Update the current selection, persist to QSettings, and emit
     *        `unitsChanged()` if the value actually changed.
     */
    void setUnits(Units units);

signals:
    /** Fired exactly once per real change. */
    void unitsChanged(Units units);

private:
    explicit UnitsProvider(QObject* parent = nullptr);

    Units units_ = Units::Metric;
};

namespace units {

// ---------- Enum ↔ persistence string ----------

/** Lowercase canonical string used in QSettings + JSON metadata. */
QString toString(Units u);

/**
 * @brief Parse a string written by `toString`. Recognises `"metric"` and
 *        `"ansi"` case-insensitively. Anything else returns Metric.
 */
Units fromString(const QString& s);

// ---------- Hard conversions (operator-input flow only) ----------

constexpr double kFeetPerMeter = 3.28083989501;
constexpr double kSqFeetPerSqMeter = kFeetPerMeter * kFeetPerMeter;

inline double metersToFeet(double m) { return m * kFeetPerMeter; }
inline double feetToMeters(double ft) { return ft / kFeetPerMeter; }

// ---------- Display formatters (read-only flow) ----------
//
// All formatters take SI inputs and consult `UnitsProvider::instance()`
// for the current selection, so call sites never need to branch.
// `decimals` controls the post-decimal precision of the rendered number.

/** e.g. "1.83 m" or "6.00 ft". */
QString formatLength(double meters, int decimals = 2);

/** e.g. "0.50 m/s" or "1.64 ft/s". */
QString formatSpeed(double meters_per_second, int decimals = 2);

/** e.g. "12.30 m²" or "132.40 ft²". */
QString formatArea(double square_meters, int decimals = 2);

/** Just the unit suffix string for `QDoubleSpinBox::setSuffix(" m")` etc. */
QString lengthUnitSuffix();
QString speedUnitSuffix();
QString areaUnitSuffix();

}  // namespace units

}  // namespace f2c_cpp
