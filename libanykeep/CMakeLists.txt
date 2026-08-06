cmake_minimum_required(VERSION 3.14.0)
include(AnyKeepMacro)
project(libanykeep VERSION ${ANYKEEP_VERSION} LANGUAGES CXX)

if(NOT DEFINED ANYKEEP_ABI_VERSION)
    set(ANYKEEP_ABI_VERSION 3 CACHE STRING "libanykeep ABI version")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

file(GLOB libanykeep_SRCS CONFIGURE_DEPENDS "./*.cpp")
file(GLOB libanykeep_HDRS CONFIGURE_DEPENDS "./*.h")
file(GLOB libanykeep_UIS CONFIGURE_DEPENDS "./*.ui")
set(libanykeep_PRIVATE_HDRS
    "${CMAKE_CURRENT_SOURCE_DIR}/notedocumenthistory.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ptffolderindex.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/updatecontroller.h"
)
list(APPEND libanykeep_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/iconutils.cpp")
list(APPEND libanykeep_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/iconutils.h")
list(REMOVE_DUPLICATES libanykeep_SRCS)
list(REMOVE_DUPLICATES libanykeep_HDRS)
list(FILTER libanykeep_HDRS EXCLUDE REGEX "/notedocumenthistory\\.h$")
list(FILTER libanykeep_HDRS EXCLUDE REGEX "/ptffolderindex\\.h$")
list(FILTER libanykeep_HDRS EXCLUDE REGEX "/updatecontroller\\.h$")
list(REMOVE_ITEM libanykeep_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/freedesktopnotifier.cpp")
list(REMOVE_ITEM libanykeep_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/freedesktopnotifier.h")
list(REMOVE_ITEM libanykeep_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/anykeepdbus.cpp")
list(REMOVE_ITEM libanykeep_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/anykeepdbus.h")

if(UNIXLIKE)
    list(APPEND libanykeep_SRCS freedesktopnotifier.cpp)
    list(APPEND libanykeep_HDRS freedesktopnotifier.h)
endif()

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS DBus QUIET)
if(TARGET Qt${QT_VERSION_MAJOR}::DBus)
    list(APPEND libanykeep_SRCS anykeepdbus.cpp)
    list(APPEND libanykeep_HDRS anykeepdbus.h)
endif()
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Multimedia QUIET)
if(NOT TARGET qca-qt${QT_VERSION_MAJOR})
    find_package(Qca-qt${QT_VERSION_MAJOR} REQUIRED)
endif()
if(NOT TARGET Qt${QT_VERSION_MAJOR}Keychain::Qt${QT_VERSION_MAJOR}Keychain)
    find_package(Qt${QT_VERSION_MAJOR}Keychain REQUIRED)
endif()

set(anykeep_CORE_SRCS
    audioattachmentrecorder.cpp
    audioplaybackcontroller.cpp
    bundledpluginregistry.cpp
    conflictresolver.cpp
    corestorageregistry.cpp
    dialogservice.cpp
    editorcursorcontroller.cpp
    editorplatformbackend.cpp
    drafts/core.cpp
    drafts/publication.cpp
    drafts/sessions.cpp
    drafts/transfer.cpp
    filedraftstore.cpp
    filefoldercatalogstore.cpp
    filenoterulestore.cpp
    fileremotecachestore.cpp
    filestorage.cpp
    folders/assignments.cpp
    folders/core.cpp
    folders/reconciliation.cpp
    folders/tree.cpp
    foldercatalogmanager.cpp
    foldernotesmodel.cpp
    folderoperationscontroller.cpp
    iconutils.cpp
    localdatakeystore.cpp
    localmediaimageprovider.cpp
    localmediastore.cpp
    mediareference.cpp
    note.cpp
    noteblockmodel/core.cpp
    noteblockmodel/fragments.cpp
    noteblockmodel/lists.cpp
    noteblockmodel/markdown.cpp
    noteblockmodel/private.h
    noteblockmodel/search.cpp
    noteblockmodel/structure.cpp
    noteblockmodel/tables_media.cpp
    notedata.cpp
    notedocumenthistory.cpp
    notehighlighter.cpp
    noteeditor.cpp
    editor/clipboard.cpp
    editor/fragment_transfer.cpp
    editor/inline_formatting.cpp
    notefragment.cpp
    notefragmentmediatransfer.cpp
    notemanager.cpp
    noterule.cpp
    noteruleapplicationcontroller.cpp
    noterulemanager.cpp
    notesindex.cpp
    notesmodel.cpp
    notessearchmodel.cpp
    workspace/core.cpp
    workspace/folders.cpp
    workspace/notes.cpp
    workspace/transfer.cpp
    pluginhost.cpp
    pluginiconimageprovider.cpp
    pluginlistmodel.cpp
    recentnotesmodel.cpp
    rulescontroller.cpp
    notestorage.cpp
    notetagline.cpp
    notetransfercontroller.cpp
    ptfstorage.cpp
    ptffolderindex.cpp
    secureenvelope.cpp
    securekeystore.cpp
    spellcheckprovider.cpp
    settingscontroller.cpp
    storageiconimageprovider.cpp
    themediconimageprovider.cpp
    storagejob.cpp
    storageprioritymodel.cpp
    utils.cpp
    windowgeometryutils.cpp
)
set(anykeep_CORE_HDRS
    audioattachmentrecorder.h
    audioplaybackcontroller.h
    bundledplugininterface.h
    bundledpluginregistry.h
    conflictresolver.h
    corestorageregistry.h
    dialogservice.h
    editorcursorcontroller.h
    editorplatformbackend.h
    draftmanager.h
    draftstore.h
    filedraftstore.h
    filefoldercatalogstore.h
    filenoterulestore.h
    fileremotecachestore.h
    filestorage.h
    foldercatalog.h
    foldercatalogmanager.h
    foldernotesmodel.h
    folderoperationscontroller.h
    localdatakeystore.h
    localmediaimageprovider.h
    localmediastore.h
    highlighterext.h
    mediareference.h
    note.h
    noteblockmodel.h
    notedata.h
    noteeditor.h
    notehighlighter.h
    noterule.h
    noteruleapplicationcontroller.h
    noterulemanager.h
    notefragment.h
    notefragmentmediatransfer.h
    notemanager.h
    notesindex.h
    notesmodel.h
    notessearchmodel.h
    notesworkspacecontroller.h
    pluginhost.h
    pluginhostinterface.h
    pluginiconimageprovider.h
    pluginlistmodel.h
    pluginlistsource.h
    recentnotesmodel.h
    rulescontroller.h
    notestorage.h
    notetagline.h
    notetransfercontroller.h
    ptfstorage.h
    ptffolderindex.h
    remotecachestore.h
    secureenvelope.h
    securekeystore.h
    settingscontroller.h
    settingsproviderinterface.h
    speechrecognitionprovider.h
    spellcheckprovider.h
    storageiconimageprovider.h
    themediconimageprovider.h
    storagejob.h
    storageprioritymodel.h
    utils.h
    windowgeometryutils.h
)
set(anykeep_CORE_UIS)

set(anykeep_PUBLIC_HDRS ${libanykeep_HDRS})
foreach(core_source IN LISTS anykeep_CORE_SRCS anykeep_CORE_UIS)
    list(REMOVE_ITEM libanykeep_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/${core_source}" "${core_source}")
    list(REMOVE_ITEM libanykeep_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/./${core_source}" "./${core_source}")
    list(REMOVE_ITEM libanykeep_UIS "${CMAKE_CURRENT_SOURCE_DIR}/${core_source}" "${core_source}")
    list(REMOVE_ITEM libanykeep_UIS "${CMAKE_CURRENT_SOURCE_DIR}/./${core_source}" "./${core_source}")
endforeach()
foreach(core_header IN LISTS anykeep_CORE_HDRS)
    list(REMOVE_ITEM libanykeep_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/${core_header}" "${core_header}")
    list(REMOVE_ITEM libanykeep_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/./${core_header}" "./${core_header}")
endforeach()

include(GenerateExportHeader)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)
add_library(anykeep_core OBJECT
    core.qrc
    ${anykeep_CORE_HDRS}
    ${anykeep_CORE_SRCS}
    ${anykeep_CORE_UIS}
)
target_include_directories(anykeep_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../plugins" "${CMAKE_BINARY_DIR}")
target_include_directories(anykeep_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include/anykeep>  # <prefix>/include/qnote
    )
set_target_properties(anykeep_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(anykeep_core PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Quick Qt${QT_VERSION_MAJOR}::Qml Qt${QT_VERSION_MAJOR}::Svg)
target_link_libraries(anykeep_core PRIVATE qca-qt${QT_VERSION_MAJOR} QSourceHighlite::QSourceHighlite)
target_link_libraries(anykeep_core PRIVATE Qt${QT_VERSION_MAJOR}Keychain::Qt${QT_VERSION_MAJOR}Keychain)
if(TARGET Qt${QT_VERSION_MAJOR}::Multimedia)
    target_link_libraries(anykeep_core PUBLIC Qt${QT_VERSION_MAJOR}::Multimedia)
    target_compile_definitions(anykeep_core PUBLIC ANYKEEP_MULTIMEDIA_AVAILABLE)
endif()
foreach(anykeep_core_dependency anykeep_bundled_qca anykeep_bundled_qtkeychain anykeep_bundled_qsourcehighlite)
    if(TARGET ${anykeep_core_dependency})
        add_dependencies(anykeep_core ${anykeep_core_dependency})
    endif()
endforeach()

if(ANYKEEP_CORE_ONLY)
    generate_export_header(anykeep_core BASE_NAME anykeep)
    set(anykeep_lib anykeep_core PARENT_SCOPE)
    return()
endif()

add_library(anykeep SHARED
    ${libanykeep_HDRS}
    ${libanykeep_PRIVATE_HDRS}
    ${libanykeep_SRCS}
    ${libanykeep_UIS}
    $<TARGET_OBJECTS:anykeep_core>
    main.qrc
)
generate_export_header(anykeep)

set(anykeep_lib anykeep PARENT_SCOPE)

target_include_directories(anykeep PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../plugins" "${CMAKE_BINARY_DIR}")
target_include_directories(anykeep PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include/anykeep>  # <prefix>/include/qnote
    )

set_target_properties(anykeep PROPERTIES
  VERSION ${ANYKEEP_VERSION}
  SOVERSION ${ANYKEEP_ABI_VERSION}
)
if(WIN32)
    set_target_properties(anykeep PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${EXECUTABLE_OUTPUT_PATH}"
    )
else()
    set_target_properties(anykeep PROPERTIES
        PUBLIC_HEADER "${anykeep_PUBLIC_HDRS};${PROJECT_BINARY_DIR}/anykeep_export.h"
    )
endif()
target_link_libraries(anykeep PUBLIC anykeep_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::PrintSupport)
target_link_libraries(anykeep PRIVATE Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Quick Qt${QT_VERSION_MAJOR}::QuickWidgets Qt${QT_VERSION_MAJOR}::Qml Qt${QT_VERSION_MAJOR}::QuickControls2)
target_link_libraries(anykeep PRIVATE qca-qt${QT_VERSION_MAJOR})
target_link_libraries(anykeep PRIVATE Qt${QT_VERSION_MAJOR}Keychain::Qt${QT_VERSION_MAJOR}Keychain)
if(TARGET Qt${QT_VERSION_MAJOR}::DBus)
    target_link_libraries(anykeep PUBLIC Qt${QT_VERSION_MAJOR}::DBus)
    target_compile_definitions(anykeep PUBLIC ANYKEEP_DBUS_AVAILABLE)
endif()
if(TARGET Qt${QT_VERSION_MAJOR}::Multimedia)
    target_link_libraries(anykeep PUBLIC Qt${QT_VERSION_MAJOR}::Multimedia)
    target_compile_definitions(anykeep PUBLIC ANYKEEP_MULTIMEDIA_AVAILABLE)
endif()
if (APPLE)
    target_link_libraries(anykeep PUBLIC "-framework CoreFoundation" "-framework Carbon")
endif()

if(WIN32)
    # Install only the DLL for Windows
    install(TARGETS anykeep
        RUNTIME
            DESTINATION "."
            COMPONENT Libraries
    )
    windeployqt(anykeep)
else()
    install(TARGETS anykeep
      LIBRARY
        DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        COMPONENT Libraries
        NAMELINK_COMPONENT Development
      PUBLIC_HEADER
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/anykeep"
        COMPONENT Development
    )
endif()
