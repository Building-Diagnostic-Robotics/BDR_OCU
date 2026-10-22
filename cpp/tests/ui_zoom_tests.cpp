// Unit tests for the app-wide zoom (A- / A+ on Get Started, ui_zoom.hpp):
//   - snapUiZoom()           : only the allowed levels ever reach Qt
//   - resolveStartupUiZoom() : dev override vs. saved choice vs. inherited env
//   - applyUiZoomBeforeQApplication(): env handed to a restarted copy

#include <gtest/gtest.h>

#include <QByteArray>
#include <QSettings>
#include <QTemporaryDir>
#include <QVariant>

#include <cmath>

#include "ui_zoom.hpp"

using namespace f2c_cpp;

TEST(UiZoom, SnapsToAllowedLevels) {
    EXPECT_DOUBLE_EQ(snapUiZoom(1.0), 1.0);
    EXPECT_DOUBLE_EQ(snapUiZoom(1.1), 1.1);
    EXPECT_DOUBLE_EQ(snapUiZoom(1.15), 1.15);
    // Out of range or junk never exceeds the field-laptop ceiling.
    EXPECT_DOUBLE_EQ(snapUiZoom(2.0), 1.15);
    EXPECT_DOUBLE_EQ(snapUiZoom(0.5), 1.0);
    EXPECT_DOUBLE_EQ(snapUiZoom(std::nan("")), kUiZoomDefault);
    EXPECT_DOUBLE_EQ(snapUiZoom(INFINITY), kUiZoomDefault);
}

TEST(UiZoom, LevelsStayWithinTheFieldLaptopCeiling) {
    // The scan page needs a ~905 px tall window; the field laptop's work
    // area is ~1046 px. See ui_zoom.hpp before raising this.
    for (const UiZoomLevel& level : kUiZoomLevels) {
        EXPECT_GE(1046.0 / level.factor, 905.0) << level.label;
    }
    EXPECT_DOUBLE_EQ(kUiZoomLevels.front().factor, kUiZoomDefault);
}

TEST(UiZoom, SavedChoiceWhenNoEnv) {
    const UiZoomStartup z = resolveStartupUiZoom(QByteArray(), false, QVariant(1.1));
    EXPECT_DOUBLE_EQ(z.factor, 1.1);
    EXPECT_FALSE(z.external_override);
}

TEST(UiZoom, DefaultWhenNothingSaved) {
    const UiZoomStartup z = resolveStartupUiZoom(QByteArray(), false, QVariant());
    EXPECT_DOUBLE_EQ(z.factor, kUiZoomDefault);
    EXPECT_FALSE(z.external_override);
}

TEST(UiZoom, DevShellOverrideWins) {
    const UiZoomStartup z = resolveStartupUiZoom("1.5", false, QVariant(1.1));
    EXPECT_DOUBLE_EQ(z.factor, 1.5);
    EXPECT_TRUE(z.external_override);
}

TEST(UiZoom, InheritedFromThisAppIsNotAnOverride) {
    // Restart to apply / the update runner relaunch inherit our own env: the
    // saved choice must still decide, or a change would never apply.
    const UiZoomStartup z = resolveStartupUiZoom("1.10", true, QVariant(1.15));
    EXPECT_DOUBLE_EQ(z.factor, 1.15);
    EXPECT_FALSE(z.external_override);
}

class UiZoomApply : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(dir_.isValid());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir_.path());
        qunsetenv("QT_SCALE_FACTOR");
        qunsetenv(kUiZoomAppliedEnv);
    }
    void TearDown() override {
        qunsetenv("QT_SCALE_FACTOR");
        qunsetenv(kUiZoomAppliedEnv);
    }
    QTemporaryDir dir_;
};

TEST_F(UiZoomApply, SetsEnvFromSavedChoice) {
    saveUiZoom(1.15);
    EXPECT_DOUBLE_EQ(applyUiZoomBeforeQApplication(), 1.15);
    EXPECT_EQ(qgetenv("QT_SCALE_FACTOR"), QByteArray("1.15"));
    EXPECT_TRUE(qEnvironmentVariableIsSet(kUiZoomAppliedEnv));
}

TEST_F(UiZoomApply, NormalClearsInheritedFactor) {
    // Old copy ran at 1.15, operator pressed A- back to 100%, then Restart to apply.
    qputenv("QT_SCALE_FACTOR", "1.15");
    qputenv(kUiZoomAppliedEnv, "1");
    saveUiZoom(1.0);
    EXPECT_DOUBLE_EQ(applyUiZoomBeforeQApplication(), 1.0);
    EXPECT_FALSE(qEnvironmentVariableIsSet("QT_SCALE_FACTOR"));
}

TEST_F(UiZoomApply, LeavesDevOverrideAlone) {
    qputenv("QT_SCALE_FACTOR", "1.5");
    saveUiZoom(1.1);
    EXPECT_DOUBLE_EQ(applyUiZoomBeforeQApplication(), 1.5);
    EXPECT_EQ(qgetenv("QT_SCALE_FACTOR"), QByteArray("1.5"));
    EXPECT_FALSE(qEnvironmentVariableIsSet(kUiZoomAppliedEnv));
}

TEST_F(UiZoomApply, SavedValueIsSnapped) {
    QSettings(QString::fromLatin1(kSettingsOrgName), QString::fromLatin1(kSettingsAppName))
        .setValue(QString::fromLatin1(kSettingsUiZoomKey), 3.0);
    EXPECT_DOUBLE_EQ(savedUiZoom(), 1.15);
}
