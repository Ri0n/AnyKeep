include_guard(GLOBAL)

# Qt Creator's package-manager helper is intentionally skipped for this project on Windows. Use the self-contained Conan
# setup below instead, so a newly configured Windows build can find the OpenSSL required by bundled QXmpp without
# relying on a previously generated build directory.
set(_anykeep_use_conan_default OFF)
if(WIN32)
  set(_anykeep_use_conan_default ON)
endif()
option(ANYKEEP_USE_CONAN "Resolve conanfile.txt dependencies during CMake configure" ${_anykeep_use_conan_default})
unset(_anykeep_use_conan_default)
set(ANYKEEP_CONAN_BUILD_POLICY
    "missing"
    CACHE STRING "Value passed to Conan's --build option")

function(anykeep_setup_conan_dependencies)
  if(NOT ANYKEEP_USE_CONAN)
    return()
  endif()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/conanfile.txt")
    message(FATAL_ERROR "ANYKEEP_USE_CONAN is ON, but conanfile.txt is missing")
  endif()

  find_program(ANYKEEP_CONAN_COMMAND NAMES conan REQUIRED)
  execute_process(
    COMMAND "${ANYKEEP_CONAN_COMMAND}" --version
    RESULT_VARIABLE _conan_version_result
    OUTPUT_VARIABLE _conan_version
    ERROR_VARIABLE _conan_version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _conan_version_result EQUAL 0)
    message(FATAL_ERROR "Could not run Conan: ${_conan_version_error}")
  endif()

  execute_process(COMMAND "${ANYKEEP_CONAN_COMMAND}" profile path default RESULT_VARIABLE _conan_profile_result
                                                                                          OUTPUT_QUIET ERROR_QUIET)
  if(NOT _conan_profile_result EQUAL 0)
    message(FATAL_ERROR "Conan's default profile is missing. Run 'conan profile detect' once, " "then configure again.")
  endif()

  set(_conan_output_dir "${CMAKE_BINARY_DIR}/conan")
  file(MAKE_DIRECTORY "${_conan_output_dir}")
  file(READ "${CMAKE_SOURCE_DIR}/conanfile.txt" _conanfile_contents)
  if(ANYKEEP_CONAN_EXTRA_REQUIRES)
    string(JOIN "\n" _conan_extra_requires ${ANYKEEP_CONAN_EXTRA_REQUIRES})
    string(REPLACE "# ANYKEEP_CMAKE_EXTRA_REQUIRES" "${_conan_extra_requires}\n# ANYKEEP_CMAKE_EXTRA_REQUIRES"
                   _conanfile_contents "${_conanfile_contents}")
  endif()
  file(WRITE "${_conan_output_dir}/conanfile.txt" "${_conanfile_contents}")
  set(_conan_common_args --profile:host=default --profile:build=default "--build=${ANYKEEP_CONAN_BUILD_POLICY}" "-s:h"
                         "compiler.cppstd=${CMAKE_CXX_STANDARD}")

  if(MSVC)
    string(REGEX MATCH "^19\\.([0-9]+)" _msvc_version_match "${CMAKE_CXX_COMPILER_VERSION}")
    if(_msvc_version_match)
      set(_msvc_minor "${CMAKE_MATCH_1}")
      math(EXPR _conan_msvc_version "190 + ${_msvc_minor} / 10")
      list(
        APPEND
        _conan_common_args
        "-s:h"
        "compiler=msvc"
        "-s:h"
        "compiler.version=${_conan_msvc_version}"
        "-s:h"
        "compiler.runtime=dynamic")
    endif()
  endif()

  if(CMAKE_CONFIGURATION_TYPES)
    set(_conan_build_types ${CMAKE_CONFIGURATION_TYPES})
  elseif(CMAKE_BUILD_TYPE)
    set(_conan_build_types "${CMAKE_BUILD_TYPE}")
  else()
    message(FATAL_ERROR "ANYKEEP_USE_CONAN requires CMAKE_BUILD_TYPE for a single-config generator")
  endif()

  foreach(_build_type IN LISTS _conan_build_types)
    set(_conan_args ${_conan_common_args} "-s:h" "build_type=${_build_type}")
    if(MSVC)
      if(_build_type STREQUAL "Debug")
        list(APPEND _conan_args "-s:h" "compiler.runtime_type=Debug")
      else()
        list(APPEND _conan_args "-s:h" "compiler.runtime_type=Release")
      endif()
    endif()

    string(JOIN ";" _conan_signature_input "${_conan_version}" "${_conanfile_contents}" "${_conan_args}")
    string(SHA256 _conan_signature "${_conan_signature_input}")
    string(TOLOWER "${_build_type}" _build_type_lower)
    set(_conan_stamp "${_conan_output_dir}/.anykeep-${_build_type_lower}.stamp")
    if(EXISTS "${_conan_stamp}")
      file(READ "${_conan_stamp}" _installed_signature)
    else()
      set(_installed_signature "")
    endif()

    if(NOT _installed_signature STREQUAL _conan_signature)
      message(STATUS "Conan: installing AnyKeep dependencies for ${_build_type}")
      execute_process(
        COMMAND "${ANYKEEP_CONAN_COMMAND}" install "${_conan_output_dir}/conanfile.txt"
                "--output-folder=${_conan_output_dir}" ${_conan_args} RESULT_VARIABLE _conan_install_result
                                                                                      COMMAND_ECHO STDOUT)
      if(NOT _conan_install_result EQUAL 0)
        message(FATAL_ERROR "Conan install failed for ${_build_type} with exit code " "${_conan_install_result}")
      endif()
      file(WRITE "${_conan_stamp}" "${_conan_signature}")
    else()
      message(STATUS "Conan: dependencies for ${_build_type} are up to date")
    endif()
  endforeach()

  list(PREPEND CMAKE_PREFIX_PATH "${_conan_output_dir}")
  list(PREPEND CMAKE_MODULE_PATH "${_conan_output_dir}")
  list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
  list(REMOVE_DUPLICATES CMAKE_MODULE_PATH)
  set(CMAKE_PREFIX_PATH
      "${CMAKE_PREFIX_PATH}"
      PARENT_SCOPE)
  set(CMAKE_MODULE_PATH
      "${CMAKE_MODULE_PATH}"
      PARENT_SCOPE)
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/conanfile.txt")
endfunction()
