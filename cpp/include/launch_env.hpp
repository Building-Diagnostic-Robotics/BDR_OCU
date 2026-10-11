/**
 * @file launch_env.hpp
 * @brief The shell fragments every OCU-driven `ros2 launch` shares.
 *
 * Both the legacy Stage 4/5 path (`AppShellWindow`) and Stage 6
 * (`MissionController`) launch the same way: `bash -lc "<preamble>;
 * ros2 launch …"` locally, and `ssh -tt … bash -lc "<preamble>; ros2
 * launch …"` on the robot. Keeping the preamble and the laptop sweep here
 * means the two paths cannot drift apart.
 */

#pragma once

namespace f2c_cpp {

/**
 * Sources the ROS + workspace env and pins the DDS config. `set -e`, never
 * `set -u`: the ROS/colcon setup scripts read variables that are normally
 * unset. `bash -lc` is not interactive, so the stock `.bashrc` returns
 * before its exports: the DDS lines below are what keep the robot's nodes
 * and its zenohd on loopback CycloneDDS. Without them CycloneDDS binds to
 * whatever interface it picks (Wi-Fi included) and nodes fall back to
 * Fast DDS.
 */
inline constexpr const char* kLaunchEnvPreamble =
    "set -e; "
    "if [ -f \"$HOME/.bashrc\" ]; then source \"$HOME/.bashrc\"; fi; "
    "if [ -f /opt/ros/humble/setup.bash ]; then source /opt/ros/humble/setup.bash; fi; "
    "if [ -f \"$HOME/pilot_ws/install/setup.bash\" ]; then source \"$HOME/pilot_ws/install/setup.bash\"; fi; "
    "case \"${CYCLONEDDS_URI:-}\" in *rf_cyclonedds.xml*) unset CYCLONEDDS_URI ;; esac; "
    "if [ -z \"${CYCLONEDDS_URI:-}\" ] && [ -f \"$HOME/cyclone_loopback.xml\" ]; then "
    "export CYCLONEDDS_URI=\"file://$HOME/cyclone_loopback.xml\"; "
    "fi; "
    "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp; "
    "export ROS_DOMAIN_ID=0; ";

/**
 * Laptop-side sweep, run before every laptop launch and after every
 * teardown. `ros2 launch` reaps its children on SIGTERM, but a SIGKILLed
 * launch (or one that died on its own) leaves `zenohd` and `host_teleop`
 * running, and every orphaned zenohd is another bridge client routing the
 * same topics again. Exact process names only; `|| true` because nothing
 * to kill is the normal case.
 */
inline constexpr const char* kLaptopLaunchSweep =
    "pkill -f '[r]os2 launch pilot_control laptop_teleop.launch.py' >/dev/null 2>&1 || true; "
    "sleep 1; "
    "pkill -x zenohd >/dev/null 2>&1 || true; "
    "pkill -x host_teleop >/dev/null 2>&1 || true";

/**
 * Robot-side stop for a map-collection tree. Killing the local `ssh` does
 * not reach it: the collect script backgrounds `ros2 launch`, so the run
 * outlives the session, and a second Collect would put two stacks on the
 * same LiDAR, CAN bus and /cmd_vel. SIGINT first so map_collection_node
 * disarms and the ODrive nodes idle their axes, then kill by name the nodes
 * known to ignore shutdown -- but only when no other `ros2 launch` is up,
 * since a director or robot_complete tree runs the same node names.
 *
 * Patterns are bracketed so they cannot match the shell running them, and
 * any script that also starts the launch must keep that command line from
 * containing `ros2 launch pilot_control robot_map_collection` verbatim, or
 * the SIGINT below lands on the script itself.
 */
inline constexpr const char* kMapCollectionStop =
    "pkill -INT -f '[r]os2 launch pilot_control robot_map_collection' >/dev/null 2>&1 || true; "
    "for i in $(seq 1 100); do "
    "  pgrep -f '[r]os2 launch pilot_control robot_map_collection' >/dev/null 2>&1 || break; "
    "  sleep 0.1; "
    "done; "
    "if ! pgrep -af '[r]os2 launch' 2>/dev/null | grep -v robot_map_collection | grep -q .; then "
    "  pkill -f '[m]ap_collection_node' >/dev/null 2>&1 || true; "
    "  pkill -f '[f]astlio_mapping' >/dev/null 2>&1 || true; "
    "  pkill -f '[l]ivox_ros_driver2_node' >/dev/null 2>&1 || true; "
    "  pkill -f '[o]drive_can_node' >/dev/null 2>&1 || true; "
    "  pkill -f '[d]iff_drive_controller' >/dev/null 2>&1 || true; "
    "fi; "
    "pkill -9 -f '[r]os2 launch pilot_control robot_map_collection' >/dev/null 2>&1 || true; ";

}  // namespace f2c_cpp
