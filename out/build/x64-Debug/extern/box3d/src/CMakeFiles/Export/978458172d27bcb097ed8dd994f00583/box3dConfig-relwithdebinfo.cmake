#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "box3d::box3d" for configuration "RelWithDebInfo"
set_property(TARGET box3d::box3d APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(box3d::box3d PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "C"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/box3d.lib"
  )

list(APPEND _cmake_import_check_targets box3d::box3d )
list(APPEND _cmake_import_check_files_for_box3d::box3d "${_IMPORT_PREFIX}/lib/box3d.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
