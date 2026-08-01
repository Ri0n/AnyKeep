include(QtNoteMacro)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)

set(openaiwhisper_sources
    openaiwhisperplugin.cpp
    openaiwhisperplugin.h
    openaiwhisper.qrc
)

if(QTNOTE_BUILD_MOBILE)
    add_qtnote_bundled_plugin(openaiwhisper SOURCES ${openaiwhisper_sources})
    target_link_libraries(qtnote_bundled_openaiwhisper PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Network
    )
    return()
endif()

add_qtnote_plugin(openaiwhisper "OpenAI Whisper speech recognition backend" ON unix macosx windows android METADATA plugin-metadata.json ICON openai-logo.svg SOURCES
    ${openaiwhisper_sources}
)

target_link_libraries(openaiwhisper
    ${EXTRA_LINK_TARGET}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Network
)

install_qtnote_plugin(openaiwhisper)
