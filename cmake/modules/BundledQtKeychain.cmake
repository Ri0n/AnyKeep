include(ExternalProject)
include(GNUInstallDirs)
include(ProcessorCount)

set(ANYKEEP_BUNDLED_QTKEYCHAIN_GIT_REPOSITORY
    "https://github.com/frankosterfeld/qtkeychain.git"
    CACHE STRING "Bundled QtKeychain git repository")
set(ANYKEEP_BUNDLED_QTKEYCHAIN_GIT_TAG
    "0.14.3"
    CACHE STRING "Bundled QtKeychain git tag")
set(ANYKEEP_QTKEYCHAIN_SOURCE_DIR
    ""
    CACHE PATH "Local QtKeychain source directory (avoids cloning the repository)")

processorcount(_qtkeychain_detected_jobs)
if(NOT _qtkeychain_detected_jobs)
  set(_qtkeychain_detected_jobs 2)
endif()
set(ANYKEEP_BUNDLED_QTKEYCHAIN_JOBS
    "${_qtkeychain_detected_jobs}"
    CACHE STRING "Parallel jobs used to build bundled QtKeychain")

set(_qtkeychain_prefix "${CMAKE_BINARY_DIR}/_deps/qtkeychain")
set(_qtkeychain_install_dir "${_qtkeychain_prefix}/install")
set(_qtkeychain_include_dir "${_qtkeychain_install_dir}/${CMAKE_INSTALL_INCLUDEDIR}")
set(_qtkeychain_library_dir "${_qtkeychain_install_dir}/${CMAKE_INSTALL_LIBDIR}")
set(_qtkeychain_debug_postfix "")
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_qtkeychain_debug_postfix "d")
endif()
set(_qtkeychain_library
    "${_qtkeychain_library_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}qt6keychain${_qtkeychain_debug_postfix}${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

if(ANYKEEP_QTKEYCHAIN_SOURCE_DIR)
  if(NOT EXISTS "${ANYKEEP_QTKEYCHAIN_SOURCE_DIR}/CMakeLists.txt")
    message(
      FATAL_ERROR
        "ANYKEEP_QTKEYCHAIN_SOURCE_DIR does not contain a QtKeychain source tree: ${ANYKEEP_QTKEYCHAIN_SOURCE_DIR}")
  endif()
  set(_qtkeychain_source_args SOURCE_DIR "${ANYKEEP_QTKEYCHAIN_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND "")
else()
  set(_qtkeychain_source_args GIT_REPOSITORY "${ANYKEEP_BUNDLED_QTKEYCHAIN_GIT_REPOSITORY}" GIT_TAG
                              "${ANYKEEP_BUNDLED_QTKEYCHAIN_GIT_TAG}" UPDATE_COMMAND "")
endif()

set(_qtkeychain_cmake_args
    "-DBUILD_SHARED_LIBS=OFF"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    "-DBUILD_TEST_APPLICATION=OFF"
    "-DBUILD_WITH_QT6=ON"
    "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
    "-DCMAKE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
    "-DCMAKE_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
    "-DQt6_DIR=${Qt6_DIR}"
    "-DQt6Core_DIR=${Qt6Core_DIR}"
    "-DQT_HOST_PATH=${QT_HOST_PATH}"
    "-DQT_HOST_PATH_CMAKE_DIR=${QT_HOST_PATH_CMAKE_DIR}"
    "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
    "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DANDROID_ABI=${ANDROID_ABI}"
    "-DCMAKE_ANDROID_ARCH_ABI=${ANDROID_ABI}"
    "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
    "-DANDROID_NDK=${ANDROID_NDK}"
    "-DLIBSECRET_SUPPORT=OFF"
    "-DOSX_FRAMEWORK=OFF")

anykeep_external_project_config_args(_qtkeychain_build_config_args)

ExternalProject_Add(
  anykeep_bundled_qtkeychain
  ${_qtkeychain_source_args}
  PREFIX "${_qtkeychain_prefix}"
  INSTALL_DIR "${_qtkeychain_install_dir}"
  CMAKE_ARGS ${_qtkeychain_cmake_args}
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> ${_qtkeychain_build_config_args} --parallel
                "${ANYKEEP_BUNDLED_QTKEYCHAIN_JOBS}"
  INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> ${_qtkeychain_build_config_args}
  BUILD_BYPRODUCTS "${_qtkeychain_library}")

file(MAKE_DIRECTORY "${_qtkeychain_include_dir}")

add_library(Qt6Keychain::Qt6Keychain STATIC IMPORTED GLOBAL)
set_target_properties(
  Qt6Keychain::Qt6Keychain
  PROPERTIES IMPORTED_LOCATION "${_qtkeychain_library}" INTERFACE_INCLUDE_DIRECTORIES "${_qtkeychain_include_dir}"
             INTERFACE_LINK_LIBRARIES "Qt6::Core")
if(APPLE)
  # The static Apple keychain backend calls SecItem* from Security.framework. Model that requirement on QtKeychain
  # itself rather than on libanykeep.
  target_link_libraries(Qt6Keychain::Qt6Keychain INTERFACE "-framework Security")
endif()
add_dependencies(Qt6Keychain::Qt6Keychain anykeep_bundled_qtkeychain)
