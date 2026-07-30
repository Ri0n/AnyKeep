include(QtNoteMacro)

set (QXT_SRC 3rdparty/qxt/qxtglobalshortcut.h
    3rdparty/qxt/qxtglobalshortcut.cpp
    3rdparty/qxt/qxtglobal.h
    3rdparty/qxt/qxtglobalshortcut_p.h)
if (APPLE)
    list(APPEND QXT_SRC 3rdparty/qxt/qxtglobalshortcut_mac.cpp)
endif()
if (UNIX AND NOT APPLE AND QTNOTE_ENABLE_X11)
    list(APPEND QXT_SRC 3rdparty/qxt/qxtglobalshortcut_x11.cpp)
endif()
if (WIN32)
    list(APPEND QXT_SRC 3rdparty/qxt/qxtglobalshortcut_win.cpp)
endif()

set(_baseintegration_buildable ON)
if(UNIX AND NOT APPLE AND NOT QTNOTE_ENABLE_X11)
    set(_baseintegration_buildable OFF)
endif()

add_qtnote_plugin(baseintegration "Base (fallback) desktop integration plugin" ${_baseintegration_buildable} unix macosx windows METADATA plugin-metadata.json ICON qtnote16.png SOURCES
    ${QXT_SRC}
    baseintegration.h
    baseintegration.cpp
    baseintegrationtray.h
    baseintegrationtray.cpp
    baseintegration.qrc
)

target_include_directories(baseintegration PRIVATE 3rdparty/qxt)
target_link_libraries(baseintegration ${EXTRA_LINK_TARGET} Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
if (UNIX)
    if (APPLE)
        target_link_libraries(baseintegration "-framework Carbon")
    elseif(QTNOTE_ENABLE_X11)
        find_package(X11 REQUIRED)
        target_compile_definitions(baseintegration PRIVATE QTNOTE_ENABLE_X11)
        target_link_libraries(baseintegration X11::X11)
        if(${QT_VERSION_MAJOR} LESS 6)
            find_package(Qt${QT_VERSION_MAJOR} COMPONENTS X11Extras REQUIRED)
            target_link_libraries(baseintegration Qt${QT_VERSION_MAJOR}::X11Extras)
        endif()
    endif()
endif()

install_qtnote_plugin(baseintegration)
