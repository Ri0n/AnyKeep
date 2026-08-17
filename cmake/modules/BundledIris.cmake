include(ExternalProject)
include(GNUInstallDirs)
include(ProcessorCount)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Network Xml)
if(NOT TARGET qca-qt6)
  message(FATAL_ERROR "Iris backend requires QCA (qca-qt6 target)")
endif()

set(ANYKEEP_BUNDLED_IRIS_GIT_REPOSITORY
    "https://github.com/psi-im/iris.git"
    CACHE STRING "Iris git repository")
# Keep the repository's default Iris revision source-controlled.  A cache
# entry for the default pin silently survives source updates and can make a
# clean ExternalProject checkout build an older Iris revision.
set(_anykeep_iris_git_tag "54c6c7c8a43658ab97cc8e300298913070ea91c8")
set(ANYKEEP_IRIS_GIT_TAG
    ""
    CACHE STRING "Override the Iris git tag, branch, or commit")
if(ANYKEEP_IRIS_GIT_TAG)
  set(_iris_git_tag "${ANYKEEP_IRIS_GIT_TAG}")
else()
  set(_iris_git_tag "${_anykeep_iris_git_tag}")
endif()
set(ANYKEEP_IRIS_SOURCE_DIR
    ""
    CACHE PATH "Local Iris source directory (avoids cloning the repository)")
processorcount(_iris_detected_jobs)
if(NOT _iris_detected_jobs)
  set(_iris_detected_jobs 2)
endif()
set(ANYKEEP_BUNDLED_IRIS_JOBS
    "${_iris_detected_jobs}"
    CACHE STRING "Parallel jobs used to build Iris")

set(_iris_prefix "${CMAKE_BINARY_DIR}/_deps/iris")
set(_iris_install_dir "${_iris_prefix}/install")
set(_iris_include_dir "${_iris_install_dir}/${CMAKE_INSTALL_INCLUDEDIR}/xmpp")
# Installed Iris wrapper headers include public implementation headers as
# "xmpp/...". The latter live below include/xmpp/iris, so consumers need
# both include roots. This mirrors Iris' installed target interface.
set(_iris_xmpp_include_dir "${_iris_include_dir}/iris")

if(WIN32)
  set(_iris_runtime "${_iris_install_dir}/${CMAKE_INSTALL_BINDIR}/iris${CMAKE_SHARED_LIBRARY_SUFFIX}")
  set(_iris_implib "${_iris_install_dir}/${CMAKE_INSTALL_LIBDIR}/${CMAKE_IMPORT_LIBRARY_PREFIX}iris${CMAKE_IMPORT_LIBRARY_SUFFIX}")
  set(_iris_byproducts "${_iris_runtime}" "${_iris_implib}")
else()
  set(_iris_runtime "${_iris_install_dir}/${CMAKE_INSTALL_LIBDIR}/${CMAKE_SHARED_LIBRARY_PREFIX}iris${CMAKE_SHARED_LIBRARY_SUFFIX}")
  set(_iris_byproducts "${_iris_runtime}")
endif()

if(ANYKEEP_IRIS_SOURCE_DIR)
  if(NOT EXISTS "${ANYKEEP_IRIS_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "ANYKEEP_IRIS_SOURCE_DIR does not contain an Iris source tree: ${ANYKEEP_IRIS_SOURCE_DIR}")
  endif()
  set(_iris_source_args SOURCE_DIR "${ANYKEEP_IRIS_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND "")
else()
  set(_iris_source_args
      GIT_REPOSITORY "${ANYKEEP_BUNDLED_IRIS_GIT_REPOSITORY}"
      GIT_TAG "${_iris_git_tag}"
      GIT_SHALLOW FALSE
      UPDATE_COMMAND "")
endif()

# Iris and AnyKeep must resolve QCA to the same library instance. In
# particular, do not let a shared Iris ExternalProject embed its own static
# QCA when AnyKeep already links QCA. BundledQca builds shared QCA for the
# Iris backend, and Iris consumes that installed ExternalProject output.
set(_iris_prefix_path "${CMAKE_PREFIX_PATH}")
if(ANYKEEP_QCA_INSTALL_DIR)
  list(PREPEND _iris_prefix_path "${ANYKEEP_QCA_INSTALL_DIR}")
endif()
string(REPLACE ";" "|" _iris_prefix_path_arg "${_iris_prefix_path}")

set(_iris_cmake_args
    "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
    "-DCMAKE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
    "-DCMAKE_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
    "-DCMAKE_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_PREFIX_PATH=${_iris_prefix_path_arg}"
    "-DBUILD_SHARED_LIBS=ON"
    "-DUSE_QT6=ON"
    "-DIRIS_ENABLE_INSTALL=ON"
    "-DIRIS_ENABLE_OMEMO=ON"
    "-DIRIS_BUNDLED_OMEMO_C=ON"
    "-DIRIS_BUNDLED_QCA=OFF"
    # File transfer over Jingle S5B/IBB/ICE is part of core Iris. SCTP is the
    # optional WebRTC data-channel transport and brings an extra usrsctp dependency.
    "-DIRIS_ENABLE_JINGLE_SCTP=OFF"
    "-DIRIS_BUILD_TOOLS=OFF")
if(ANDROID AND DEFINED ANYKEEP_ANDROID_OPENSSL_ROOT AND NOT "${ANYKEEP_ANDROID_OPENSSL_ROOT}" STREQUAL "")
  list(APPEND _iris_cmake_args "-DIRIS_ANDROID_OPENSSL_ROOT=${ANYKEEP_ANDROID_OPENSSL_ROOT}")
endif()
if(ANYKEEP_QCA_INSTALL_DIR)
  list(APPEND _iris_cmake_args "-DQCA_DIR=${ANYKEEP_QCA_INSTALL_DIR}")
endif()
if(CMAKE_C_COMPILER)
  list(APPEND _iris_cmake_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if(CMAKE_CXX_COMPILER)
  list(APPEND _iris_cmake_args "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endif()
if(CMAKE_C_COMPILER_LAUNCHER)
  list(APPEND _iris_cmake_args "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}")
endif()
if(CMAKE_CXX_COMPILER_LAUNCHER)
  list(APPEND _iris_cmake_args "-DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}")
endif()
foreach(
  _var IN
  ITEMS CMAKE_TOOLCHAIN_FILE
        CMAKE_SYSROOT
        CMAKE_STAGING_PREFIX
        CMAKE_FIND_ROOT_PATH
        CMAKE_OSX_SYSROOT
        CMAKE_OSX_ARCHITECTURES
        ANDROID_ABI
        ANDROID_PLATFORM
        ANDROID_NDK
        CMAKE_ANDROID_NDK
        CMAKE_ANDROID_ARCH_ABI
        CMAKE_ANDROID_API
        QT_HOST_PATH
        QT_HOST_PATH_CMAKE_DIR
        Qt6_DIR
        Qt6Core_DIR
        Qt6Gui_DIR
        Qt6Network_DIR
        Qt6Xml_DIR)
  if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
    string(REPLACE ";" "|" _value "${${_var}}")
    list(APPEND _iris_cmake_args "-D${_var}=${_value}")
  endif()
endforeach()

set(_iris_external_dependency_args)
if(TARGET anykeep_bundled_qca)
  list(APPEND _iris_external_dependency_args DEPENDS anykeep_bundled_qca)
endif()

anykeep_external_project_config_args(_iris_build_config_args)
ExternalProject_Add(
  anykeep_bundled_iris
  ${_iris_source_args}
  PREFIX "${_iris_prefix}"
  INSTALL_DIR "${_iris_install_dir}"
  LIST_SEPARATOR "|"
  CMAKE_ARGS ${_iris_cmake_args}
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> ${_iris_build_config_args} --parallel
                "${ANYKEEP_BUNDLED_IRIS_JOBS}"
  INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> ${_iris_build_config_args}
  BUILD_BYPRODUCTS ${_iris_byproducts}
  ${_iris_external_dependency_args})

file(MAKE_DIRECTORY "${_iris_include_dir}" "${_iris_xmpp_include_dir}")
add_library(Iris::Iris SHARED IMPORTED GLOBAL)
set_target_properties(
  Iris::Iris PROPERTIES
  IMPORTED_LOCATION "${_iris_runtime}"
  INTERFACE_INCLUDE_DIRECTORIES "${_iris_include_dir};${_iris_xmpp_include_dir}"
  INTERFACE_LINK_LIBRARIES "qca-qt6;Qt6::Core;Qt6::Gui;Qt6::Network;Qt6::Xml"
  INTERFACE_COMPILE_DEFINITIONS "IRIS_ENABLE_OMEMO=1;IRISNET_STATIC;QSTRINGPREP_BUILDING")
if(WIN32)
  set_target_properties(Iris::Iris PROPERTIES IMPORTED_IMPLIB "${_iris_implib}")
endif()
add_dependencies(Iris::Iris anykeep_bundled_iris)

set(ANYKEEP_IRIS_INSTALL_DIR "${_iris_install_dir}")

if(WIN32)
  install(FILES "${_iris_runtime}" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Libraries)
else()
  install(FILES "${_iris_runtime}" DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Libraries)
endif()
