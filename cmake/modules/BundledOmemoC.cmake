if(DEFINED ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS AND NOT ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS)
  message(FATAL_ERROR "BundledOmemoC.cmake was included while ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS=OFF")
endif()

include(ExternalProject)
include(ProcessorCount)

set(ANYKEEP_BUNDLED_OMEMO_C_VERSION
    "0.5.1"
    CACHE STRING "Bundled libomemo-c version")
set(ANYKEEP_BUNDLED_PROTOBUF_C_VERSION
    "1.5.1"
    CACHE STRING "Bundled protobuf-c runtime version")
set(ANYKEEP_OMEMO_C_SOURCE_DIR
    ""
    CACHE PATH "Local libomemo-c source directory")
set(ANYKEEP_PROTOBUF_C_SOURCE_DIR
    ""
    CACHE PATH "Local protobuf-c source directory")

processorcount(_omemoc_detected_jobs)
if(NOT _omemoc_detected_jobs)
  set(_omemoc_detected_jobs 2)
endif()
set(ANYKEEP_BUNDLED_OMEMO_C_JOBS
    "${_omemoc_detected_jobs}"
    CACHE STRING "Parallel jobs used to build bundled libomemo-c dependencies")

set(_omemoc_prefix "${CMAKE_BINARY_DIR}/_deps/omemo-c")
# protobuf-c and libomemo-c intentionally share one install prefix. The libomemo-c pkg-config file links -lprotobuf-c
# without a separate -L entry, so both static archives need to live in the same library directory.
set(ANYKEEP_OMEMO_C_INSTALL_DIR "${_omemoc_prefix}/install")
set(ANYKEEP_OMEMO_C_PKGCONFIG_DIR "${ANYKEEP_OMEMO_C_INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/pkgconfig")
set(_omemoc_library_dir "${ANYKEEP_OMEMO_C_INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}")
set(_omemoc_include_dir "${ANYKEEP_OMEMO_C_INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR}/omemo")
set(_protobuf_c_include_root "${ANYKEEP_OMEMO_C_INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR}")
set(_omemoc_library "${_omemoc_library_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}omemo-c${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(_protobuf_c_library "${_omemoc_library_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}protobuf-c${CMAKE_STATIC_LIBRARY_SUFFIX}")

function(_anykeep_append_cross_compile_args output_var)
  set(_args ${${output_var}})
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
          QT_HOST_PATH_CMAKE_DIR)
    if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
      string(REPLACE ";" "|" _value "${${_var}}")
      list(APPEND _args "-D${_var}=${_value}")
    endif()
  endforeach()
  set(${output_var}
      "${_args}"
      PARENT_SCOPE)
endfunction()

if(ANYKEEP_PROTOBUF_C_SOURCE_DIR)
  if(NOT EXISTS "${ANYKEEP_PROTOBUF_C_SOURCE_DIR}/protobuf-c/protobuf-c.c")
    message(FATAL_ERROR "ANYKEEP_PROTOBUF_C_SOURCE_DIR does not contain a protobuf-c source tree: "
                        "${ANYKEEP_PROTOBUF_C_SOURCE_DIR}")
  endif()
  set(_protobuf_c_source_args SOURCE_DIR "${ANYKEEP_PROTOBUF_C_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND "")
else()
  set(_protobuf_c_source_args
      GIT_REPOSITORY
      https://github.com/protobuf-c/protobuf-c.git
      GIT_TAG
      "v${ANYKEEP_BUNDLED_PROTOBUF_C_VERSION}"
      GIT_SHALLOW
      TRUE
      GIT_PROGRESS
      TRUE)
endif()

set(_protobuf_c_cmake_args
    "-DPROTOBUF_C_SOURCE_DIR=<SOURCE_DIR>" "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
    "-DCMAKE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}" "-DCMAKE_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}" "-DCMAKE_POSITION_INDEPENDENT_CODE=ON")
if(CMAKE_C_COMPILER)
  list(APPEND _protobuf_c_cmake_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if(CMAKE_C_COMPILER_LAUNCHER)
  list(APPEND _protobuf_c_cmake_args "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}")
endif()
_anykeep_append_cross_compile_args(_protobuf_c_cmake_args)

# Single-config generators use CMAKE_BUILD_TYPE. Multi-config generators need the active configuration selected
# explicitly for both build and install. The wrapper project may use Visual Studio even if the top-level project uses
# Ninja, so omitting --config would otherwise build Debug and install Release.
if(CMAKE_CONFIGURATION_TYPES)
  set(_omemoc_build_config "$<CONFIG>")
else()
  set(_omemoc_build_config "${CMAKE_BUILD_TYPE}")
endif()

ExternalProject_Add(
  anykeep_bundled_protobuf_c
  ${_protobuf_c_source_args}
  PREFIX "${_omemoc_prefix}/protobuf-c"
  SOURCE_SUBDIR "."
  INSTALL_DIR "${ANYKEEP_OMEMO_C_INSTALL_DIR}"
  LIST_SEPARATOR "|"
  CONFIGURE_COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/protobuf-c-runtime" -B <BINARY_DIR>
                    ${_protobuf_c_cmake_args}
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config "${_omemoc_build_config}" --parallel
                "${ANYKEEP_BUNDLED_OMEMO_C_JOBS}"
  INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config "${_omemoc_build_config}"
  BUILD_BYPRODUCTS "${_protobuf_c_library}")

if(ANYKEEP_OMEMO_C_SOURCE_DIR)
  if(NOT EXISTS "${ANYKEEP_OMEMO_C_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "ANYKEEP_OMEMO_C_SOURCE_DIR does not contain a libomemo-c source tree: "
                        "${ANYKEEP_OMEMO_C_SOURCE_DIR}")
  endif()
  set(_omemoc_source_args SOURCE_DIR "${ANYKEEP_OMEMO_C_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND "")
else()
  set(_omemoc_source_args
      GIT_REPOSITORY
      https://github.com/dino/libomemo-c.git
      GIT_TAG
      "v${ANYKEEP_BUNDLED_OMEMO_C_VERSION}"
      GIT_SHALLOW
      TRUE
      GIT_PROGRESS
      TRUE)
endif()

set(_omemoc_c_flags "${CMAKE_C_FLAGS}")
string(APPEND _omemoc_c_flags " -I${ANYKEEP_OMEMO_C_INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR}")
set(_omemoc_cmake_args
    "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
    "-DLIB_INSTALL_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_LIBDIR}"
    "-DINCLUDE_INSTALL_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_INCLUDEDIR}"
    "-DINSTALL_PKGCONFIG_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DBUILD_TESTING=OFF"
    "-DCMAKE_C_FLAGS=${_omemoc_c_flags}")

# libomemo-c 0.5.1 still declares cmake_minimum_required(VERSION 2.8.4). CMake 4 removed policy compatibility below 3.5.
# This variable is the supported external override for unmodified third-party projects.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
  list(APPEND _omemoc_cmake_args "-DCMAKE_POLICY_VERSION_MINIMUM=3.5")
endif()

if(CMAKE_C_COMPILER)
  list(APPEND _omemoc_cmake_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if(CMAKE_C_COMPILER_LAUNCHER)
  list(APPEND _omemoc_cmake_args "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}")
endif()
_anykeep_append_cross_compile_args(_omemoc_cmake_args)

ExternalProject_Add(
  anykeep_bundled_omemoc
  ${_omemoc_source_args}
  PREFIX "${_omemoc_prefix}/libomemo-c"
  INSTALL_DIR "${ANYKEEP_OMEMO_C_INSTALL_DIR}"
  LIST_SEPARATOR "|"
  CMAKE_ARGS ${_omemoc_cmake_args}
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config "${_omemoc_build_config}" --parallel
                "${ANYKEEP_BUNDLED_OMEMO_C_JOBS}"
  INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config "${_omemoc_build_config}"
  BUILD_BYPRODUCTS "${_omemoc_library}"
  DEPENDS anykeep_bundled_protobuf_c)

# Imported targets make the bundled stack look like pkg_check_modules(... IMPORTED_TARGET libomemo-c) to the rest of
# AnyKeep. QXmpp itself still finds the generated .pc file through CMAKE_PREFIX_PATH during its ExternalProject
# configure step.
file(MAKE_DIRECTORY "${_omemoc_include_dir}" "${_protobuf_c_include_root}/protobuf-c"
     "${ANYKEEP_OMEMO_C_PKGCONFIG_DIR}")

add_library(AnyKeepProtobufC STATIC IMPORTED GLOBAL)
set_target_properties(AnyKeepProtobufC PROPERTIES IMPORTED_LOCATION "${_protobuf_c_library}"
                                                  INTERFACE_INCLUDE_DIRECTORIES "${_protobuf_c_include_root}")
add_dependencies(AnyKeepProtobufC anykeep_bundled_protobuf_c)

add_library(PkgConfig::OmemoC STATIC IMPORTED GLOBAL)
set_target_properties(
  PkgConfig::OmemoC
  PROPERTIES IMPORTED_LOCATION "${_omemoc_library}" INTERFACE_INCLUDE_DIRECTORIES "${_omemoc_include_dir}"
             INTERFACE_LINK_LIBRARIES AnyKeepProtobufC)
add_dependencies(PkgConfig::OmemoC anykeep_bundled_omemoc)
