include(QtNoteMacro)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)

set(gemini_sources
    geminiplugin.cpp
    geminiplugin.h
    gemini.qrc
)

if(QTNOTE_BUILD_MOBILE)
    add_qtnote_bundled_plugin(gemini SOURCES ${gemini_sources})
    target_link_libraries(qtnote_bundled_gemini PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Network
    )
    return()
endif()

add_qtnote_plugin(gemini "Gemini API speech recognition backend" ON unix macosx windows android METADATA plugin-metadata.json ICON google-gemini.svg SOURCES
    ${gemini_sources}
)

target_link_libraries(gemini
    ${EXTRA_LINK_TARGET}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Network
)

install_qtnote_plugin(gemini)
