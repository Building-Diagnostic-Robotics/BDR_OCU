/**
 * @file robot_update_copy.hpp
 * @brief What the operator reads about a ROOFUS software update.
 *
 * RepoSyncManager speaks engineer (branches, SHAs, colcon, ssh) and keeps
 * doing so for update.log. Operators are roofing contractors: the banner
 * and RobotSyncDialog show only the plain words built here. The technical
 * text stays reachable behind "Details for BDR support" on a failure.
 *
 * Pure functions, Qt Core only; tested in robot_update_copy_tests.cpp.
 */
#pragma once

#include <QString>

#include "repo_sync_manager.hpp"

namespace f2c_cpp::robot_update_copy {

inline constexpr const char* kSupportPhone = "(510) 514-9518";

enum class Step { Laptop, Robot, Prepare };

/// Which of the two steps a RepoSyncManager stage label belongs to.
inline Step stepForStage(const QString& stage) {
    static const char* const kPrepare[] = {"Preparing robot", "Installing robot"};
    static const char* const kRobot[] = {"Verifying robot", "Checking robot",
                                         "Pushing to robot", "Switching robot",
                                         "Rebuilding robot"};
    for (const char* p : kPrepare) {
        if (stage.startsWith(QLatin1String(p))) return Step::Prepare;
    }
    for (const char* p : kRobot) {
        if (stage.startsWith(QLatin1String(p))) return Step::Robot;
    }
    return Step::Laptop;
}

inline QString stepText(Step step) {
    switch (step) {
        case Step::Laptop:  return QStringLiteral("Step 1 of 2: Updating the laptop");
        case Step::Robot:   return QStringLiteral("Step 2 of 2: Updating ROOFUS");
        case Step::Prepare: return QStringLiteral("Getting ROOFUS ready");
    }
    return QString();
}

/// Why Update Now is disabled, or empty.
inline QString blockerText(bool low_battery) {
    return low_battery
               ? QStringLiteral("Laptop battery is low. Plug in the charger to update.")
               : QString();
}

struct Text {
    QString subtitle;
    QString message;
};

/// Before the operator starts.
inline Text offerText(const RepoSyncSnapshot& snap) {
    if (!snap.robot_online) {
        return {QStringLiteral("New software for ROOFUS"),
                QStringLiteral("ROOFUS isn't connected. Turn ROOFUS on and wait "
                               "for it to connect, then update.")};
    }
    return {QStringLiteral("New software for ROOFUS"),
            QStringLiteral("This takes a few minutes. Keep ROOFUS turned on "
                           "and connected until it finishes.")};
}

inline Text busyText() {
    return {QStringLiteral("Updating…"),
            QStringLiteral("Don't turn off ROOFUS or close this app.")};
}

/// A background status check is running (not an update).
inline Text checkingText() {
    return {QStringLiteral("Checking…"), QStringLiteral("Checking ROOFUS for updates.")};
}

enum class Outcome { Done, RobotNotConnected, Failed };

/// After a run. `snap` is the snapshot the run left behind.
inline Outcome outcome(int level, const RepoSyncSnapshot& snap) {
    if (level == RepoSyncManager::LevelBad) return Outcome::Failed;
    if (snap.robot_pending || !snap.robot_online) return Outcome::RobotNotConnected;
    return Outcome::Done;
}

inline Text outcomeText(Outcome o) {
    switch (o) {
        case Outcome::Done:
            return {QStringLiteral("Up to date"), QStringLiteral("ROOFUS is up to date.")};
        case Outcome::RobotNotConnected:
            return {QStringLiteral("Not finished"),
                    QStringLiteral("The laptop is updated, but ROOFUS wasn't "
                                   "connected. Turn ROOFUS on and update again.")};
        case Outcome::Failed:
            return {QStringLiteral("Not finished"),
                    QStringLiteral("The update didn't finish. Try again. If it keeps "
                                   "happening, call BDR support at %1.")
                        .arg(QLatin1String(kSupportPhone))};
    }
    return {};
}

/// Dashboard banner.
inline QString bannerTitle() { return QStringLiteral("ROOFUS update available"); }
inline QString bannerSubtitle() {
    return QStringLiteral("Takes a few minutes. Keep ROOFUS on and connected.");
}

}  // namespace f2c_cpp::robot_update_copy
