include(AnyKeepMacro)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)

set(openaiwhisper_sources
    openaiwhisperplugin.cpp
    openaiwhisperplugin.h
    openaiwhisper.qrc
)

if(ANYKEEP_BUILD_MOBILE)
    add_anykeep_bundled_plugin(openaiwhisper SOURCES ${openaiwhisper_sources})
    target_link_libraries(anykeep_bundled_openaiwhisper PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Network
    )
    return()
endif()

add_anykeep_plugin(openaiwhisper "OpenAI Whisper speech recognition backend" ON unix macosx windows android METADATA plugin-metadata.json ICON openai-logo.svg SOURCES
    ${openaiwhisper_sources}
)

target_link_libraries(openaiwhisper
    ${EXTRA_LINK_TARGET}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Network
)

install_anykeep_plugin(openaiwhisper)
