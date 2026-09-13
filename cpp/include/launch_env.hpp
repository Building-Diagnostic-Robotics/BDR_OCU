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
 * unset. `.bashrc` first so the robot's own env (RMW, Zenoh config)
 * matches what an interactive launch would get.
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

}  // namespace f2c_cpp
