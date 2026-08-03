include(ExternalProject)
include(GNUInstallDirs)
include(ProcessorCount)

set(ANYKEEP_QSOURCEHIGHLITE_GIT_REPOSITORY
    "https://github.com/Waqar144/QSourceHighlite.git"
    CACHE STRING "QSourceHighlite repository")
set(ANYKEEP_QSOURCEHIGHLITE_GIT_TAG
    "d78f0d4171580a18646b15425c449db3f2bb240a"
    CACHE STRING "Pinned QSourceHighlite revision")
set(ANYKEEP_QSOURCEHIGHLITE_SOURCE_DIR "" CACHE PATH
    "Local QSourceHighlite source directory (avoids cloning the repository)")

ProcessorCount(_qsourcehighlite_detected_jobs)
if(NOT _qsourcehighlite_detected_jobs)
    set(_qsourcehighlite_detected_jobs 2)
endif()
set(ANYKEEP_QSOURCEHIGHLITE_JOBS "${_qsourcehighlite_detected_jobs}" CACHE STRING
    "Parallel jobs used to build bundled QSourceHighlite")

set(_qsourcehighlite_prefix "${CMAKE_BINARY_DIR}/_deps/qsourcehighlite")
set(_qsourcehighlite_install_dir "${_qsourcehighlite_prefix}/install")
# This is a private staging prefix, not an installation into the host system.
# Keep its layout deterministic instead of inheriting Debian multiarch values
# such as lib/x86_64-linux-gnu from the parent project.
set(_qsourcehighlite_install_includedir "include")
set(_qsourcehighlite_install_libdir "lib")
set(_qsourcehighlite_install_bindir "bin")
set(_qsourcehighlite_install_datadir "share")
set(_qsourcehighlite_include_dir
    "${_qsourcehighlite_install_dir}/${_qsourcehighlite_install_includedir}/QSourceHighlite")
set(_qsourcehighlite_library_dir
    "${_qsourcehighlite_install_dir}/${_qsourcehighlite_install_libdir}")
set(_qsourcehighlite_library
    "${_qsourcehighlite_library_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}QSourceHighlite${CMAKE_STATIC_LIBRARY_SUFFIX}")

set(_qsourcehighlite_generator_args -G "${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND _qsourcehighlite_generator_args -A "${CMAKE_GENERATOR_PLATFORM}")
endif()
if(CMAKE_GENERATOR_TOOLSET)
    list(APPEND _qsourcehighlite_generator_args -T "${CMAKE_GENERATOR_TOOLSET}")
endif()

if(ANYKEEP_QSOURCEHIGHLITE_SOURCE_DIR)
    if(NOT EXISTS "${ANYKEEP_QSOURCEHIGHLITE_SOURCE_DIR}/qsourcehighliter.cpp")
        message(FATAL_ERROR
            "ANYKEEP_QSOURCEHIGHLITE_SOURCE_DIR does not contain a QSourceHighlite source tree: "
            "${ANYKEEP_QSOURCEHIGHLITE_SOURCE_DIR}")
    endif()
    set(_qsourcehighlite_source_args
        SOURCE_DIR "${ANYKEEP_QSOURCEHIGHLITE_SOURCE_DIR}"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
    )
else()
    set(_qsourcehighlite_source_args
        GIT_REPOSITORY "${ANYKEEP_QSOURCEHIGHLITE_GIT_REPOSITORY}"
        GIT_TAG "${ANYKEEP_QSOURCEHIGHLITE_GIT_TAG}"
        GIT_SHALLOW FALSE
        UPDATE_COMMAND ""
    )
endif()

ExternalProject_Add(anykeep_bundled_qsourcehighlite
    ${_qsourcehighlite_source_args}
    # The dependency is pinned. Avoid re-running its update/configure/build/
    # install chain, and printing that chain, on every focused AnyKeep build.
    UPDATE_DISCONNECTED TRUE
    PREFIX "${_qsourcehighlite_prefix}"
    BINARY_DIR "${_qsourcehighlite_prefix}/build"
    INSTALL_DIR "${_qsourcehighlite_install_dir}"
    CONFIGURE_COMMAND
        "${CMAKE_COMMAND}"
        ${_qsourcehighlite_generator_args}
        -S "${CMAKE_SOURCE_DIR}/cmake/thirdparty/qsourcehighlite"
        -B <BINARY_DIR>
        "-DQSOURCEHIGHLITE_SOURCE_DIR=<SOURCE_DIR>"
        "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
        "-DCMAKE_INSTALL_LIBDIR=${_qsourcehighlite_install_libdir}"
        "-DCMAKE_INSTALL_INCLUDEDIR=${_qsourcehighlite_install_includedir}"
        "-DCMAKE_INSTALL_BINDIR=${_qsourcehighlite_install_bindir}"
        "-DCMAKE_INSTALL_DATADIR=${_qsourcehighlite_install_datadir}"
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DQt6_DIR=${Qt6_DIR}"
        "-DQt6Core_DIR=${Qt6Core_DIR}"
        "-DQt6Gui_DIR=${Qt6Gui_DIR}"
        "-DQT_HOST_PATH=${QT_HOST_PATH}"
        "-DQT_HOST_PATH_CMAKE_DIR=${QT_HOST_PATH_CMAKE_DIR}"
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DANDROID_ABI=${ANDROID_ABI}"
        "-DCMAKE_ANDROID_ARCH_ABI=${ANDROID_ABI}"
        "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
        "-DANDROID_NDK=${ANDROID_NDK}"
    BUILD_COMMAND
        "${CMAKE_COMMAND}" --build <BINARY_DIR>
        --parallel "${ANYKEEP_QSOURCEHIGHLITE_JOBS}"
    INSTALL_COMMAND
        "${CMAKE_COMMAND}" --install <BINARY_DIR>
    BUILD_BYPRODUCTS "${_qsourcehighlite_library}"
)

file(MAKE_DIRECTORY "${_qsourcehighlite_include_dir}")

add_library(QSourceHighlite::QSourceHighlite STATIC IMPORTED GLOBAL)
set_target_properties(QSourceHighlite::QSourceHighlite PROPERTIES
    IMPORTED_LOCATION "${_qsourcehighlite_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${_qsourcehighlite_include_dir}"
    INTERFACE_LINK_LIBRARIES "Qt6::Core;Qt6::Gui"
)
add_dependencies(QSourceHighlite::QSourceHighlite anykeep_bundled_qsourcehighlite)
