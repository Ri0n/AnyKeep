include_guard(GLOBAL)

get_filename_component(_anykeep_dependency_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_anykeep_dependency_lock_file "${_anykeep_dependency_root}/dependencies.lock.json")
if(NOT EXISTS "${_anykeep_dependency_lock_file}")
  message(FATAL_ERROR "AnyKeep dependency lock is missing: ${_anykeep_dependency_lock_file}")
endif()

# A lock-file edit must re-run CMake even when no CMake source file changed.
# This also gives the cleanup below a chance to invalidate the dependency's
# download, build and install trees before ExternalProject/FetchContent reads
# their old stamps.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_anykeep_dependency_lock_file}")

file(READ "${_anykeep_dependency_lock_file}" _anykeep_dependency_lock)

string(JSON ANYKEEP_DEP_IRIS_TAG GET "${_anykeep_dependency_lock}" iris tag)
string(JSON ANYKEEP_DEP_IRIS_COMMIT GET "${_anykeep_dependency_lock}" iris commit)
string(JSON ANYKEEP_DEP_QCA_TAG GET "${_anykeep_dependency_lock}" qca tag)
string(JSON ANYKEEP_DEP_QCA_COMMIT GET "${_anykeep_dependency_lock}" qca commit)
string(JSON ANYKEEP_DEP_QTKEYCHAIN_TAG GET "${_anykeep_dependency_lock}" qtkeychain tag)
string(JSON ANYKEEP_DEP_QTKEYCHAIN_VERSION GET "${_anykeep_dependency_lock}" qtkeychain version)
string(JSON ANYKEEP_DEP_QTKEYCHAIN_REVISION GET "${_anykeep_dependency_lock}" qtkeychain revision)

foreach(_anykeep_commit IN ITEMS ANYKEEP_DEP_IRIS_COMMIT ANYKEEP_DEP_QCA_COMMIT)
  string(LENGTH "${${_anykeep_commit}}" _anykeep_commit_length)
  if(NOT _anykeep_commit_length EQUAL 40 OR NOT "${${_anykeep_commit}}" MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "${_anykeep_commit} must be a 40-character lowercase Git commit SHA")
  endif()
endforeach()

# ExternalProject and FetchContent deliberately retain their state in the
# build tree.  In particular, UPDATE_DISCONNECTED prevents a new Git tag from
# being checked out after a normal reconfigure.  Keep a small, per-dependency
# fingerprint in CMakeCache.txt and discard only the affected private _deps
# trees when its lock entry changes.  An absent fingerprint means that this
# build tree predates lock tracking, so its existing dependency cannot be
# trusted and is rebuilt once.
function(_anykeep_reset_locked_dependency dependency fingerprint)
  set(options)
  set(one_value_args)
  set(multi_value_args PATHS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if(NOT ARG_PATHS)
    message(FATAL_ERROR "No build directories were provided for locked dependency ${dependency}")
  endif()

  string(TOUPPER "${dependency}" _anykeep_dependency_cache_name)
  string(REGEX REPLACE "[^A-Z0-9_]" "_" _anykeep_dependency_cache_name "${_anykeep_dependency_cache_name}")
  set(_anykeep_dependency_cache_name "ANYKEEP_LOCK_${_anykeep_dependency_cache_name}_FINGERPRINT")

  set(_anykeep_dependency_reset FALSE)
  if(NOT DEFINED CACHE{${_anykeep_dependency_cache_name}})
    set(_anykeep_dependency_reset TRUE)
    set(_anykeep_dependency_reset_reason "no previous lock fingerprint")
  elseif(NOT "${${_anykeep_dependency_cache_name}}" STREQUAL "${fingerprint}")
    set(_anykeep_dependency_reset TRUE)
    set(_anykeep_dependency_reset_reason "lock entry changed")
  endif()

  if(_anykeep_dependency_reset)
    foreach(_anykeep_dependency_path IN LISTS ARG_PATHS)
      if(EXISTS "${_anykeep_dependency_path}")
        message(STATUS "${dependency}: clearing stale bundled dependency state (${_anykeep_dependency_reset_reason}): ${_anykeep_dependency_path}")
        file(REMOVE_RECURSE "${_anykeep_dependency_path}")
      endif()
    endforeach()
  endif()

  set(${_anykeep_dependency_cache_name}
      "${fingerprint}"
      CACHE INTERNAL "Fingerprint of the ${dependency} entry in dependencies.lock.json" FORCE)
endfunction()

string(SHA256 _anykeep_qca_lock_fingerprint "${ANYKEEP_DEP_QCA_TAG}\n${ANYKEEP_DEP_QCA_COMMIT}")
_anykeep_reset_locked_dependency(
  qca
  "${_anykeep_qca_lock_fingerprint}"
  PATHS "${CMAKE_BINARY_DIR}/_deps/qca" "${CMAKE_BINARY_DIR}/_deps/qca-android-plugins")

string(SHA256 _anykeep_iris_lock_fingerprint "${ANYKEEP_DEP_IRIS_TAG}\n${ANYKEEP_DEP_IRIS_COMMIT}")
_anykeep_reset_locked_dependency(
  iris
  "${_anykeep_iris_lock_fingerprint}"
  PATHS "${CMAKE_BINARY_DIR}/_deps/anykeep_iris-src" "${CMAKE_BINARY_DIR}/_deps/anykeep_iris-build"
        "${CMAKE_BINARY_DIR}/_deps/anykeep_iris-subbuild")

string(SHA256 _anykeep_qtkeychain_lock_fingerprint
       "${ANYKEEP_DEP_QTKEYCHAIN_TAG}\n${ANYKEEP_DEP_QTKEYCHAIN_VERSION}\n${ANYKEEP_DEP_QTKEYCHAIN_REVISION}")
_anykeep_reset_locked_dependency(
  qtkeychain
  "${_anykeep_qtkeychain_lock_fingerprint}"
  PATHS "${CMAKE_BINARY_DIR}/_deps/qtkeychain")

unset(_anykeep_commit)
unset(_anykeep_commit_length)
unset(_anykeep_dependency_lock)
unset(_anykeep_dependency_lock_file)
unset(_anykeep_dependency_root)
unset(_anykeep_qca_lock_fingerprint)
unset(_anykeep_iris_lock_fingerprint)
unset(_anykeep_qtkeychain_lock_fingerprint)
