#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Fields2Cover::Fields2Cover" for configuration "Release"
set_property(TARGET Fields2Cover::Fields2Cover APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Fields2Cover::Fields2Cover PROPERTIES
  IMPORTED_LINK_DEPENDENT_LIBRARIES_RELEASE "ortools::ortools;Fields2Cover::steering_functions;Fields2Cover::matplot"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libFields2Cover.so"
  IMPORTED_SONAME_RELEASE "libFields2Cover.so"
  )

list(APPEND _IMPORT_CHECK_TARGETS Fields2Cover::Fields2Cover )
list(APPEND _IMPORT_CHECK_FILES_FOR_Fields2Cover::Fields2Cover "${_IMPORT_PREFIX}/lib/libFields2Cover.so" )

# Import target "Fields2Cover::steering_functions" for configuration "Release"
set_property(TARGET Fields2Cover::steering_functions APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Fields2Cover::steering_functions PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libsteering_functions.so"
  IMPORTED_SONAME_RELEASE "libsteering_functions.so"
  )

list(APPEND _IMPORT_CHECK_TARGETS Fields2Cover::steering_functions )
list(APPEND _IMPORT_CHECK_FILES_FOR_Fields2Cover::steering_functions "${_IMPORT_PREFIX}/lib/libsteering_functions.so" )

# Import target "Fields2Cover::matplot" for configuration "Release"
set_property(TARGET Fields2Cover::matplot APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Fields2Cover::matplot PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libmatplot.so.1.2.0"
  IMPORTED_SONAME_RELEASE "libmatplot.so.1"
  )

list(APPEND _IMPORT_CHECK_TARGETS Fields2Cover::matplot )
list(APPEND _IMPORT_CHECK_FILES_FOR_Fields2Cover::matplot "${_IMPORT_PREFIX}/lib/libmatplot.so.1.2.0" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
