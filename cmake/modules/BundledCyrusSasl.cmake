include_guard(GLOBAL)
include(ExternalProject)
include(DependencyVersions)

set(ANYKEEP_CYRUS_SASL_ROOT "" CACHE PATH "Static Cyrus SASL SDK for the bundled QCA providers")
if(ANYKEEP_CYRUS_SASL_ROOT)
  find_path(ANYKEEP_CYRUS_SASL_INCLUDE_DIR sasl/sasl.h PATHS "${ANYKEEP_CYRUS_SASL_ROOT}/include"
            NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE REQUIRED)
  find_library(ANYKEEP_CYRUS_SASL_LIBRARY NAMES libsasl2-static sasl2-static sasl2
               PATHS "${ANYKEEP_CYRUS_SASL_ROOT}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE REQUIRED)
  return()
endif()
set(_cyrus_prefix "${CMAKE_BINARY_DIR}/_deps/cyrus-sasl")
set(ANYKEEP_CYRUS_SASL_ROOT "${_cyrus_prefix}/install")

if(ANDROID OR APPLE)
  if(ANDROID)
    set(_cyrus_platform "android-${CMAKE_ANDROID_ARCH_ABI}")
  else()
    set(_cyrus_arch "${CMAKE_OSX_ARCHITECTURES}")
    if(NOT _cyrus_arch)
      set(_cyrus_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(_cyrus_arch STREQUAL "aarch64")
      set(_cyrus_arch arm64)
    endif()
    set(_cyrus_platform "macos-${_cyrus_arch}")
  endif()
  string(JSON _cyrus_sha256 ERROR_VARIABLE _cyrus_hash_error
         GET "${ANYKEEP_DEP_CYRUS_SASL_HASHES}" "${_cyrus_platform}")
  if(_cyrus_hash_error)
    message(FATAL_ERROR "No pinned Cyrus SASL bundle for ${_cyrus_platform}; set ANYKEEP_CYRUS_SASL_ROOT to a matching static SDK")
  endif()
  set(_cyrus_bundle "cyrus-sasl-${ANYKEEP_DEP_CYRUS_SASL_VERSION}-${ANYKEEP_DEP_CYRUS_SASL_REVISION}")
  set(ANYKEEP_CYRUS_SASL_LIBRARY "${ANYKEEP_CYRUS_SASL_ROOT}/lib/libsasl2.a")
  # Use the same PIC PLAIN/SCRAM archive as upstream QCA SDKs. Its OpenSSL dependency is linked by QCA.
  ExternalProject_Add(anykeep_bundled_cyrus_sasl
    URL "https://github.com/psi-im/qca/releases/download/deps-${_cyrus_bundle}/${_cyrus_bundle}-${_cyrus_platform}.tar.gz"
    URL_HASH "SHA256=${_cyrus_sha256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${_cyrus_prefix}"
    INSTALL_DIR "${ANYKEEP_CYRUS_SASL_ROOT}"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND "${CMAKE_COMMAND}" -E copy_directory <SOURCE_DIR> <INSTALL_DIR>
    BUILD_BYPRODUCTS "${ANYKEEP_CYRUS_SASL_LIBRARY}")
  return()
endif()
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "Set ANYKEEP_CYRUS_SASL_ROOT to a matching static Cyrus SASL SDK for this toolchain")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(_cyrus_nmake NAMES nmake REQUIRED)
set(ANYKEEP_CYRUS_SASL_GIT_REPOSITORY "https://github.com/Ri0n/cyrus-sasl.git"
    CACHE STRING "Bundled Cyrus SASL source repository")
set(ANYKEEP_CYRUS_SASL_LIBRARY "${ANYKEEP_CYRUS_SASL_ROOT}/lib/libsasl2-static.lib")

# This fork provides SCRAM with Windows CNG and QCA crypto callbacks. Build
# with the application's CRT, including /MDd for a Debug build.
ExternalProject_Add(anykeep_bundled_cyrus_sasl
  GIT_REPOSITORY "${ANYKEEP_CYRUS_SASL_GIT_REPOSITORY}"
  GIT_TAG "${ANYKEEP_DEP_CYRUS_SASL_COMMIT}"
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
