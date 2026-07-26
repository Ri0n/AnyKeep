set(QTNOTE_DEFAULT_VERSION 1.0.0)

macro(sanitize_version VERSION PREFIX OUT_VAR)
    # try to sanitize version to fit cmake standard
    string(STRIP "${VERSION}" VERSION)
    if(${VERSION} MATCHES "^v?([0-9]+)\\.([0-9]+)\\.?([0-9]+)?(\\-([0-9]+)\\-g([0-9a-f]+))?$")
        set(${PREFIX}_VERSION_MAJOR ${CMAKE_MATCH_1})
        set(${PREFIX}_VERSION_MINOR ${CMAKE_MATCH_2})
        set(${PREFIX}_VERSION_PATCH ${CMAKE_MATCH_3})
        set(${PREFIX}_VERSION_TWEAK ${CMAKE_MATCH_5})
        set(${PREFIX}_VERSION_HASH ${CMAKE_MATCH_6})

        if ("${CMAKE_MATCH_3}" STREQUAL "")
            set(${PREFIX}_VERSION_PATCH 0)
        endif()
        set(${OUT_VAR} ${${PREFIX}_VERSION_MAJOR}.${${PREFIX}_VERSION_MINOR}.${${PREFIX}_VERSION_PATCH})
    else()
        message(WARNING "Failed to sanitize version ${VERSION}. Doesn't look like semver at all.")
        set(${OUT_VAR})
    endif()
endmacro()

if (NOT QTNOTE_VERSION_MAJOR)
    if (EXISTS "${CMAKE_SOURCE_DIR}/.git")
        find_package(Git)
        if (Git_FOUND)
            # should give x.y.z but we will sanitize anyway
            execute_process(COMMAND ${GIT_EXECUTABLE} -C "${CMAKE_SOURCE_DIR}" describe --tags --always
                            OUTPUT_VARIABLE GIT_REPO_FULL_VERSION)
            message(STATUS "Version taken from git ${GIT_REPO_FULL_VERSION}")
            string(STRIP "${GIT_REPO_FULL_VERSION}" GIT_REPO_FULL_VERSION)
            sanitize_version(${GIT_REPO_FULL_VERSION} QTNOTE GIT_REPO_VERSION)
            if (QTNOTE_VERSION_HASH)
                message(STATUS "commit ${QTNOTE_VERSION_HASH}")
                message(STATUS "commits from last tag ${QTNOTE_VERSION_TWEAK}")

                math(EXPR QTNOTE_VERSION_PATCH "${QTNOTE_VERSION_PATCH} + ${QTNOTE_VERSION_TWEAK}" OUTPUT_FORMAT DECIMAL)
                set(GIT_REPO_VERSION ${QTNOTE_VERSION_MAJOR}.${QTNOTE_VERSION_MINOR}.${QTNOTE_VERSION_PATCH})
                set(QTNOTE_VERSION_TWEAK 0)
                set(QTNOTE_VERSION ${GIT_REPO_VERSION})
            else()
                set(QTNOTE_VERSION ${GIT_REPO_FULL_VERSION})
            endif()
        endif()
    else()
        if (EXISTS "${CMAKE_SOURCE_DIR}/qtnote.version")
            file(READ "${CMAKE_SOURCE_DIR}/qtnote.version" QTNOTE_VERSION_FILE)
            sanitize_version(${QTNOTE_VERSION_FILE} QTNOTE QTNOTE_VERSION_FILE)
            set(QTNOTE_VERSION ${QTNOTE_VERSION_FILE})
        endif()
    endif()
    set(VERSION_FOR_WIX  ${QTNOTE_VERSION}.0)
    message(STATUS "Version for WiX ${VERSION_FOR_WIX}")
endif()

if ("${QTNOTE_VERSION}" STREQUAL "")
    sanitize_version(${QTNOTE_DEFAULT_VERSION} QTNOTE QTNOTE_VERSION)
    if ("${QTNOTE_VERSION}" STREQUAL "")
        message(FATAL_ERROR "Invalid default version ${QTNOTE_DEFAULT_VERSION}")
    endif()
    message(WARNING "Failed to find QtNote version. Using ${QTNOTE_VERSION}")
    #set(QTNOTE_VERSION "${QTNOTE_VERSION}" CACHE STRING "QtNote version string")
endif()

function(qtnote_platform_has_plugin out_var platforms)
    if(ANDROID AND android IN_LIST platforms)
        set(${out_var} ON PARENT_SCOPE)
    elseif(APPLE AND macosx IN_LIST platforms)
        set(${out_var} ON PARENT_SCOPE)
    elseif(UNIXLIKE AND NOT ANDROID AND unix IN_LIST platforms)
        set(${out_var} ON PARENT_SCOPE)
    elseif(WIN32 AND windows IN_LIST platforms)
        set(${out_var} ON PARENT_SCOPE)
    else()
        set(${out_var} OFF PARENT_SCOPE)
    endif()
endfunction()

macro(add_qtnote_plugin name description buildable)
    set(multiValueArgs
        SOURCES)
    cmake_parse_arguments(arg "" "" "${multiValueArgs}" ${ARGN})

    cmake_minimum_required(VERSION 3.14.0)
    project(qtnote_plugin_${name} VERSION ${QTNOTE_VERSION} LANGUAGES CXX)
    qtnote_platform_has_plugin(_def_plugin_enabled "${arg_UNPARSED_ARGUMENTS}")
    if(${buildable})
        if(${_def_plugin_enabled})
            message(STATUS "PLUGIN ${name} is available on this platform and buildable")
        else()
            message(STATUS "PLUGIN ${name} is disabled on this platform")
        endif()
    else()
        message(STATUS "PLUGIN ${name} is not buildable")
        set(_def_plugin_enabled OFF)
    endif()
    option(QTNOTE_PLUGIN_ENABLE_${name} "Enable QtNote plugin: ${description}" ${_def_plugin_enabled})

    if (NOT QTNOTE_PLUGIN_ENABLE_${name})
        add_custom_target(${name} SOURCES ${arg_SOURCES})
        return()
    endif()

    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    set(QTNOTE_COMMON_PLUGIN_SRC
        ${plugins_SOURCE_DIR}/deintegrationinterface.h
        ${plugins_SOURCE_DIR}/qtnoteplugininterface.h
        ${plugins_SOURCE_DIR}/trayimpl.h
        )
    include_directories(${CMAKE_BINARY_DIR} ${plugins_SOURCE_DIR})
    set(EXTRA_LINK_TARGET ${qtnote_lib})
    if(WIN32)
        set(LIB_TYPE "MODULE")
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}/plugins)
    else()
        set(LIB_TYPE "SHARED")
    endif()
    add_library(${name} ${LIB_TYPE}
        ${QTNOTE_COMMON_PLUGIN_SRC}
        ${arg_SOURCES}
        )
endmacro()

function(add_qtnote_bundled_plugin name)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(arg "" "" "${multiValueArgs}" ${ARGN})

    set(target "qtnote_bundled_${name}")
    add_library(${target} STATIC ${arg_SOURCES})
    set_target_properties(${target} PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        AUTOUIC ON
        POSITION_INDEPENDENT_CODE ON
    )
    target_compile_definitions(${target} PRIVATE QTNOTE_BUNDLED_PLUGIN_BUILD)
    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/libqtnote"
        "${CMAKE_BINARY_DIR}/libqtnote"
        "${CMAKE_BINARY_DIR}"
        "${plugins_SOURCE_DIR}"
    )
endfunction()

macro(qtnote_optional_pkgconfig)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(${ARGN})
    endif()
endmacro()

macro(windeployqt name)
    if(WINDEPLOYQT_EXECUTABLE)
        install(CODE "
            execute_process(
                COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                    --no-compiler-runtime
                    --no-system-dxc-compiler
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    --dir ${CMAKE_INSTALL_PREFIX}
                    $<TARGET_FILE:${name}>
            )
        ")
    endif()
endmacro()

macro(install_qtnote_plugin name)
    install(TARGETS ${name} LIBRARY DESTINATION ${PLUGINSDIR} COMPONENT Libraries NAMELINK_COMPONENT Development)
    windeployqt(${name})
endmacro()
