/**
 * @file robot_login.hpp
 * @brief Login to robot via SSH + pilot_control_auth (legacy BDR coverage planner flow).
 */

#pragma once

#include <QString>

namespace f2c_cpp {

class RobotRegistry;

/**
 * Performs login to the robot: resolves robot_id via registry, SSHs to the robot,
 * runs pilot_control_auth login --robot-id X --pin-stdin --json, sends PIN on stdin.
 * On success, optional profileOut can be used to persist robot host (e.g. for diagnostics).
 * @return true if login succeeded (token received), false otherwise with errorOut set.
 *
 * TODO(BDR_REWIRE): expose the parsed bearer token via a tokenOut parameter
 * so callers can plumb it into the planner mission-upload path. Currently
 * the token is verified and discarded; downstream authorized-command flows
 * (waypoint upload, mission start) still rely on plain SSH/rsync. Once a
 * consumer exists, add `QString* tokenOut = nullptr` here and have
 * AppShellWindow stash it as session state.
 */
bool loginToRobot(const QString& robotId,
                  const QString& pin,
                  const RobotRegistry& registry,
                  QString* errorOut,
                  QString* hostOut = nullptr);

}  // namespace f2c_cpp
