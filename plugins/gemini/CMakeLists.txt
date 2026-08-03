include(AnyKeepMacro)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)

set(gemini_sources
    geminiplugin.cpp
    geminiplugin.h
    gemini.qrc
)

if(ANYKEEP_BUILD_MOBILE)
    add_anykeep_bundled_plugin(gemini SOURCES ${gemini_sources})
    target_link_libraries(anykeep_bundled_gemini PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Network
    )
    return()
endif()

add_anykeep_plugin(gemini "Gemini API speech recognition backend" ON unix macosx windows android METADATA plugin-metadata.json ICON google-gemini.svg SOURCES
    ${gemini_sources}
)

target_link_libraries(gemini
    ${EXTRA_LINK_TARGET}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Network
)

install_anykeep_plugin(gemini)
