cmake_minimum_required(VERSION 3.14.0)
include(QtNoteMacro)
project(libqtnote VERSION ${QTNOTE_VERSION} LANGUAGES CXX)

if(NOT DEFINED QTNOTE_ABI_VERSION)
    set(QTNOTE_ABI_VERSION 3 CACHE STRING "libqtnote ABI version")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

file(GLOB libqtnote_SRCS CONFIGURE_DEPENDS "./*.cpp")
file(GLOB libqtnote_HDRS CONFIGURE_DEPENDS "./*.h")
file(GLOB libqtnote_UIS CONFIGURE_DEPENDS "./*.ui")
set(libqtnote_PRIVATE_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/notedocumenthistory.h")
list(APPEND libqtnote_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/iconutils.cpp")
list(APPEND libqtnote_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/iconutils.h")
list(REMOVE_DUPLICATES libqtnote_SRCS)
list(REMOVE_DUPLICATES libqtnote_HDRS)
list(FILTER libqtnote_HDRS EXCLUDE REGEX "/notedocumenthistory\\.h$")
list(REMOVE_ITEM libqtnote_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/freedesktopnotifier.cpp")
list(REMOVE_ITEM libqtnote_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/freedesktopnotifier.h")
list(REMOVE_ITEM libqtnote_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/qtnotedbus.cpp")
list(REMOVE_ITEM libqtnote_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/qtnotedbus.h")

if(UNIXLIKE)
    list(APPEND libqtnote_SRCS freedesktopnotifier.cpp)
    list(APPEND libqtnote_HDRS freedesktopnotifier.h)
endif()

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS DBus QUIET)
if(TARGET Qt${QT_VERSION_MAJOR}::DBus)
    list(APPEND libqtnote_SRCS qtnotedbus.cpp)
    list(APPEND libqtnote_HDRS qtnotedbus.h)
endif()
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Multimedia QUIET)
if(NOT TARGET qca-qt${QT_VERSION_MAJOR})
    find_package(Qca-qt${QT_VERSION_MAJOR} REQUIRED)
endif()
if(NOT TARGET Qt${QT_VERSION_MAJOR}Keychain::Qt${QT_VERSION_MAJOR}Keychain)
    find_package(Qt${QT_VERSION_MAJOR}Keychain REQUIRED)
endif()

set(qtnote_CORE_SRCS
    bundledpluginregistry.cpp
    conflictresolver.cpp
    corestorageregistry.cpp
    dialogservice.cpp
    draftmanager.cpp
    filedraftstore.cpp
    fileremotecachestore.cpp
    filestorage.cpp
    iconutils.cpp
    localdatakeystore.cpp
    localmediaimageprovider.cpp
    localmediastore.cpp
    mediareference.cpp
    note.cpp
    noteblockmodel.cpp
    notedata.cpp
    notedocumenthistory.cpp
    noteeditor.cpp
    noteeditoroperations.cpp
    notefragment.cpp
    notefragmentmediatransfer.cpp
    notemanager.cpp
    notesmodel.cpp
    notessearchmodel.cpp
    notesworkspacecontroller.cpp
    pluginlistmodel.cpp
    recentnotesmodel.cpp
    notestorage.cpp
    notetransfercontroller.cpp
    ptfstorage.cpp
    secureenvelope.cpp
    securekeystore.cpp
    settingscontroller.cpp
    storageiconimageprovider.cpp
    themediconimageprovider.cpp
    storagejob.cpp
    storageprioritymodel.cpp
    utils.cpp
)
set(qtnote_CORE_HDRS
    bundledplugininterface.h
    bundledpluginregistry.h
    conflictresolver.h
    corestorageregistry.h
    dialogservice.h
    draftmanager.h
    draftstore.h
    filedraftstore.h
    fileremotecachestore.h
    filestorage.h
    localdatakeystore.h
    localmediaimageprovider.h
    localmediastore.h
    mediareference.h
    note.h
    noteblockmodel.h
    notedata.h
    noteeditor.h
    notefragment.h
    notefragmentmediatransfer.h
    notemanager.h
    notesmodel.h
    notessearchmodel.h
    notesworkspacecontroller.h
    pluginlistmodel.h
    pluginlistsource.h
    recentnotesmodel.h
    notestorage.h
    notetransfercontroller.h
    ptfstorage.h
    remotecachestore.h
    secureenvelope.h
    securekeystore.h
    settingscontroller.h
    settingsproviderinterface.h
    speechrecognitionprovider.h
    storageiconimageprovider.h
    themediconimageprovider.h
    storagejob.h
    storageprioritymodel.h
    utils.h
)
set(qtnote_CORE_UIS)

set(qtnote_PUBLIC_HDRS ${libqtnote_HDRS})
foreach(core_source IN LISTS qtnote_CORE_SRCS qtnote_CORE_UIS)
    list(REMOVE_ITEM libqtnote_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/${core_source}" "${core_source}")
    list(REMOVE_ITEM libqtnote_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/./${core_source}" "./${core_source}")
    list(REMOVE_ITEM libqtnote_UIS "${CMAKE_CURRENT_SOURCE_DIR}/${core_source}" "${core_source}")
    list(REMOVE_ITEM libqtnote_UIS "${CMAKE_CURRENT_SOURCE_DIR}/./${core_source}" "./${core_source}")
endforeach()
foreach(core_header IN LISTS qtnote_CORE_HDRS)
    list(REMOVE_ITEM libqtnote_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/${core_header}" "${core_header}")
    list(REMOVE_ITEM libqtnote_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/./${core_header}" "./${core_header}")
endforeach()

include(GenerateExportHeader)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)
add_library(qtnote_core OBJECT
    core.qrc
    ${qtnote_CORE_HDRS}
    ${qtnote_CORE_SRCS}
    ${qtnote_CORE_UIS}
)
target_include_directories(qtnote_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../plugins" "${CMAKE_BINARY_DIR}")
target_include_directories(qtnote_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include/qtnote>  # <prefix>/include/qnote
    )
set_target_properties(qtnote_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(qtnote_core PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Quick Qt${QT_VERSION_MAJOR}::Qml)
target_link_libraries(qtnote_core PRIVATE qca-qt${QT_VERSION_MAJOR})
target_link_libraries(qtnote_core PRIVATE Qt${QT_VERSION_MAJOR}Keychain::Qt${QT_VERSION_MAJOR}Keychain)
foreach(qtnote_core_dependency qtnote_bundled_qca qtnote_bundled_qtkeychain)
    if(TARGET ${qtnote_core_dependency})
        add_dependencies(qtnote_core ${qtnote_core_dependency})
    endif()
endforeach()

if(QTNOTE_CORE_ONLY)
    generate_export_header(qtnote_core BASE_NAME qtnote)
    set(qtnote_lib qtnote_core PARENT_SCOPE)
    return()
endif()

add_library(qtnote SHARED
    ${libqtnote_HDRS}
    ${libqtnote_PRIVATE_HDRS}
    ${libqtnote_SRCS}
    ${libqtnote_UIS}
    $<TARGET_OBJECTS:qtnote_core>
    main.qrc
)
generate_export_header(qtnote)

set(qtnote_lib qtnote PARENT_SCOPE)

target_include_directories(qtnote PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../plugins" "${CMAKE_BINARY_DIR}")
target_include_directories(qtnote PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include/qtnote>  # <prefix>/include/qnote
    )

set_target_properties(qtnote PROPERTIES
  VERSION ${QTNOTE_VERSION}
  SOVERSION ${QTNOTE_ABI_VERSION}
)
if(WIN32)
    set_target_properties(qtnote PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${EXECUTABLE_OUTPUT_PATH}"
    )
else()
    set_target_properties(qtnote PROPERTIES
        PUBLIC_HEADER "${qtnote_PUBLIC_HDRS};${PROJECT_BINARY_DIR}/qtnote_export.h"
    )
endif()
target_link_libraries(qtnote PUBLIC qtnote_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::PrintSupport)
target_link_libraries(qtnote PRIVATE Qt${QT_VERSION_MAJOR}::Quick Qt${QT_VERSION_MAJOR}::QuickWidgets Qt${QT_VERSION_MAJOR}::Qml Qt${QT_VERSION_MAJOR}::QuickControls2)
target_link_libraries(qtnote PRIVATE qca-qt${QT_VERSION_MAJOR})
target_link_libraries(qtnote PRIVATE Qt${QT_VERSION_MAJOR}Keychain::Qt${QT_VERSION_MAJOR}Keychain)
if(TARGET Qt${QT_VERSION_MAJOR}::DBus)
    target_link_libraries(qtnote PUBLIC Qt${QT_VERSION_MAJOR}::DBus)
    target_compile_definitions(qtnote PUBLIC QTNOTE_DBUS_AVAILABLE)
endif()
if(TARGET Qt${QT_VERSION_MAJOR}::Multimedia)
    target_link_libraries(qtnote PUBLIC Qt${QT_VERSION_MAJOR}::Multimedia)
    target_compile_definitions(qtnote PUBLIC QTNOTE_MULTIMEDIA_AVAILABLE)
endif()
if (APPLE)
    target_link_libraries(qtnote PUBLIC "-framework CoreFoundation" "-framework Carbon")
endif()

if(WIN32)
    # Install only the DLL for Windows
    install(TARGETS qtnote
        RUNTIME
            DESTINATION "."
            COMPONENT Libraries
    )
    windeployqt(qtnote)
else()
    install(TARGETS qtnote
      LIBRARY
        DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        COMPONENT Libraries
        NAMELINK_COMPONENT Development
      PUBLIC_HEADER
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/qtnote"
        COMPONENT Development
    )
endif()
