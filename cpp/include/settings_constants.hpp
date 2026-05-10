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

/** QApplication dynamic property: qint64 epoch ms when the OCU process started (`main`). */
constexpr const char* kOcuStartEpochMsProperty = "bdr_ocu_start_epoch_ms";

}  // namespace f2c_cpp
