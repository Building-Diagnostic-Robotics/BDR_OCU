cmake_minimum_required(VERSION 3.16)
project(bdr_coverage_planner VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# --- OTA / version stamping ---
# Renders include/version_info.hpp.in -> ${CMAKE_BINARY_DIR}/generated/version_info.hpp.
# Carries semver + git short SHA + repo coordinates for the OTA UpdateChecker.
set(APP_SEMVER       "${PROJECT_VERSION}")
set(APP_REPO_OWNER   "Building-Diagnostic-Robotics")
set(APP_REPO_NAME    "BDR_OCU")

# First run at configure time so the first compile finds the header.
execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBIN_DIR=${CMAKE_BINARY_DIR}
        -DAPP_SEMVER=${APP_SEMVER}
        -DAPP_REPO_OWNER=${APP_REPO_OWNER}
        -DAPP_REPO_NAME=${APP_REPO_NAME}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenVersionInfo.cmake
)

# Re-run on every build so the SHA tracks HEAD without manual cmake re-config.
add_custom_target(version_info_stamp ALL
    COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBIN_DIR=${CMAKE_BINARY_DIR}
        -DAPP_SEMVER=${APP_SEMVER}
        -DAPP_REPO_OWNER=${APP_REPO_OWNER}
        -DAPP_REPO_NAME=${APP_REPO_NAME}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenVersionInfo.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/version_info.hpp
    COMMENT "Stamping version_info.hpp with current git SHA"
    VERBATIM
)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# BDR_REWIRE: Unified dev-mode gate bypass (see cpp/include/dev_flags.hpp and
# docs/DEV_BYPASSES.md). OFF by default. Compiled in ONLY for non-Release build
# types so a Release .deb can never carry the bypass. Enabling it on a Release
# build is a hard configure error.
option(BDR_DEV_MODE "Bypass operational gates for offline UI testing (non-Release only)" OFF)
if(BDR_DEV_MODE AND CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR
        "BDR_DEV_MODE=ON is forbidden with CMAKE_BUILD_TYPE=Release. "
        "Dev mode bypasses operational gates and must never ship. "
        "Use -DCMAKE_BUILD_TYPE=Debug (or RelWithDebInfo) to enable it.")
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Try Qt5 first (to match PCL's Qt version), fallback to Qt6
find_package(Qt5 QUIET COMPONENTS Core Widgets Concurrent Network Svg)
if(Qt5_FOUND)
    set(QT_VERSION 5)
    find_package(Qt5 REQUIRED COMPONENTS Core Widgets Concurrent Network Svg)
    set(QT_LIBS Qt5::Core Qt5::Widgets Qt5::Concurrent Qt5::Network Qt5::Svg)
    set(QT_CORE_LIBS Qt5::Core Qt5::Network)
    set(QT_GUI_LIBS Qt5::Core Qt5::Widgets Qt5::Network)
    message(STATUS "Using Qt5: ${Qt5Core_VERSION}")
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Concurrent Network Svg)
    set(QT_VERSION 6)
    set(QT_LIBS Qt6::Core Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::Svg)
    set(QT_CORE_LIBS Qt6::Core Qt6::Network)
    set(QT_GUI_LIBS Qt6::Core Qt6::Widgets Qt6::Network)
    message(STATUS "Using Qt6: ${Qt6Core_VERSION}")
endif()

# PCL 1.12 on Ubuntu 22.04 transitively pulls VTK 9.1, which declares
# MPI::MPI_C in its target link interface. Without an explicit
# find_package(MPI) here, CMake fails to register the imported MPI
# target and configure aborts with:
#   "VTK::mpi contains MPI::MPI_C but the target was not found"
# We don't actually use MPI in our code; this is purely to satisfy
# VTK's transitive dependency chain.
find_package(MPI QUIET)

# Find PCL (no visualization component to avoid Qt/VTK conflicts)
# 3D viewing is done via external viewers (pcl_viewer, Open3D, CloudCompare)
find_package(PCL 1.10 REQUIRED COMPONENTS common io filters)
find_package(Eigen3 REQUIRED)
find_package(Fields2Cover QUIET)

# CGAL for geometry operations (alphashape, etc.)
find_package(CGAL QUIET)

# GStreamer for video streaming
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-video-1.0)
include_directories(${GST_INCLUDE_DIRS})

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/components
    ${CMAKE_BINARY_DIR}/generated
    ${PCL_INCLUDE_DIRS}
    ${EIGEN3_INCLUDE_DIR}
)

# Add PCL definitions
add_definitions(${PCL_DEFINITIONS})

# QCustomPlot (embedded - download if not present)
set(QCUSTOMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/qcustomplot)
if(NOT EXISTS ${QCUSTOMPLOT_DIR}/qcustomplot.cpp)
    message(STATUS "QCustomPlot not found, will use simple Qt plotting")
    set(USE_QCUSTOMPLOT OFF)
else()
    set(USE_QCUSTOMPLOT ON)
    set(QCUSTOMPLOT_SOURCES ${QCUSTOMPLOT_DIR}/qcustomplot.cpp)
    include_directories(${QCUSTOMPLOT_DIR})
endif()

# Header files (for MOC processing)
set(HEADERS
    include/coverage_pipeline.hpp
    include/coverage_geometry.hpp
    include/coverage_stats.hpp
    include/setup_screen.hpp
    include/app_shell.hpp
    include/startup_screen.hpp
    include/dashboard_screen.hpp
    include/exploration_screen.hpp
    include/planner_screen.hpp
    include/plot_widget.hpp
    include/obstacle_detector.hpp
    include/transfer_manager.hpp
    include/preset_manager.hpp
    include/ui_theme_constants.hpp
    include/cloud_upload_manager.hpp
    include/robot_registry.hpp
    include/upload_runner.hpp
    include/upload_dialog.hpp
    include/components/bdr_message_box.hpp
    include/components/bdr_input_dialog.hpp
    include/components/bdr_progress_dialog.hpp
    include/components/banter_loader_widget.hpp
    include/components/tilt_calibration_dialog.hpp
    include/components/pre_scan_checklist_dialog.hpp
    include/components/mission_metadata_dialog.hpp
    include/components/offline_finalize_dialog.hpp
    include/components/fpv_camera_view.hpp
    include/components/auto_hide_scroll_bar.hpp
    include/components/svg_icon_button.hpp
    include/components/camera_switch_pill.hpp
    include/components/rollback_banner.hpp
    include/components/update_banner.hpp
    include/components/update_modal.hpp
    include/units_system.hpp
    include/link_health_monitor.hpp
    include/robot_reachability_probe.hpp
    include/update/update_settings.hpp
    include/update/update_checker.hpp
    include/video_stream_widget.hpp
)

# ---------------------------------------------------------------------------
# bdr_update_core — static library shared by the OCU and bdr-update-runner.
# Contains everything needed to download a release and append to the OTA
# log, but NOT the GUI pieces (UpdateModal, UpdateBanner) or the polling
# UpdateChecker (OCU only). Phase 7 locked Q2=B.
# ---------------------------------------------------------------------------
add_library(bdr_update_core STATIC
    include/update/update_types.hpp
    include/update/update_log.hpp
    include/update/update_lockfile.hpp
    include/update/update_state.hpp
    include/update/update_downloader.hpp
    src/update/update_log.cpp
    src/update/update_lockfile.cpp
    src/update/update_state.cpp
    src/update/update_downloader.cpp
)
target_include_directories(bdr_update_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(bdr_update_core PUBLIC
    ${QT_CORE_LIBS}
)
# fPIC required because the OCU exe is built with PIE on most distros.
set_target_properties(bdr_update_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Source files
set(PIPELINE_SOURCES
    src/coverage_pipeline.cpp
)

# Legacy-only sources (recordings / cloud tab UX, teleop dock, session JSON)
# kept on disk for Dashboard rewiring — add back to GUI_SOURCES when wired:
#   src/data_transfer_dialog.cpp  src/cloud_upload_dialog.cpp
#   src/scan_session_tracker.cpp  src/teleop_widget.cpp
#   src/network_monitor.cpp
# and matching HEADERS for Qt AUTOMOC:
#   include/data_transfer_dialog.hpp  include/cloud_upload_dialog.hpp
#   include/scan_session_tracker.hpp  include/teleop_widget.hpp
#   include/network_monitor.hpp

set(GUI_SOURCES
    src/plot_widget.cpp
    src/setup_screen.cpp
    src/app_shell.cpp
    src/startup_screen.cpp
    src/dashboard_screen.cpp
    src/exploration_screen.cpp
    src/planner_screen.cpp
    src/obstacle_detector.cpp
    src/transfer_manager.cpp
    src/preset_manager.cpp
    src/cloud_upload_manager.cpp
    src/upload_runner.cpp
    src/upload_dialog.cpp
    src/video_stream_widget.cpp
    src/robot_registry.cpp
    src/robot_login.cpp
    src/components/bdr_message_box.cpp
    src/components/bdr_input_dialog.cpp
    src/components/bdr_progress_dialog.cpp
    src/components/banter_loader_widget.cpp
    src/components/tilt_calibration_dialog.cpp
    src/components/pre_scan_checklist_dialog.cpp
    src/components/mission_metadata_dialog.cpp
    src/components/offline_finalize_dialog.cpp
    src/components/fpv_camera_view.cpp
    src/components/auto_hide_scroll_bar.cpp
    src/components/svg_icon_button.cpp
    src/components/camera_switch_pill.cpp
    src/components/rollback_banner.cpp
    src/components/update_banner.cpp
    src/components/update_modal.cpp
    src/update/update_checker.cpp
    src/units_system.cpp
    src/link_health_monitor.cpp
    src/robot_reachability_probe.cpp
    src/main.cpp
    resources.qrc
)

# If QCustomPlot is available, add it
if(USE_QCUSTOMPLOT)
    list(APPEND GUI_SOURCES ${QCUSTOMPLOT_SOURCES})
    add_definitions(-DUSE_QCUSTOMPLOT)
endif()

# Create executable
add_executable(bdr_coverage_planner
    ${HEADERS}
    ${PIPELINE_SOURCES}
    ${GUI_SOURCES}
)

# Ensure version_info.hpp is regenerated before compiling anything that includes it.
add_dependencies(bdr_coverage_planner version_info_stamp)

# Link libraries - PCL libs (no visualization to avoid VTK conflicts)
set(PCL_FILTERED_LIBS
    ${PCL_COMMON_LIBRARIES}
    ${PCL_IO_LIBRARIES}
    ${PCL_FILTERS_LIBRARIES}
)

target_link_libraries(bdr_coverage_planner
    bdr_update_core
    ${QT_LIBS}
    ${PCL_FILTERED_LIBS}
    Eigen3::Eigen
    ${GST_LIBRARIES}
)

# BDR_REWIRE: define BDR_DEV_MODE so dev_flags.hpp's kDevMode is true. Only ever
# reached for non-Release builds (guarded above).
if(BDR_DEV_MODE)
    target_compile_definitions(bdr_coverage_planner PRIVATE BDR_DEV_MODE)
    message(STATUS "BDR_DEV_MODE=ON — operational gates bypassed (DEV BUILD, do not ship)")
endif()

# Link Fields2Cover if found
if(Fields2Cover_FOUND)
    target_link_libraries(bdr_coverage_planner Fields2Cover::Fields2Cover)
    target_compile_definitions(bdr_coverage_planner PRIVATE HAVE_FIELDS2COVER)
    message(STATUS "Fields2Cover found and linked")
else()
    # Try to find it manually
    find_library(FIELDS2COVER_LIB fields2cover
        PATHS /usr/local/lib /usr/lib /opt/fields2cover/lib
    )
    find_path(FIELDS2COVER_INCLUDE fields2cover.h
        PATHS /usr/local/include /usr/include /opt/fields2cover/include
    )
    if(FIELDS2COVER_LIB AND FIELDS2COVER_INCLUDE)
        target_include_directories(bdr_coverage_planner PRIVATE ${FIELDS2COVER_INCLUDE})
        target_link_libraries(bdr_coverage_planner ${FIELDS2COVER_LIB})
        target_compile_definitions(bdr_coverage_planner PRIVATE HAVE_FIELDS2COVER)
        message(STATUS "Fields2Cover found manually: ${FIELDS2COVER_LIB}")
    else()
        message(WARNING "Fields2Cover not found - coverage features will be disabled")
    endif()
endif()

# Link CGAL if found
if(CGAL_FOUND)
    target_link_libraries(bdr_coverage_planner CGAL::CGAL)
    target_compile_definitions(bdr_coverage_planner PRIVATE HAVE_CGAL)
    message(STATUS "CGAL found and linked")
else()
    message(WARNING "CGAL not found - alphashape will use fallback method")
endif()

# ROS2 dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(std_srvs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(odrive_can REQUIRED)
find_package(tf2 REQUIRED)
find_package(tf2_geometry_msgs REQUIRED)

# Add ROS2 includes and libraries
include_directories(${rclcpp_INCLUDE_DIRS})
target_link_libraries(bdr_coverage_planner ${rclcpp_LIBRARIES})
ament_target_dependencies(bdr_coverage_planner rclcpp std_msgs std_srvs geometry_msgs sensor_msgs nav_msgs odrive_can tf2 tf2_geometry_msgs)

# ---------------------------------------------------------------------------
# bdr-update-runner — external Qt binary that owns the OTA download +
# install lifecycle. Lean Qt5 Core/Widgets/Network only; no ROS, no PCL,
# no Fields2Cover. Phase 7 locked Q1=A (CLI args), Q3=B (lockfile-gated
# handoff). Spawned by the OCU and replaces it via execv on completion.
# ---------------------------------------------------------------------------
add_executable(bdr-update-runner
    include/runner/update_runner_window.hpp
    src/runner/update_runner_window.cpp
    src/runner/main.cpp
)
add_dependencies(bdr-update-runner version_info_stamp)
target_include_directories(bdr-update-runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/runner
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(bdr-update-runner PRIVATE
    bdr_update_core
    ${QT_GUI_LIBS}
)

# ---------------------------------------------------------------------------
# Unit tests (GoogleTest). Opt-in via -DBDR_BUILD_TESTS=ON (default ON when
# GTest is available). Tests compile UpdateChecker's pure-function surface in
# isolation against Qt Core/Network + bdr_update_core — no GUI, ROS, or PCL.
# Never installed; create_deb only packages the two RUNTIME targets below.
# ---------------------------------------------------------------------------
option(BDR_BUILD_TESTS "Build unit tests (requires GoogleTest)" ON)
if(BDR_BUILD_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        enable_testing()
        add_executable(update_checker_tests
            tests/update_checker_tests.cpp
            src/update/update_checker.cpp
            include/update/update_checker.hpp
        )
        add_dependencies(update_checker_tests version_info_stamp)
        target_include_directories(update_checker_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_BINARY_DIR}/generated
        )
        target_link_libraries(update_checker_tests PRIVATE
            bdr_update_core
            ${QT_CORE_LIBS}
            GTest::gtest
            GTest::gtest_main
        )
        add_test(NAME update_checker_tests COMMAND update_checker_tests)
        message(STATUS "Unit tests: enabled (update_checker_tests)")
    else()
        message(STATUS "Unit tests: GTest not found, skipping")
    endif()
endif()

# Install
install(TARGETS bdr_coverage_planner bdr-update-runner
    RUNTIME DESTINATION bin
)

# Print configuration summary
message(STATUS "=== BDR Coverage Planner Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Qt version: ${QT_VERSION}")
message(STATUS "PCL: ${PCL_VERSION}")
message(STATUS "Eigen3: Found")
message(STATUS "Fields2Cover: ${Fields2Cover_FOUND}")
message(STATUS "CGAL: ${CGAL_FOUND}")
message(STATUS "GStreamer: ${GST_VERSION}")
message(STATUS "QCustomPlot: ${USE_QCUSTOMPLOT}")
message(STATUS "==========================================")
