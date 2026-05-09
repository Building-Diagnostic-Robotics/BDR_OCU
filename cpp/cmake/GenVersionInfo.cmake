# GenVersionInfo.cmake
#
# Renders include/version_info.hpp.in -> ${BIN_DIR}/generated/version_info.hpp
# Re-runs each build so APP_COMMIT_SHA stays in sync with HEAD.
#
# Required cache vars (passed via -D):
#   SRC_DIR         absolute path to cpp/ source dir
#   BIN_DIR         absolute path to cpp/build dir
#   APP_SEMVER      e.g. "1.0.0" (CMake project version)
#   APP_REPO_OWNER  GitHub org/user that owns the OTA repo
#   APP_REPO_NAME   GitHub repo name

if(NOT SRC_DIR OR NOT BIN_DIR OR NOT APP_SEMVER)
    message(FATAL_ERROR "GenVersionInfo.cmake: SRC_DIR/BIN_DIR/APP_SEMVER required")
endif()

if(NOT APP_REPO_OWNER)
    set(APP_REPO_OWNER "Building-Diagnostic-Robotics")
endif()
if(NOT APP_REPO_NAME)
    set(APP_REPO_NAME "BDR_OCU")
endif()

# Find git short SHA. Walk up from SRC_DIR so this works whether CMakeLists.txt
# lives at repo root or in a subdir (cpp/).
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${SRC_DIR}
    OUTPUT_VARIABLE APP_COMMIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_sha_rc
    ERROR_QUIET
)
if(NOT _git_sha_rc EQUAL 0 OR APP_COMMIT_SHA STREQUAL "")
    set(APP_COMMIT_SHA "unknown")
endif()

# Mark dirty if the working tree has uncommitted changes — avoids OTA reporting
# a SHA that doesn't match what's on the remote.
execute_process(
    COMMAND git diff --quiet --ignore-submodules HEAD
    WORKING_DIRECTORY ${SRC_DIR}
    RESULT_VARIABLE _git_dirty_rc
    ERROR_QUIET
)
if(_git_dirty_rc EQUAL 1)
    set(APP_COMMIT_SHA "${APP_COMMIT_SHA}-dirty")
endif()

string(TIMESTAMP APP_BUILD_DATE "%Y-%m-%d" UTC)

set(_template "${SRC_DIR}/include/version_info.hpp.in")
set(_out_dir  "${BIN_DIR}/generated")
set(_out_file "${_out_dir}/version_info.hpp")
set(_tmp_file "${_out_file}.tmp")

file(MAKE_DIRECTORY ${_out_dir})

configure_file(${_template} ${_tmp_file} @ONLY)

# Avoid spurious rebuilds when SHA hasn't changed.
if(EXISTS ${_out_file})
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E compare_files ${_tmp_file} ${_out_file}
        RESULT_VARIABLE _files_differ
        OUTPUT_QUIET ERROR_QUIET
    )
else()
    set(_files_differ 1)
endif()

if(_files_differ)
    file(RENAME ${_tmp_file} ${_out_file})
    message(STATUS "version_info.hpp regenerated (sha=${APP_COMMIT_SHA})")
else()
    file(REMOVE ${_tmp_file})
endif()
