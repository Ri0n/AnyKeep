include_guard(GLOBAL)
include(ExternalProject)

set(ANYKEEP_CYRUS_SASL_ROOT "" CACHE PATH "Cyrus SASL SDK for the bundled Windows QCA providers")
if(ANYKEEP_CYRUS_SASL_ROOT)
  find_path(ANYKEEP_CYRUS_SASL_INCLUDE_DIR sasl/sasl.h PATHS "${ANYKEEP_CYRUS_SASL_ROOT}/include" NO_DEFAULT_PATH NO_CACHE REQUIRED)
  find_library(ANYKEEP_CYRUS_SASL_LIBRARY NAMES libsasl2-static sasl2-static sasl2
               PATHS "${ANYKEEP_CYRUS_SASL_ROOT}/lib" NO_DEFAULT_PATH NO_CACHE REQUIRED)
  return()
endif()
if(NOT MSVC)
  message(FATAL_ERROR "Set ANYKEEP_CYRUS_SASL_ROOT to a matching Cyrus SASL SDK for this Windows toolchain")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(_cyrus_nmake NAMES nmake REQUIRED)
set(_cyrus_prefix "${CMAKE_BINARY_DIR}/_deps/cyrus-sasl")
set(ANYKEEP_CYRUS_SASL_GIT_REPOSITORY "https://github.com/Ri0n/cyrus-sasl.git"
    CACHE STRING "Bundled Cyrus SASL source repository")
set(ANYKEEP_CYRUS_SASL_ROOT "${_cyrus_prefix}/install")
set(ANYKEEP_CYRUS_SASL_LIBRARY "${ANYKEEP_CYRUS_SASL_ROOT}/lib/libsasl2-static.lib")

# This fork provides SCRAM with Windows CNG and QCA crypto callbacks. Build
# with the application's CRT, including /MDd for a Debug build.
ExternalProject_Add(anykeep_bundled_cyrus_sasl
  GIT_REPOSITORY "${ANYKEEP_CYRUS_SASL_GIT_REPOSITORY}"
  GIT_TAG 590be2c5bd2ae9824631061dddc50526a2ab2038
  UPDATE_COMMAND ""
  PREFIX "${_cyrus_prefix}"
  INSTALL_DIR "${ANYKEEP_CYRUS_SASL_ROOT}"
  PATCH_COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../scripts/prepare-cyrus-windows.py" <SOURCE_DIR>
  CONFIGURE_COMMAND ""
  BUILD_IN_SOURCE TRUE
  BUILD_COMMAND "${CMAKE_COMMAND}" -E chdir <SOURCE_DIR>/lib "${_cyrus_nmake}" /f NTMakefile
                STATIC=no STATIC_BUNDLE=1 SASL_CRYPTO=cng
                "CODEGEN=$<IF:$<CONFIG:Debug>,/MDd,/MD>" "CFG=$<IF:$<CONFIG:Debug>,Debug,Release>"
  INSTALL_COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>" "-DINSTALL_DIR=<INSTALL_DIR>"
                  -P "${CMAKE_CURRENT_LIST_DIR}/../scripts/install-cyrus-windows.cmake"
  BUILD_BYPRODUCTS "${ANYKEEP_CYRUS_SASL_LIBRARY}")
