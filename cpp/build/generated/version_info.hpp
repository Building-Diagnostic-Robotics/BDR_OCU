/**
 * @file version_info.hpp
 * @brief Build-stamped version constants for the BDR Coverage Planner.
 *
 * GENERATED FILE — do not edit. The template lives at
 * cpp/include/version_info.hpp.in and is rendered into the build dir by
 * cpp/cmake/GenVersionInfo.cmake on every build.
 *
 * Used by the OTA update flow to compare the embedded commit SHA against the
 * latest GitHub release.
 */

#pragma once

#include <QString>
#include <QStringLiteral>

namespace f2c_cpp::version {

inline constexpr const char* kAppSemver = "1.0.0";
inline constexpr const char* kAppCommitSha = "fb662d4-dirty";
inline constexpr const char* kAppBuildDate = "2026-05-09";

inline constexpr const char* kRepoOwner = "Building-Diagnostic-Robotics";
inline constexpr const char* kRepoName = "BDR_OCU";

inline QString displayString() {
    return QStringLiteral("BDR Coverage Planner v%1 (%2)")
        .arg(QString::fromLatin1(kAppSemver))
        .arg(QString::fromLatin1(kAppCommitSha));
}

inline QString releasesApiUrl() {
    return QStringLiteral("https://api.github.com/repos/%1/%2/releases/latest")
        .arg(QString::fromLatin1(kRepoOwner))
        .arg(QString::fromLatin1(kRepoName));
}

}  // namespace f2c_cpp::version
