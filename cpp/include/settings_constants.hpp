/**
 * @file settings_constants.hpp
 * @brief Shared QSettings organization and application name for BDR Coverage Planner.
 */

#pragma once

namespace f2c_cpp {

/** QSettings organization name (must match QApplication::organizationName). */
constexpr const char* kSettingsOrgName = "PilotControl";

/** QSettings application name for this app. */
constexpr const char* kSettingsAppName = "BDRCoveragePlanner";

}  // namespace f2c_cpp
