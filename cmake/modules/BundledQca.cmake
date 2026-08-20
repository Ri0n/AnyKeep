include(ExternalProject)
include(GNUInstallDirs)
include(ProcessorCount)

set(ANYKEEP_BUNDLED_QCA_GIT_REPOSITORY
    "https://github.com/psi-im/qca.git"
    CACHE STRING "Bundled QCA git repository")
set(ANYKEEP_BUNDLED_QCA_GIT_TAG
    "master"
    CACHE STRING "Bundled QCA git tag or branch")
set(ANYKEEP_QCA_SOURCE_DIR
    ""
    CACHE PATH "Local QCA source directory (avoids cloning the repository)")

processorcount(_qca_detected_jobs)
if(NOT _qca_detected_jobs)
  set(_qca_detected_jobs 2)
endif()
set(ANYKEEP_BUNDLED_QCA_JOBS
    "${_qca_detected_jobs}"
    CACHE STRING "Parallel jobs used to build bundled QCA")

# QCA is built exactly once by AnyKeep. The QXmpp backend may use a static
# QCA3, while the Iris backend requires one shared QCA3 instance so libanykeep
# and Iris never carry independent QCA runtimes.
set(_qca_build_shared OFF)
if(ANYKEEP_XMPP_BACKEND STREQUAL "IRIS")
  set(_qca_build_shared ON)
endif()
set(ANYKEEP_BUNDLED_QCA_SHARED ${_qca_build_shared})

if(Qt6Core_DIR AND NOT Qt6Test_DIR)
  get_filename_component(_anykeep_qt6_cmake_root "${Qt6Core_DIR}/.." ABSOLUTE)
  if(EXISTS "${_anykeep_qt6_cmake_root}/Qt6Test/Qt6TestConfig.cmake")
    set(Qt6Test_DIR
        "${_anykeep_qt6_cmake_root}/Qt6Test"
        CACHE PATH "Qt6Test package directory" FORCE)
  endif()
endif()

if(ANDROID)
  include(AndroidOpenSSL)
else()
  if(NOT TARGET OpenSSL::Crypto OR NOT TARGET OpenSSL::SSL)
    find_package(OpenSSL QUIET CONFIG)
  endif()
  if(NOT TARGET OpenSSL::Crypto OR NOT TARGET OpenSSL::SSL)
    find_package(OpenSSL REQUIRED)
  endif()
endif()

set(_qca_prefix "${CMAKE_BINARY_DIR}/_deps/qca")
set(_qca_install_dir "${_qca_prefix}/install")
set(ANYKEEP_QCA_INSTALL_DIR "${_qca_install_dir}")
set(ANYKEEP_QCA_IS_BUNDLED TRUE)
set(_qca_include_dir "${_qca_install_dir}/${CMAKE_INSTALL_INCLUDEDIR}/Qca3-qt6/QtCrypto")
set(_qca_library_dir "${_qca_install_dir}/${CMAKE_INSTALL_LIBDIR}")
set(_qca_runtime_dir "${_qca_install_dir}/${CMAKE_INSTALL_BINDIR}")
set(_qca_debug_postfix "")
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_qca_debug_postfix "d")
endif()

if(_qca_build_shared)
  if(WIN32)
    set(_qca_runtime
        "${_qca_runtime_dir}/qca3-qt6${_qca_debug_postfix}${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(_qca_library
        "${_qca_library_dir}/${CMAKE_IMPORT_LIBRARY_PREFIX}qca3-qt6${_qca_debug_postfix}${CMAKE_IMPORT_LIBRARY_SUFFIX}")
    set(_qca_ossl_plugin
        "${_qca_library_dir}/qca3-qt6/crypto/qca-ossl${_qca_debug_postfix}${CMAKE_SHARED_MODULE_SUFFIX}")
  else()
    set(_qca_library
        "${_qca_library_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}qca3-qt6${CMAKE_SHARED_LIBRARY_SUFFIX}")
    if(APPLE)
      set(_qca_ossl_suffix ".dylib")
    else()
      set(_qca_ossl_suffix "${CMAKE_SHARED_MODULE_SUFFIX}")
    endif()
    set(_qca_ossl_plugin
        "${_qca_library_dir}/qca3-qt6/crypto/${CMAKE_SHARED_MODULE_PREFIX}qca-ossl${_qca_ossl_suffix}")
  endif()
else()
  set(_qca_library
      "${_qca_library_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}qca3-qt6${_qca_debug_postfix}${CMAKE_STATIC_LIBRARY_SUFFIX}")
  string(CONCAT _qca_ossl_plugin "${_qca_library_dir}/qca3-qt6/crypto/${CMAKE_STATIC_LIBRARY_PREFIX}qca-ossl"
                "${_qca_debug_postfix}${CMAKE_STATIC_LIBRARY_SUFFIX}")
endif()

set(ANYKEEP_QCA_OSSL_PLUGIN "${_qca_ossl_plugin}")

if(ANYKEEP_QCA_SOURCE_DIR)
  if(NOT EXISTS "${ANYKEEP_QCA_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "ANYKEEP_QCA_SOURCE_DIR does not contain a QCA source tree: ${ANYKEEP_QCA_SOURCE_DIR}")
  endif()
  set(_qca_source_args SOURCE_DIR "${ANYKEEP_QCA_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND "")
else()
  set(_qca_source_args GIT_REPOSITORY "${ANYKEEP_BUNDLED_QCA_GIT_REPOSITORY}" GIT_TAG "${ANYKEEP_BUNDLED_QCA_GIT_TAG}"
                       UPDATE_COMMAND "")
endif()

string(REPLACE ";" "|" _qca_prefix_path_arg "${CMAKE_PREFIX_PATH}")
set(_qca_cmake_args
    "-DBUILD_SHARED_LIBS=${_qca_build_shared}"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    "-DBUILD_PLUGINS=ossl"
    "-DLOAD_SHARED_PLUGINS=${_qca_build_shared}"
    "-DBUILD_TESTS=OFF"
    "-DBUILD_TOOLS=OFF"
    "-DBUILD_WITH_QT6=ON"
    "-DQCA_SUFFIX=qt6"
    "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
    "-DCMAKE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
    "-DCMAKE_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
    "-DLIB_INSTALL_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_LIBDIR}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_PREFIX_PATH=${_qca_prefix_path_arg}"
    "-DQt6_DIR=${Qt6_DIR}"
    "-DQt6Core_DIR=${Qt6Core_DIR}"
    "-DQt6Test_DIR=${Qt6Test_DIR}"
    "-DQT_HOST_PATH=${QT_HOST_PATH}"
    "-DQT_HOST_PATH_CMAKE_DIR=${QT_HOST_PATH_CMAKE_DIR}"
    "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
    "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DANDROID_ABI=${ANDROID_ABI}"
    "-DCMAKE_ANDROID_ARCH_ABI=${ANDROID_ABI}"
    "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
    "-DANDROID_NDK=${ANDROID_NDK}"
    "-DOSX_FRAMEWORK=OFF")

foreach(_openssl_var IN ITEMS OPENSSL_ROOT_DIR OPENSSL_INCLUDE_DIR OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY
                              OPENSSL_USE_STATIC_LIBS)
  if(DEFINED ${_openssl_var}
     AND NOT "${${_openssl_var}}" STREQUAL ""
     AND NOT TARGET "${${_openssl_var}}")
    list(APPEND _qca_cmake_args "-D${_openssl_var}=${${_openssl_var}}")
  endif()
endforeach()

# Conan's config package represents these variables as target names. QCA's configure performs try_compile() checks using
# CMake's FindOpenSSL module, so give that separate project concrete library files instead.
if(WIN32
   AND OPENSSL_INCLUDE_DIR
   AND (TARGET "${OPENSSL_SSL_LIBRARY}" OR TARGET "${OPENSSL_CRYPTO_LIBRARY}"))
  get_filename_component(_qca_openssl_root "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
  find_library(
    _qca_openssl_ssl_library
    NAMES libssl ssl
    PATHS "${_qca_openssl_root}/lib"
    NO_DEFAULT_PATH REQUIRED)
  find_library(
    _qca_openssl_crypto_library
    NAMES libcrypto crypto
    PATHS "${_qca_openssl_root}/lib" NO_DEFAULT_PATH REQUIRED)
  list(APPEND _qca_cmake_args "-DOPENSSL_ROOT_DIR=${_qca_openssl_root}"
       "-DOPENSSL_SSL_LIBRARY=${_qca_openssl_ssl_library}" "-DOPENSSL_CRYPTO_LIBRARY=${_qca_openssl_crypto_library}")
endif()

anykeep_external_project_config_args(_qca_build_config_args)

ExternalProject_Add(
  anykeep_bundled_qca
  ${_qca_source_args}
  PREFIX "${_qca_prefix}"
  INSTALL_DIR "${_qca_install_dir}"
  LIST_SEPARATOR "|"
  CMAKE_ARGS ${_qca_cmake_args}
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> ${_qca_build_config_args} --parallel
                "${ANYKEEP_BUNDLED_QCA_JOBS}"
  INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> ${_qca_build_config_args}
  BUILD_BYPRODUCTS "${_qca_library}" "${_qca_ossl_plugin}" ${_qca_runtime})

file(MAKE_DIRECTORY "${_qca_include_dir}")

if(_qca_build_shared)
  add_library(Qca3::Qca SHARED IMPORTED GLOBAL)
  if(WIN32)
    set_target_properties(Qca3::Qca PROPERTIES IMPORTED_LOCATION "${_qca_runtime}" IMPORTED_IMPLIB "${_qca_library}")
  else()
    set_target_properties(Qca3::Qca PROPERTIES IMPORTED_LOCATION "${_qca_library}")
  endif()
  set_target_properties(
    Qca3::Qca
    PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_qca_include_dir}" INTERFACE_LINK_LIBRARIES "Qt6::Core")

  # The bundled shared QCA3 is a runtime dependency. qca-ossl remains a
  # dynamically loaded provider. QCA installs it under qca3-qt6/crypto in the
  # private ExternalProject prefix; Windows packaging also copies it to the
  # application's crypto/ directory, which QCA scans at runtime.
  if(WIN32)
    install(FILES "${_qca_runtime}" DESTINATION "." COMPONENT Libraries)
    install(FILES "${_qca_ossl_plugin}" DESTINATION "crypto" COMPONENT Libraries)
  endif()
else()
  add_library(Qca3::Qca STATIC IMPORTED GLOBAL)
  set_target_properties(
    Qca3::Qca
    PROPERTIES IMPORTED_LOCATION "${_qca_library}" INTERFACE_INCLUDE_DIRECTORIES "${_qca_include_dir}"
               INTERFACE_LINK_LIBRARIES "${_qca_ossl_plugin};OpenSSL::SSL;OpenSSL::Crypto;Qt6::Core"
               INTERFACE_COMPILE_DEFINITIONS QCA_STATIC)
  if(APPLE)
    # QCA's static macOS system-store implementation calls Security.framework. Keep this on the imported QCA target so
    # every consumer gets it transitively.
    target_link_libraries(Qca3::Qca INTERFACE "-framework Security")
  endif()
endif()
add_dependencies(Qca3::Qca anykeep_bundled_qca)
