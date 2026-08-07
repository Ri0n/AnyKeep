set(ANYKEEP_DEFAULT_VERSION 1.0.0)

macro(sanitize_version VERSION PREFIX OUT_VAR)
    # try to sanitize version to fit cmake standard
    string(STRIP "${VERSION}" VERSION)
    if("${VERSION}" MATCHES "^v?([0-9]+)\\.([0-9]+)\\.?([0-9]+)?(\\-([0-9]+)\\-g([0-9a-f]+))?$")
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

if (NOT ANYKEEP_VERSION_MAJOR)
    if (EXISTS "${CMAKE_SOURCE_DIR}/.git")
        find_package(Git)
        if (Git_FOUND)
            # should give x.y.z but we will sanitize anyway
            execute_process(COMMAND ${GIT_EXECUTABLE} -C "${CMAKE_SOURCE_DIR}" describe --tags --always
                            RESULT_VARIABLE GIT_REPO_VERSION_RESULT
                            OUTPUT_VARIABLE GIT_REPO_FULL_VERSION
                            ERROR_VARIABLE GIT_REPO_VERSION_ERROR
                            OUTPUT_STRIP_TRAILING_WHITESPACE)
            string(STRIP "${GIT_REPO_FULL_VERSION}" GIT_REPO_FULL_VERSION)
            if (GIT_REPO_VERSION_RESULT EQUAL 0 AND NOT "${GIT_REPO_FULL_VERSION}" STREQUAL "")
                message(STATUS "Version taken from git ${GIT_REPO_FULL_VERSION}")
                sanitize_version("${GIT_REPO_FULL_VERSION}" ANYKEEP GIT_REPO_VERSION)
                if (ANYKEEP_VERSION_HASH)
                    message(STATUS "commit ${ANYKEEP_VERSION_HASH}")
                    message(STATUS "commits from last tag ${ANYKEEP_VERSION_TWEAK}")

                    math(EXPR ANYKEEP_VERSION_PATCH "${ANYKEEP_VERSION_PATCH} + ${ANYKEEP_VERSION_TWEAK}" OUTPUT_FORMAT DECIMAL)
                    set(GIT_REPO_VERSION ${ANYKEEP_VERSION_MAJOR}.${ANYKEEP_VERSION_MINOR}.${ANYKEEP_VERSION_PATCH})
                    set(ANYKEEP_VERSION_TWEAK 0)
                    set(ANYKEEP_VERSION ${GIT_REPO_VERSION})
                else()
                    # Use the sanitized SemVer core. In particular, strip an optional
                    # leading "v" from an exact Git tag before passing it to project().
                    set(ANYKEEP_VERSION ${GIT_REPO_VERSION})
                endif()
            else()
                message(WARNING "Could not determine the version from Git: ${GIT_REPO_VERSION_ERROR}")
            endif()
        endif()
    endif()
    if ("${ANYKEEP_VERSION}" STREQUAL "" AND EXISTS "${CMAKE_SOURCE_DIR}/anykeep.version")
        file(READ "${CMAKE_SOURCE_DIR}/anykeep.version" ANYKEEP_VERSION_FILE)
        sanitize_version("${ANYKEEP_VERSION_FILE}" ANYKEEP ANYKEEP_VERSION_FILE)
        set(ANYKEEP_VERSION ${ANYKEEP_VERSION_FILE})
    endif()
    # dpkg-source keeps this file even when the source tree is not a trusted
    # Git checkout. Its first stanza is the package version.
    if ("${ANYKEEP_VERSION}" STREQUAL "" AND EXISTS "${CMAKE_SOURCE_DIR}/debian/changelog")
        file(STRINGS "${CMAKE_SOURCE_DIR}/debian/changelog" ANYKEEP_DEBIAN_CHANGELOG_LINE LIMIT_COUNT 1)
        string(REGEX MATCH "^[^(]+\\(([0-9]+\\.[0-9]+(\\.[0-9]+)?)" _anykeep_debian_version_match
               "${ANYKEEP_DEBIAN_CHANGELOG_LINE}")
        if (NOT "${CMAKE_MATCH_1}" STREQUAL "")
            sanitize_version("${CMAKE_MATCH_1}" ANYKEEP ANYKEEP_VERSION)
            message(STATUS "Version taken from debian/changelog ${ANYKEEP_VERSION}")
        endif()
    endif()
endif()

if ("${ANYKEEP_VERSION}" STREQUAL "")
    sanitize_version(${ANYKEEP_DEFAULT_VERSION} ANYKEEP ANYKEEP_VERSION)
    if ("${ANYKEEP_VERSION}" STREQUAL "")
        message(FATAL_ERROR "Invalid default version ${ANYKEEP_DEFAULT_VERSION}")
    endif()
    message(WARNING "Failed to find AnyKeep version. Using ${ANYKEEP_VERSION}")
    #set(ANYKEEP_VERSION "${ANYKEEP_VERSION}" CACHE STRING "AnyKeep version string")
endif()

set(VERSION_FOR_WIX ${ANYKEEP_VERSION}.0)
message(STATUS "Version for WiX ${VERSION_FOR_WIX}")

function(anykeep_platform_has_plugin out_var platforms)
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

macro(add_anykeep_plugin name description buildable)
    set(oneValueArgs METADATA ICON)
    set(multiValueArgs
        SOURCES)
    cmake_parse_arguments(arg "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    cmake_minimum_required(VERSION 3.14.0)
    project(anykeep_plugin_${name} VERSION ${ANYKEEP_VERSION} LANGUAGES CXX)
    anykeep_platform_has_plugin(_def_plugin_enabled "${arg_UNPARSED_ARGUMENTS}")
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
    option(ANYKEEP_PLUGIN_ENABLE_${name} "Enable AnyKeep plugin: ${description}" ${_def_plugin_enabled})

    if (NOT ANYKEEP_PLUGIN_ENABLE_${name})
        add_custom_target(${name} SOURCES ${arg_SOURCES})
        return()
    endif()

    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    set(ANYKEEP_COMMON_PLUGIN_SRC
        ${plugins_SOURCE_DIR}/deintegrationinterface.h
        ${plugins_SOURCE_DIR}/anykeepplugininterface.h
        ${plugins_SOURCE_DIR}/trayimpl.h
        )
    include_directories(${CMAKE_BINARY_DIR} ${plugins_SOURCE_DIR})
    set(EXTRA_LINK_TARGET ${anykeep_lib})
    if(WIN32)
        set(LIB_TYPE "MODULE")
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}/plugins)
    else()
        set(LIB_TYPE "SHARED")
    endif()
    if(NOT arg_METADATA OR NOT arg_ICON)
        message(FATAL_ERROR "Plugin ${name} must provide METADATA and ICON")
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(GLOB _plugin_metadata_translations CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/translations/plugin_metadata_*.ts")
    set(_plugin_metadata_output "${CMAKE_CURRENT_BINARY_DIR}/${name}.metadata.json")
    set(_plugin_metadata_include "${CMAKE_CURRENT_BINARY_DIR}/${name}_plugin_metadata.inc")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/cmake/generate_plugin_metadata.py"
            --source "${CMAKE_CURRENT_SOURCE_DIR}/${arg_METADATA}"
            --translations-dir "${CMAKE_SOURCE_DIR}/translations"
            --icon "${CMAKE_CURRENT_SOURCE_DIR}/${arg_ICON}"
            --output "${_plugin_metadata_output}"
            --anykeep-version "${ANYKEEP_VERSION}"
        RESULT_VARIABLE _plugin_metadata_result
        OUTPUT_VARIABLE _plugin_metadata_stdout
        ERROR_VARIABLE _plugin_metadata_stderr
    )
    if(NOT _plugin_metadata_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to generate metadata for ${name}:\n${_plugin_metadata_stdout}${_plugin_metadata_stderr}")
    endif()
    file(WRITE "${_plugin_metadata_include}"
        "Q_PLUGIN_METADATA(IID ANYKEEP_PLUGIN_INTERFACE_IID FILE \"${name}.metadata.json\")\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/cmake/generate_plugin_metadata.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/${arg_METADATA}"
        "${CMAKE_CURRENT_SOURCE_DIR}/${arg_ICON}"
        ${_plugin_metadata_translations}
    )

    add_library(${name} ${LIB_TYPE}
        ${ANYKEEP_COMMON_PLUGIN_SRC}
        ${arg_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/${arg_METADATA}"
        "${CMAKE_CURRENT_SOURCE_DIR}/${arg_ICON}"
        "${_plugin_metadata_output}"
        "${_plugin_metadata_include}"
        )
    set_property(GLOBAL APPEND PROPERTY ANYKEEP_ENABLED_PLUGIN_TARGETS ${name})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endmacro()

function(add_anykeep_bundled_plugin name)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(arg "" "" "${multiValueArgs}" ${ARGN})

    set(target "anykeep_bundled_${name}")
    add_library(${target} STATIC ${arg_SOURCES})
    set_target_properties(${target} PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        AUTOUIC ON
        POSITION_INDEPENDENT_CODE ON
    )
    target_compile_definitions(${target} PRIVATE ANYKEEP_BUNDLED_PLUGIN_BUILD)
    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/libanykeep"
        "${CMAKE_BINARY_DIR}/libanykeep"
        "${CMAKE_BINARY_DIR}"
        "${plugins_SOURCE_DIR}"
    )
endfunction()

macro(anykeep_optional_pkgconfig)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(${ARGN})
    endif()
endmacro()

macro(windeployqt name)
    if(WIN32 AND NOT WINDEPLOYQT_EXECUTABLE)
        set(_anykeep_windeployqt_hints)
        if(TARGET Qt${QT_VERSION_MAJOR}::qmake)
            get_target_property(_anykeep_qmake_location Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
            if(_anykeep_qmake_location)
                get_filename_component(_anykeep_qt_bin_dir "${_anykeep_qmake_location}" DIRECTORY)
                list(APPEND _anykeep_windeployqt_hints "${_anykeep_qt_bin_dir}")
            endif()
        endif()
        find_program(WINDEPLOYQT_EXECUTABLE
            NAMES windeployqt.exe windeployqt
            HINTS ${_anykeep_windeployqt_hints}
        )
        unset(_anykeep_windeployqt_hints)
        unset(_anykeep_qmake_location)
        unset(_anykeep_qt_bin_dir)
    endif()

    if(WINDEPLOYQT_EXECUTABLE)
        # Keep CMAKE_INSTALL_PREFIX as an install-time variable.  The update
        # target installs into an isolated staging prefix with `cmake --install
        # --prefix`; baking the configure-time prefix here would make
        # windeployqt copy Qt into the normal install tree instead.
        install(CODE "
            execute_process(
                COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                    --no-compiler-runtime
                    --no-system-dxc-compiler
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    --dir \"\${CMAKE_INSTALL_PREFIX}\"
                    \"$<TARGET_FILE:${name}>\"
                RESULT_VARIABLE _anykeep_windeployqt_result
            )
            if(NOT _anykeep_windeployqt_result EQUAL 0)
                message(FATAL_ERROR \"windeployqt failed for ${name}: \${_anykeep_windeployqt_result}\")
            endif()
        ")
    endif()
endmacro()

macro(install_anykeep_plugin name)
    install(TARGETS ${name}
        LIBRARY DESTINATION ${PLUGINSDIR}
        RUNTIME DESTINATION ${PLUGINSDIR}
        COMPONENT Libraries
        NAMELINK_COMPONENT Development)
    windeployqt(${name})
endmacro()
