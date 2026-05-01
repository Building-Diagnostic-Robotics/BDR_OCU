cmake_minimum_required(VERSION 3.16)
project(bdr_coverage_planner VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Try Qt5 first (to match PCL's Qt version), fallback to Qt6
find_package(Qt5 QUIET COMPONENTS Core Widgets Concurrent Network)
if(Qt5_FOUND)
    set(QT_VERSION 5)
    find_package(Qt5 REQUIRED COMPONENTS Core Widgets Concurrent Network)
    set(QT_LIBS Qt5::Core Qt5::Widgets Qt5::Concurrent Qt5::Network)
    message(STATUS "Using Qt5: ${Qt5Core_VERSION}")
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Concurrent Network)
    set(QT_VERSION 6)
    set(QT_LIBS Qt6::Core Qt6::Widgets Qt6::Concurrent Qt6::Network)
    message(STATUS "Using Qt6: ${Qt6Core_VERSION}")
endif()

# Find PCL (no visualization component to avoid Qt/VTK conflicts)
# 3D viewing is done via external viewers (pcl_viewer, Open3D, CloudCompare)
find_package(PCL 1.10 REQUIRED COMPONENTS common io filters registration)
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
    include/coverage_gui.hpp
    include/setup_screen.hpp
    include/app_shell.hpp
    include/startup_screen.hpp
    include/obstacle_detector.hpp
    include/robot_registry.hpp
    include/transfer_manager.hpp
    include/data_transfer_dialog.hpp
    include/preset_manager.hpp
    include/preset_dialog.hpp
    include/teleop_widget.hpp
    include/network_monitor.hpp
    include/cloud_upload_manager.hpp
    include/scan_session_tracker.hpp
    include/cloud_upload_dialog.hpp
)

# Source files
set(PIPELINE_SOURCES
    src/coverage_pipeline.cpp
)

set(GUI_SOURCES
    src/coverage_gui.cpp
    src/setup_screen.cpp
    src/app_shell.cpp
    src/startup_screen.cpp
    src/obstacle_detector.cpp
    src/robot_registry.cpp
    src/transfer_manager.cpp
    src/data_transfer_dialog.cpp
    src/preset_manager.cpp
    src/preset_dialog.cpp
    src/teleop_widget.cpp
    src/network_monitor.cpp
    src/cloud_upload_manager.cpp
    src/scan_session_tracker.cpp
    src/cloud_upload_dialog.cpp
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

# Link libraries - PCL libs (no visualization to avoid VTK conflicts)
set(PCL_FILTERED_LIBS
    ${PCL_COMMON_LIBRARIES}
    ${PCL_IO_LIBRARIES}
    ${PCL_FILTERS_LIBRARIES}
    ${PCL_REGISTRATION_LIBRARIES}
)

target_link_libraries(bdr_coverage_planner
    ${QT_LIBS}
    ${PCL_FILTERED_LIBS}
    Eigen3::Eigen
    ${GST_LIBRARIES}
)

# -----------------------------------------------------------------------------
# Runtime linking robustness (Fields2Cover transitive deps)
#
# Fields2Cover (especially when installed under /usr/local) may depend on
# additional shared libraries (e.g., OR-Tools, matplot, steering_functions).
# Those are transitive dependencies of libFields2Cover.so.
#
# With RUNPATH (new dtags), the executable's runpath is NOT used to resolve
# transitive dependencies. Using an old-style RPATH makes the executable's
# rpath apply transitively and avoids "libortools.so.9 not found" at startup.
# -----------------------------------------------------------------------------
target_link_options(bdr_coverage_planner PRIVATE "-Wl,--disable-new-dtags")

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
find_package(tf2 REQUIRED)
find_package(tf2_geometry_msgs REQUIRED)

# Add ROS2 includes and libraries
include_directories(${rclcpp_INCLUDE_DIRS})
target_link_libraries(bdr_coverage_planner ${rclcpp_LIBRARIES})
ament_target_dependencies(bdr_coverage_planner rclcpp std_msgs std_srvs geometry_msgs sensor_msgs nav_msgs tf2 tf2_geometry_msgs)

# Install
install(TARGETS bdr_coverage_planner
    RUNTIME DESTINATION bin
)

install(DIRECTORY config
    DESTINATION share/${PROJECT_NAME}
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
