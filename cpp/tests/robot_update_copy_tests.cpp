// ROOFUS update dialog: operator wording, and the flows behind its buttons.
#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>

#include "components/robot_sync_dialog.hpp"
#include "components/robot_update_copy.hpp"

using namespace f2c_cpp;
namespace copy = f2c_cpp::robot_update_copy;

namespace {

RepoSyncSnapshot offer() {
    RepoSyncSnapshot s;
    s.deploy_branch = "main";
    s.laptop_on_deploy = true;
    s.robot_online = true;
    s.offer_update = true;
    return s;
}

QString text(const RobotSyncDialog& d, const char* name) {
    auto* l = d.findChild<QLabel*>(name);
    return l ? l->text() : QString();
}

QPushButton* button(const RobotSyncDialog& d, const char* name) {
    return d.findChild<QPushButton*>(name);
}

/// Nothing an operator reads may carry engineer words.
void expectPlain(const QString& s) {
    for (const char* bad : {"branch", "git", "SHA", "colcon", "ssh", "origin",
                            "workspace", "sync", "pilot_ws", "@"}) {
        EXPECT_FALSE(s.contains(QLatin1String(bad), Qt::CaseInsensitive))
            << s.toStdString() << " contains " << bad;
    }
}

}  // namespace

TEST(RobotUpdateCopy, StagesMapToTwoSteps) {
    EXPECT_EQ(copy::stepForStage("Checking for updates…"), copy::Step::Laptop);
    EXPECT_EQ(copy::stepForStage("Fetching from origin…"), copy::Step::Laptop);
    EXPECT_EQ(copy::stepForStage("Checking out robot-fix…"), copy::Step::Laptop);
    EXPECT_EQ(copy::stepForStage("Rebuilding affected packages…"), copy::Step::Laptop);
    EXPECT_EQ(copy::stepForStage("Checking robot…"), copy::Step::Robot);
    EXPECT_EQ(copy::stepForStage("Pushing to robot…"), copy::Step::Robot);
    EXPECT_EQ(copy::stepForStage("Switching robot to main…"), copy::Step::Robot);
    EXPECT_EQ(copy::stepForStage("Rebuilding robot…"), copy::Step::Robot);
    EXPECT_EQ(copy::stepForStage("Verifying robot…"), copy::Step::Robot);
    EXPECT_EQ(copy::stepForStage("Preparing robot…"), copy::Step::Prepare);
    EXPECT_EQ(copy::stepForStage("Installing robot deploy helpers…"), copy::Step::Prepare);
}

TEST(RobotUpdateCopy, OutcomeFromLevelAndRobot) {
    RepoSyncSnapshot s = offer();
    EXPECT_EQ(copy::outcome(RepoSyncManager::LevelOk, s), copy::Outcome::Done);
    EXPECT_EQ(copy::outcome(RepoSyncManager::LevelWarn, s), copy::Outcome::Done)
        << "dev-laptop warnings (unpushed commits) are still done for the operator";
    EXPECT_EQ(copy::outcome(RepoSyncManager::LevelBad, s), copy::Outcome::Failed);
    s.robot_online = false;
    s.robot_pending = true;
    EXPECT_EQ(copy::outcome(RepoSyncManager::LevelWarn, s), copy::Outcome::RobotNotConnected);
}

TEST(RobotUpdateCopy, EveryOperatorStringIsPlain) {
    RepoSyncSnapshot s = offer();
    for (const copy::Text& t :
         {copy::offerText(s), copy::busyText(), copy::checkingText(),
          copy::outcomeText(copy::Outcome::Done),
          copy::outcomeText(copy::Outcome::RobotNotConnected),
          copy::outcomeText(copy::Outcome::Failed)}) {
        expectPlain(t.subtitle);
        expectPlain(t.message);
    }
    s.robot_online = false;
    expectPlain(copy::offerText(s).message);
    expectPlain(copy::blockerText(true));
    EXPECT_TRUE(copy::blockerText(false).isEmpty());
    expectPlain(copy::bannerTitle());
    expectPlain(copy::bannerSubtitle());
    for (copy::Step st : {copy::Step::Laptop, copy::Step::Robot, copy::Step::Prepare}) {
        expectPlain(copy::stepText(st));
    }
    EXPECT_TRUE(copy::outcomeText(copy::Outcome::Failed).message.contains("(510) 514-9518"));
}

// ------------------------------------------------------------ dialog flows

TEST(RobotSyncDialogFlow, UpdateNowThenSuccess) {
    RobotSyncDialog d;
    d.setBusy(false);
    d.setSnapshot(offer());
    int sync = 0;
    QObject::connect(&d, &RobotSyncDialog::syncRequested, [&]() { ++sync; });
    EXPECT_EQ(button(d, "RobotSyncDialog_Primary")->text(), "Update Now");
    button(d, "RobotSyncDialog_Primary")->click();
    EXPECT_EQ(sync, 1);
    d.setBusy(true, "Pushing to robot…");
    EXPECT_EQ(text(d, "RobotSyncDialog_Step"), "Step 2 of 2: Updating ROOFUS");
    d.setFinished(RepoSyncManager::LevelOk, "In sync · main", "Laptop and robot are both on main @ abc.");
    RepoSyncSnapshot after = offer();
    after.offer_update = false;
    d.setSnapshot(after);
    EXPECT_EQ(text(d, "RobotSyncDialog_Message"), "ROOFUS is up to date.");
    EXPECT_FALSE(button(d, "RobotSyncDialog_DetailsToggle")->isVisibleTo(&d));
}

TEST(RobotSyncDialogFlow, FailureShowsSupportDetailsOnRequest) {
    RobotSyncDialog d;
    d.setBusy(false);
    d.setSnapshot(offer());
    button(d, "RobotSyncDialog_Primary")->click();
    d.setFinished(RepoSyncManager::LevelBad, "Robot build failed", "CMake Error");
    EXPECT_TRUE(text(d, "RobotSyncDialog_Message").startsWith("The update didn't finish."));
    EXPECT_EQ(button(d, "RobotSyncDialog_Primary")->text(), "Try Again");
    EXPECT_TRUE(button(d, "RobotSyncDialog_DetailsToggle")->isVisibleTo(&d));
    auto* details = d.findChild<QLabel*>("RobotSyncDialog_Details");
    EXPECT_FALSE(details->isVisibleTo(&d)) << "engineer text hidden until asked for";
    button(d, "RobotSyncDialog_DetailsToggle")->click();
    EXPECT_TRUE(details->isVisibleTo(&d));
    EXPECT_EQ(details->text(), "Robot build failed\nCMake Error");
}

TEST(RobotSyncDialogFlow, RobotNeedingSetupIsPreparedThenUpdatedFromOnePress) {
    RobotSyncDialog d;
    d.setBusy(false);
    RepoSyncSnapshot s = offer();
    s.needs_prepare = true;
    d.setSnapshot(s);
    int prepare = 0;
    QObject::connect(&d, &RobotSyncDialog::prepareRequested, [&]() { ++prepare; });
    int sync = 0;
    QObject::connect(&d, &RobotSyncDialog::syncRequested, [&]() { ++sync; });
    button(d, "RobotSyncDialog_Primary")->click();
    EXPECT_EQ(prepare, 1);
    EXPECT_EQ(sync, 0);
    d.setBusy(true, "Installing robot deploy helpers…");
    EXPECT_EQ(text(d, "RobotSyncDialog_Step"), "Getting ROOFUS ready");
    d.setFinished(RepoSyncManager::LevelOk, "Robot ready for sync", "Helpers installed");
    EXPECT_FALSE(button(d, "RobotSyncDialog_Primary")->isVisibleTo(&d)) << "still busy";
    QCoreApplication::processEvents();
    EXPECT_EQ(sync, 1);
}

TEST(RobotSyncDialogFlow, FailedSetupDoesNotStartTheUpdate) {
    RobotSyncDialog d;
    d.setBusy(false);
    RepoSyncSnapshot s = offer();
    s.needs_prepare = true;
    d.setSnapshot(s);
    int sync = 0;
    QObject::connect(&d, &RobotSyncDialog::syncRequested, [&]() { ++sync; });
    button(d, "RobotSyncDialog_Primary")->click();
    d.setFinished(RepoSyncManager::LevelBad, "Robot prepare failed", "x");
    QCoreApplication::processEvents();
    EXPECT_EQ(sync, 0);
    EXPECT_EQ(button(d, "RobotSyncDialog_Primary")->text(), "Try Again");
}

TEST(RobotSyncDialogFlow, BackgroundCheckDoesNotLookLikeAnUpdateResult) {
    RobotSyncDialog d;
    d.setBusy(false);
    d.setSnapshot(offer());
    d.setBusy(true, "Checking status…");
    EXPECT_EQ(text(d, "RobotSyncDialog_Message"), "Checking ROOFUS for updates.");
    EXPECT_FALSE(d.findChild<QLabel*>("RobotSyncDialog_Step")->isVisibleTo(&d));
    d.setFinished(RepoSyncManager::LevelWarn, "Robot software out of date", "Laptop: main @ abc");
    d.setSnapshot(offer());
    EXPECT_EQ(button(d, "RobotSyncDialog_Primary")->text(), "Update Now");
    EXPECT_FALSE(button(d, "RobotSyncDialog_DetailsToggle")->isVisibleTo(&d));
}

TEST(RobotSyncDialogFlow, LowLaptopBatteryBlocksWithAReason) {
    RobotSyncDialog d;
    d.setBusy(false);
    d.setSnapshot(offer());
    RobotSyncDialog::GateState g;
    g.power.on_ac = false;
    g.power.battery_pct = 5;
    d.setGateState(g);
    EXPECT_FALSE(button(d, "RobotSyncDialog_Primary")->isEnabled());
    EXPECT_EQ(text(d, "RobotSyncDialog_Notice"),
              "Laptop battery is low. Plug in the charger to update.");
}

TEST(RobotSyncDialogFlow, LaterSnoozesOnlyFromTheOffer) {
    RobotSyncDialog d;
    d.setBusy(false);
    d.setSnapshot(offer());
    int later = 0;
    QObject::connect(&d, &RobotSyncDialog::laterRequested, [&]() { ++later; });
    button(d, "RobotSyncDialog_Secondary")->click();
    EXPECT_EQ(later, 1);
    button(d, "RobotSyncDialog_Primary")->click();
    d.setFinished(RepoSyncManager::LevelBad, "x", "y");
    button(d, "RobotSyncDialog_Secondary")->click();  // "Close" after a failure
    EXPECT_EQ(later, 1);
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
