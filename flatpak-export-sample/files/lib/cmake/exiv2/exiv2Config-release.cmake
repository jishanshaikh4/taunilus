#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "exiv2-xmp" for configuration "Release"
set_property(TARGET exiv2-xmp APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(exiv2-xmp PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "/app/lib/libexiv2-xmp.a"
  )

list(APPEND _IMPORT_CHECK_TARGETS exiv2-xmp )
list(APPEND _IMPORT_CHECK_FILES_FOR_exiv2-xmp "/app/lib/libexiv2-xmp.a" )

# Import target "exiv2lib" for configuration "Release"
set_property(TARGET exiv2lib APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(exiv2lib PROPERTIES
  IMPORTED_LOCATION_RELEASE "/app/lib/libexiv2.so.0.27.5"
  IMPORTED_SONAME_RELEASE "libexiv2.so.27"
  )

list(APPEND _IMPORT_CHECK_TARGETS exiv2lib )
list(APPEND _IMPORT_CHECK_FILES_FOR_exiv2lib "/app/lib/libexiv2.so.0.27.5" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
