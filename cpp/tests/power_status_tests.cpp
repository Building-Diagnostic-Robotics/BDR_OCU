// power_status: install / robot-sync battery gate against fake sysfs trees.
#include "update/power_status.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using f2c_cpp::update::PowerStatus;
using f2c_cpp::update::lowBatteryBlocks;
using f2c_cpp::update::powerReadout;
using f2c_cpp::update::readPowerStatus;

namespace {

class FakeSysfs {
public:
    FakeSysfs() { EXPECT_TRUE(dir_.isValid()); }
    QString root() const { return dir_.path(); }
    void set(const QString& supply, const QString& file, const QString& value) {
        QDir().mkpath(dir_.path() + "/" + supply);
        QFile f(dir_.path() + "/" + supply + "/" + file);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write((value + "\n").toUtf8());
    }
    void battery(const QString& name, int capacity, const QString& status) {
        set(name, "type", "Battery");
        set(name, "capacity", QString::number(capacity));
        set(name, "status", status);
    }
    void mains(const QString& name, bool online) {
        set(name, "type", "Mains");
        set(name, "online", online ? "1" : "0");
    }

private:
    QTemporaryDir dir_;
};

}  // namespace

TEST(PowerStatus, NoSuppliesIsUnknownAndNeverBlocks) {
    FakeSysfs fs;
    const PowerStatus s = readPowerStatus(fs.root());
    EXPECT_EQ(s.battery_pct, -1);
    EXPECT_FALSE(lowBatteryBlocks(s));
    EXPECT_EQ(powerReadout(s), "AC");
}

TEST(PowerStatus, MissingRootNeverBlocks) {
    const PowerStatus s = readPowerStatus("/nonexistent/power_supply");
    EXPECT_EQ(s.battery_pct, -1);
    EXPECT_FALSE(lowBatteryBlocks(s));
}

TEST(PowerStatus, LowOnBatteryBlocks) {
    FakeSysfs fs;
    fs.mains("AC", false);
    fs.battery("BAT0", 15, "Discharging");
    const PowerStatus s = readPowerStatus(fs.root());
    EXPECT_FALSE(s.on_ac);
    EXPECT_EQ(s.battery_pct, 15);
    EXPECT_TRUE(lowBatteryBlocks(s));
}

// The field bug: battery dipped low, laptop plugged in, gate stayed shut.
TEST(PowerStatus, LowButPluggedInDoesNotBlock) {
    FakeSysfs fs;
    fs.mains("AC", true);
    fs.battery("BAT0", 15, "Charging");
    const PowerStatus s = readPowerStatus(fs.root());
    EXPECT_TRUE(s.on_ac);
    EXPECT_FALSE(lowBatteryBlocks(s));
    EXPECT_EQ(powerReadout(s), QString::fromUtf8("Charging · 15%"));
}

TEST(PowerStatus, ChargingBatteryCountsAsPluggedInWithoutMainsNode) {
    FakeSysfs fs;
    fs.battery("BAT0", 12, "Charging");
    EXPECT_FALSE(lowBatteryBlocks(readPowerStatus(fs.root())));
}

TEST(PowerStatus, NotChargingCountsAsPluggedIn) {
    FakeSysfs fs;
    fs.battery("BAT0", 18, "Not charging");
    EXPECT_TRUE(readPowerStatus(fs.root()).on_ac);
}

TEST(PowerStatus, UsbCChargerCountsAsPluggedIn) {
    FakeSysfs fs;
    fs.set("ucsi-source-psy-USBC000:001", "type", "USB");
    fs.set("ucsi-source-psy-USBC000:001", "online", "1");
    fs.battery("BAT0", 10, "Discharging");  // some firmwares lag the status
    EXPECT_FALSE(lowBatteryBlocks(readPowerStatus(fs.root())));
}

// Dual-battery laptops charge one pack at a time: BAT0 low, BAT1 full.
TEST(PowerStatus, DualBatteryUsesCombinedLevel) {
    FakeSysfs fs;
    fs.mains("AC", false);
    fs.set("BAT0", "type", "Battery");
    fs.set("BAT0", "status", "Discharging");
    fs.set("BAT0", "energy_now", "5000000");
    fs.set("BAT0", "energy_full", "50000000");
    fs.set("BAT1", "type", "Battery");
    fs.set("BAT1", "status", "Discharging");
    fs.set("BAT1", "energy_now", "40000000");
    fs.set("BAT1", "energy_full", "50000000");
    const PowerStatus s = readPowerStatus(fs.root());
    EXPECT_EQ(s.battery_pct, 45);
    EXPECT_FALSE(lowBatteryBlocks(s));
}

TEST(PowerStatus, ChargeCountersUsedWhenNoEnergyCounters) {
    FakeSysfs fs;
    fs.set("BAT0", "type", "Battery");
    fs.set("BAT0", "status", "Discharging");
    fs.set("BAT0", "charge_now", "1000");
    fs.set("BAT0", "charge_full", "4000");
    EXPECT_EQ(readPowerStatus(fs.root()).battery_pct, 25);
}

TEST(PowerStatus, CapacityOnlyBatteriesAreAveraged) {
    FakeSysfs fs;
    fs.battery("BAT0", 10, "Discharging");
    fs.battery("BAT1", 50, "Discharging");
    EXPECT_EQ(readPowerStatus(fs.root()).battery_pct, 30);
}

TEST(PowerStatus, PeripheralBatteryIgnored) {
    FakeSysfs fs;
    fs.battery("BAT0", 80, "Discharging");
    fs.battery("hidpp_battery_0", 5, "Discharging");
    fs.set("hidpp_battery_0", "scope", "Device");
    const PowerStatus s = readPowerStatus(fs.root());
    EXPECT_EQ(s.battery_pct, 80);
    EXPECT_FALSE(lowBatteryBlocks(s));
}

TEST(PowerStatus, TypeInferredFromNameWhenMissing) {
    FakeSysfs fs;
    fs.set("ADP1", "online", "1");
    fs.set("BAT0", "capacity", "8");
    const PowerStatus s = readPowerStatus(fs.root());
    EXPECT_TRUE(s.on_ac);
    EXPECT_EQ(s.battery_pct, 8);
}

TEST(PowerStatus, ThresholdBoundary) {
    PowerStatus s;
    s.battery_pct = 20;
    EXPECT_FALSE(lowBatteryBlocks(s));
    s.battery_pct = 19;
    EXPECT_TRUE(lowBatteryBlocks(s));
}
