
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was Fields2CoverConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

include("${CMAKE_CURRENT_LIST_DIR}/Fields2CoverTargets.cmake")

include(CMakeFindDependencyMacro)

# Find dependencies
find_dependency(GDAL 3.0 REQUIRED)
find_dependency(Threads REQUIRED)
find_dependency(Eigen3 REQUIRED)

# Optional dependencies
if()
  find_dependency(TBB REQUIRED)
endif()

set_and_check(Fields2Cover_INCLUDE_DIR "${PACKAGE_PREFIX_DIR}/include")
check_required_components(Fields2Cover)



