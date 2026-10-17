/**
 * @file power_status.cpp
 * @brief See power_status.hpp.
 */
#include "update/power_status.hpp"

#include <QDir>
#include <QFile>
#include <QStringList>

#include <cmath>

namespace f2c_cpp::update {

namespace {

QString readTrimmed(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(f.readLine()).trimmed();
}

bool readLongLong(const QString& path, qint64* out) {
    bool ok = false;
    const qint64 v = readTrimmed(path).toLongLong(&ok);
    if (ok) {
        *out = v;
    }
    return ok;
}

/// Some kernels omit `type`; fall back to the usual node names.
QString supplyType(const QString& dir, const QString& name) {
    const QString type = readTrimmed(dir + QStringLiteral("/type"));
    if (!type.isEmpty()) {
        return type;
    }
    if (name.startsWith(QStringLiteral("BAT"))) {
        return QStringLiteral("Battery");
    }
    if (name.startsWith(QStringLiteral("AC")) ||
        name.startsWith(QStringLiteral("ADP"))) {
        return QStringLiteral("Mains");
    }
    return QString();
}

}  // namespace

PowerStatus readPowerStatus(const QString& sysfs_root) {
    PowerStatus out;
    QDir root(sysfs_root);
    if (!root.exists()) {
        return out;
    }
    // Entries are symlinks to device dirs; QDir::Dirs follows them.
    const QStringList names =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    qint64 now_sum = 0;
    qint64 full_sum = 0;
    int capacity_sum = 0;
    int capacity_count = 0;
    bool any_energy = false;

    for (const QString& name : names) {
        const QString dir = root.absoluteFilePath(name);
        const QString type = supplyType(dir, name);

        if (type == QStringLiteral("Mains") || type == QStringLiteral("USB")) {
            if (readTrimmed(dir + QStringLiteral("/online")) == QStringLiteral("1")) {
                out.on_ac = true;
            }
            continue;
        }
        if (type != QStringLiteral("Battery")) {
            continue;
        }
        // Wireless mouse / keyboard batteries report scope=Device.
        if (readTrimmed(dir + QStringLiteral("/scope")) == QStringLiteral("Device")) {
            continue;
        }
        const QString status = readTrimmed(dir + QStringLiteral("/status"));
        if (status == QStringLiteral("Charging") || status == QStringLiteral("Full") ||
            status == QStringLiteral("Not charging")) {
            out.on_ac = true;  // these states only occur with a charger present
        }

        qint64 now = 0;
        qint64 full = 0;
        const bool have_energy =
            (readLongLong(dir + QStringLiteral("/energy_now"), &now) &&
             readLongLong(dir + QStringLiteral("/energy_full"), &full)) ||
            (readLongLong(dir + QStringLiteral("/charge_now"), &now) &&
             readLongLong(dir + QStringLiteral("/charge_full"), &full));
        if (have_energy && full > 0) {
            now_sum += std::max<qint64>(0, now);
            full_sum += full;
            any_energy = true;
            continue;
        }
        qint64 cap = 0;
        if (readLongLong(dir + QStringLiteral("/capacity"), &cap) && cap >= 0 &&
            cap <= 100) {
            capacity_sum += static_cast<int>(cap);
            ++capacity_count;
        }
    }

    if (any_energy && capacity_count == 0) {
        out.battery_pct = static_cast<int>(
            std::lround(100.0 * static_cast<double>(now_sum) /
                        static_cast<double>(full_sum)));
    } else if (!any_energy && capacity_count > 0) {
        out.battery_pct = static_cast<int>(
            std::lround(static_cast<double>(capacity_sum) / capacity_count));
    } else if (any_energy && capacity_count > 0) {
        // Mixed reporting (rare): weight each pack equally.
        const double energy_pct =
            100.0 * static_cast<double>(now_sum) / static_cast<double>(full_sum);
        out.battery_pct = static_cast<int>(std::lround(
            (energy_pct + capacity_sum) / (1 + capacity_count)));
    }
    if (out.battery_pct > 100) {
        out.battery_pct = 100;
    }
    return out;
}

bool lowBatteryBlocks(const PowerStatus& status) {
    return !status.on_ac && status.battery_pct >= 0 &&
           status.battery_pct < kLowBatteryThresholdPct;
}

QString powerReadout(const PowerStatus& status) {
    if (status.battery_pct < 0) {
        return QStringLiteral("AC");
    }
    if (status.on_ac) {
        return QStringLiteral("Charging · %1%").arg(status.battery_pct);
    }
    if (lowBatteryBlocks(status)) {
        return QStringLiteral("%1%  (low)").arg(status.battery_pct);
    }
    return QStringLiteral("%1%").arg(status.battery_pct);
}

QString powerLogLine(const PowerStatus& status) {
    return QStringLiteral("power: on_ac=%1 battery_pct=%2 blocks=%3")
        .arg(status.on_ac ? 1 : 0)
        .arg(status.battery_pct)
        .arg(lowBatteryBlocks(status) ? 1 : 0);
}

}  // namespace f2c_cpp::update
