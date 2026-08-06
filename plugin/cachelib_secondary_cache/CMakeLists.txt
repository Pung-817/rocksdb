if(NOT CACHELIB_ROOT)
  message(FATAL_ERROR
    "CACHELIB_ROOT is required for cachelib_secondary_cache")
endif()

if(NOT USE_RTTI STREQUAL "ON")
  message(FATAL_ERROR
    "cachelib_secondary_cache requires -DUSE_RTTI=ON")
endif()

find_library(CACHELIB_NAVY_LIBRARY
  NAMES cachelib_navy
  PATHS "${CACHELIB_ROOT}/lib"
  NO_DEFAULT_PATH
  REQUIRED)
find_library(CACHELIB_FOLLY_LIBRARY
  NAMES folly
  PATHS
    "${CACHELIB_ROOT}/lib"
    ${CACHELIB_DEPENDENCY_LIBRARY_DIRS}
  NO_DEFAULT_PATH
  REQUIRED)
find_library(CACHELIB_THRIFT_PROTOCOL_LIBRARY
  NAMES thriftprotocol
  PATHS
    "${CACHELIB_ROOT}/lib"
    ${CACHELIB_DEPENDENCY_LIBRARY_DIRS}
  NO_DEFAULT_PATH
  REQUIRED)

add_library(cachelib_secondary_cache_dependencies UNKNOWN IMPORTED GLOBAL)
set_target_properties(cachelib_secondary_cache_dependencies PROPERTIES
  IMPORTED_LOCATION "${CACHELIB_NAVY_LIBRARY}"
  INTERFACE_COMPILE_OPTIONS -fexceptions
  INTERFACE_INCLUDE_DIRECTORIES
    "${CACHELIB_ROOT}/include;${CACHELIB_DEPENDENCY_INCLUDE_DIRS}"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
    "${CACHELIB_ROOT}/include;${CACHELIB_DEPENDENCY_INCLUDE_DIRS}"
  INTERFACE_LINK_LIBRARIES
    "${CACHELIB_FOLLY_LIBRARY};${CACHELIB_THRIFT_PROTOCOL_LIBRARY}")

set(cachelib_secondary_cache_LIBS
  cachelib_secondary_cache_dependencies
  PARENT_SCOPE)
set(cachelib_secondary_cache_TESTS
  cachelib_secondary_cache_test.cc
  PARENT_SCOPE)
