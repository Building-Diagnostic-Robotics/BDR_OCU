/**
 * @file power_status.hpp
 * @brief Laptop power state for the install / robot-sync battery gate.
 *
 * The gate exists so a laptop on a dying battery does not lose power half
 * way through an install. Reading only BAT0/capacity got that wrong twice:
 *   - it ignored the charger, so a laptop plugged in at 15 % stayed blocked;
 *   - it took the first battery only, and dual-battery laptops (Toughbook)
 *     charge one pack at a time, so BAT0 can sit low while BAT1 fills and
 *     the OS shows a healthy total.
 * This reads every supply under /sys/class/power_supply once per call: any
 * online Mains/USB supply (or a battery reporting Charging/Full/Not
 * charging) means plugged in, and the battery level is the combined level
 * across all system batteries, the way the OS shows it.
 *
 * Qt Core only, no GUI, so it is unit-tested against fake sysfs trees.
 */
#pragma once

#include <QString>

namespace f2c_cpp::update {

/// Installs and robot syncs are blocked on battery power below this.
constexpr int kLowBatteryThresholdPct = 20;

struct PowerStatus {
    /// A charger is connected (Mains/USB online, or a battery charging).
    bool on_ac = false;
    /// Combined charge of all system batteries, 0..100. -1 = no battery
    /// found or none readable (desktop / unknown: never blocks).
    int battery_pct = -1;

    bool operator==(const PowerStatus& o) const {
        return on_ac == o.on_ac && battery_pct == o.battery_pct;
    }
    bool operator!=(const PowerStatus& o) const { return !(*this == o); }
};

/// Read the current power state. `sysfs_root` is for tests.
PowerStatus readPowerStatus(
    const QString& sysfs_root = QStringLiteral("/sys/class/power_supply"));

/// True only when running on battery below the threshold. Plugged in, or
/// no readable battery, never blocks.
bool lowBatteryBlocks(const PowerStatus& status);

/// Short readout for the UI: "AC", "Charging · 15%", "62%", "15%  (low)".
QString powerReadout(const PowerStatus& status);

/// One-line form for the OTA log.
QString powerLogLine(const PowerStatus& status);

}  // namespace f2c_cpp::update
