#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "box3d::box3d" for configuration "Debug"
set_property(TARGET box3d::box3d APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(box3d::box3d PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/box3dd.lib"
  )

list(APPEND _cmake_import_check_targets box3d::box3d )
list(APPEND _cmake_import_check_files_for_box3d::box3d "${_IMPORT_PREFIX}/lib/box3dd.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
