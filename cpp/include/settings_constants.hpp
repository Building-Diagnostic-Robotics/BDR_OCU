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

// ---------------------------------------------------------------------------
// Per-mission session metadata (collected via MissionMetadataDialog before
// every Stage 3 → Stage 4 transition). Values persist across runs so a
// returning operator only has to confirm Proceed.
//
// Building / operator are pushed to the robot's data_collection_coordinator
// as ROS parameters at Stage 5 Planner Start Scan, BEFORE the autonomous
// controller fires its /dc/start (see
// AppShellWindow::sendDataCollectorSessionMetadata), so the per-section
// folder layout becomes /R_DATA/<day>/<building_slug>/Section_<n>_<HHMMSS>/.
//
// Units are display-only. EVERYTHING on the wire (ROS messages, service
// requests, persisted CoverageConfig presets) stays in SI; only labels and
// operator-edited spinbox suffixes flip when this changes. See
// `units_system.hpp` for the formatter helpers.
// ---------------------------------------------------------------------------

/** QSettings key: last building name typed into the New Scan modal (raw text). */
constexpr const char* kSettingsBuildingNameKey = "session/building_name";

/** QSettings key: last operator name typed into the New Scan modal (raw text). */
constexpr const char* kSettingsOperatorNameKey = "session/operator_name";

/**
 * QSettings key: preferred display unit system. Stored as the lowercase
 * string `"metric"` or `"ansi"` — see `units_system.hpp` for the parser.
 * Default on a fresh QSettings is `"metric"`.
 */
constexpr const char* kSettingsUnitsKey = "session/units";

}  // namespace f2c_cpp
