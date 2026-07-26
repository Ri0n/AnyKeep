include(ExternalProject)
include(ProcessorCount)

if(NOT QT_VERSION_MAJOR EQUAL 6)
    message(FATAL_ERROR "Bundled QXmpp is supported only with Qt 6")
endif()
if(NOT UNIX)
    message(FATAL_ERROR "Bundled QXmpp currently supports Unix-like targets only")
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Xml)
if(ANDROID)
    include(AndroidOpenSSL)
else()
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
endif()
if(ANDROID)
    # Never query the host pkg-config database for an Android dependency. A
    # host libomemo-c target contributes a raw -lomemo-c flag and headers for
    # the wrong platform, while also preventing the bundled ExternalProject
    # from being built.
    if(NOT QTNOTE_BUILD_BUNDLED_OMEMO_C)
        message(FATAL_ERROR
            "Android bundled QXmpp requires bundled libomemo-c. Enable "
            "QTNOTE_BUILD_BUNDLED_OMEMO_C.")
    endif()
    message(STATUS
        "Android cross-build: using bundled libomemo-c and protobuf-c runtime")
    include(BundledOmemoC)
else()
    include(FindPkgConfig)
    pkg_check_modules(OmemoC QUIET IMPORTED_TARGET libomemo-c)
    if(TARGET PkgConfig::OmemoC)
        message(STATUS "Using system libomemo-c for bundled QXmpp")
    elseif(QTNOTE_BUILD_BUNDLED_OMEMO_C)
        message(STATUS
            "System libomemo-c not found; using bundled libomemo-c and protobuf-c runtime")
        include(BundledOmemoC)
    else()
        message(FATAL_ERROR
            "Bundled QXmpp with OMEMO requires libomemo-c. Install libomemo-c or "
            "enable QTNOTE_BUILD_BUNDLED_OMEMO_C.")
    endif()
endif()

set(QTNOTE_BUNDLED_QXMPP_VERSION "1.15.1")
set(QTNOTE_QXMPP_SOURCE_DIR "" CACHE PATH "Local QXmpp source directory (avoids downloading the release tarball)")
option(QTNOTE_BUNDLED_QXMPP_STATIC "Link bundled QXmpp statically into its consumers" ON)
ProcessorCount(_qxmpp_detected_jobs)
if(NOT _qxmpp_detected_jobs)
    set(_qxmpp_detected_jobs 2)
endif()
set(QTNOTE_BUNDLED_QXMPP_JOBS "${_qxmpp_detected_jobs}" CACHE STRING "Parallel jobs used to build bundled QXmpp")

set(_qxmpp_prefix "${CMAKE_BINARY_DIR}/_deps/qxmpp")
set(_qxmpp_install_dir "${_qxmpp_prefix}/install")
set(_qxmpp_library_dir "${_qxmpp_install_dir}/${CMAKE_INSTALL_LIBDIR}")
set(_qxmpp_include_dir "${_qxmpp_install_dir}/${CMAKE_INSTALL_INCLUDEDIR}/QXmppQt6")
set(_qxmpp_omemo_include_dir "${_qxmpp_include_dir}/Omemo")

if(QTNOTE_BUNDLED_QXMPP_STATIC)
    set(_qxmpp_library_type STATIC)
    set(_qxmpp_library_suffix "${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(_qxmpp_build_shared OFF)
else()
    set(_qxmpp_library_type SHARED)
    set(_qxmpp_library_suffix "${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(_qxmpp_build_shared ON)
endif()
set(_qxmpp_library "${_qxmpp_library_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}QXmppQt6${_qxmpp_library_suffix}")
set(_qxmpp_omemo_library "${_qxmpp_library_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}QXmppOmemoQt6${_qxmpp_library_suffix}")

if(QTNOTE_QXMPP_SOURCE_DIR)
    if(NOT EXISTS "${QTNOTE_QXMPP_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "QTNOTE_QXMPP_SOURCE_DIR does not contain a QXmpp source tree: ${QTNOTE_QXMPP_SOURCE_DIR}")
    endif()
    set(_qxmpp_source_args
        SOURCE_DIR "${QTNOTE_QXMPP_SOURCE_DIR}"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
    )
else()
    set(_qxmpp_source_args
        URL "https://download.kde.org/unstable/qxmpp/qxmpp-${QTNOTE_BUNDLED_QXMPP_VERSION}.tar.xz"
        URL_HASH "SHA256=0747758a4f5b5ea4c60686c65b390766f1909d09e1a5a457c8e80ef272730c46"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()

set(_qxmpp_prefix_path "${CMAKE_PREFIX_PATH}")
if(DEFINED QTNOTE_OMEMO_C_INSTALL_DIR)
    list(PREPEND _qxmpp_prefix_path "${QTNOTE_OMEMO_C_INSTALL_DIR}")
endif()
string(REPLACE ";" "|" _qxmpp_prefix_path_arg "${_qxmpp_prefix_path}")

set(_qxmpp_cmake_args
    "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
    "-DCMAKE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
    "-DCMAKE_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_PREFIX_PATH=${_qxmpp_prefix_path_arg}"
    "-DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=ON"
    "-DBUILD_SHARED=${_qxmpp_build_shared}"
    "-DBUILD_TESTING=OFF"
    "-DBUILD_DOCUMENTATION=OFF"
    "-DBUILD_DOCBOOK=OFF"
    "-DBUILD_EXAMPLES=OFF"
    "-DBUILD_OMEMO=ON"
    "-DWITH_GSTREAMER=OFF"
    "-DWITH_ENCRYPTION=ON"
)
foreach(_openssl_var IN ITEMS
    OPENSSL_ROOT_DIR
    OPENSSL_INCLUDE_DIR
    OPENSSL_SSL_LIBRARY
    OPENSSL_CRYPTO_LIBRARY
    OPENSSL_USE_STATIC_LIBS
)
    if(DEFINED ${_openssl_var} AND NOT "${${_openssl_var}}" STREQUAL "")
        list(APPEND _qxmpp_cmake_args "-D${_openssl_var}=${${_openssl_var}}")
    endif()
endforeach()
set(_qxmpp_cxx_compiler "${CMAKE_CXX_COMPILER}")
set(_qxmpp_cxx_flags "${CMAKE_CXX_FLAGS}")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    # GCC 13 has multiple frontend ICEs in QXmpp 1.15.1 coroutines, including
    # the OMEMO implementation, even at -O0. Build only this ExternalProject
    # with Clang; both compilers use the same libstdc++ ABI on Debian/Ubuntu.
    find_program(_qxmpp_clangxx NAMES clang++-18 clang++-17 clang++ REQUIRED)
    set(_qxmpp_cxx_compiler "${_qxmpp_clangxx}")
    # dpkg-buildflags enables GCC LTO flags that must not be passed to Clang.
    string(REGEX REPLACE "(^| )-flto(=[^ ]+)?( |$)" " " _qxmpp_cxx_flags "${_qxmpp_cxx_flags}")
    string(REGEX REPLACE "(^| )-ffat-lto-objects( |$)" " " _qxmpp_cxx_flags "${_qxmpp_cxx_flags}")
    message(STATUS "Building bundled QXmpp with ${_qxmpp_clangxx} to avoid GCC ${CMAKE_CXX_COMPILER_VERSION} coroutine compiler errors")
endif()
if(_qxmpp_cxx_compiler)
    list(APPEND _qxmpp_cmake_args "-DCMAKE_CXX_COMPILER=${_qxmpp_cxx_compiler}")
endif()
if(CMAKE_CXX_COMPILER_LAUNCHER)
    list(APPEND _qxmpp_cmake_args "-DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}")
endif()
foreach(_var IN ITEMS
    CMAKE_TOOLCHAIN_FILE
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
)
    if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
        string(REPLACE ";" "|" _value "${${_var}}")
        list(APPEND _qxmpp_cmake_args "-D${_var}=${_value}")
    endif()
endforeach()
set(_qxmpp_extra_cxx_flags "")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # QXmpp 1.15.1 itself still copies its deprecated QXmppPromise type in a
    # few places. Keep that third-party warning out of QtNote build logs.
    string(APPEND _qxmpp_extra_cxx_flags " -Wno-deprecated-declarations")
endif()
if(_qxmpp_extra_cxx_flags)
    list(APPEND _qxmpp_cmake_args "-DCMAKE_CXX_FLAGS=${_qxmpp_cxx_flags}${_qxmpp_extra_cxx_flags}")
endif()

set(_qxmpp_external_dependency_args)
if(TARGET qtnote_bundled_omemoc)
    list(APPEND _qxmpp_external_dependency_args DEPENDS qtnote_bundled_omemoc)
endif()

ExternalProject_Add(qtnote_bundled_qxmpp
    ${_qxmpp_source_args}
    PREFIX "${_qxmpp_prefix}"
    LIST_SEPARATOR "|"
    PATCH_COMMAND
        "${CMAKE_COMMAND}"
        "-DQXMPP_SOURCE_DIR=<SOURCE_DIR>"
        -P "${CMAKE_CURRENT_LIST_DIR}/../patches/PatchQXmppQt64.cmake"
    INSTALL_DIR "${_qxmpp_install_dir}"
    CMAKE_ARGS ${_qxmpp_cmake_args}
    BUILD_COMMAND
        "${CMAKE_COMMAND}" --build <BINARY_DIR>
        --parallel "${QTNOTE_BUNDLED_QXMPP_JOBS}"
    BUILD_BYPRODUCTS
        "${_qxmpp_library}"
        "${_qxmpp_omemo_library}"
    ${_qxmpp_external_dependency_args}
)

# Imported targets let the rest of QtNote use the same target names as a
# system QXmpp package. The directories must exist when CMake validates the
# imported target interfaces; ExternalProject fills them during the build.
file(MAKE_DIRECTORY "${_qxmpp_include_dir}" "${_qxmpp_omemo_include_dir}")

add_library(QXmpp::QXmpp ${_qxmpp_library_type} IMPORTED GLOBAL)
set_target_properties(QXmpp::QXmpp PROPERTIES
    IMPORTED_LOCATION "${_qxmpp_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${_qxmpp_include_dir}"
    INTERFACE_LINK_LIBRARIES "Qt6::Core;Qt6::Network;Qt6::Xml;OpenSSL::Crypto"
)
add_dependencies(QXmpp::QXmpp qtnote_bundled_qxmpp)

add_library(QXmpp::Omemo ${_qxmpp_library_type} IMPORTED GLOBAL)
set_target_properties(QXmpp::Omemo PROPERTIES
    IMPORTED_LOCATION "${_qxmpp_omemo_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${_qxmpp_omemo_include_dir}"
    INTERFACE_LINK_LIBRARIES "QXmpp::QXmpp;PkgConfig::OmemoC;OpenSSL::Crypto"
)
add_dependencies(QXmpp::Omemo qtnote_bundled_qxmpp)

if(NOT QTNOTE_BUNDLED_QXMPP_STATIC)
    install(
        DIRECTORY "${_qxmpp_library_dir}/"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        COMPONENT Libraries
        FILES_MATCHING
            PATTERN "${CMAKE_SHARED_LIBRARY_PREFIX}QXmppQt6${CMAKE_SHARED_LIBRARY_SUFFIX}*"
            PATTERN "${CMAKE_SHARED_LIBRARY_PREFIX}QXmppOmemoQt6${CMAKE_SHARED_LIBRARY_SUFFIX}*"
    )
endif()
