include(QtNoteMacro)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)
if(QTNOTE_BUILD_MOBILE)
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Multimedia REQUIRED)
else()
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Multimedia QUIET)
    if(NOT TARGET Qt${QT_VERSION_MAJOR}::Multimedia)
        message(STATUS "Skipping Yandex plugin: Qt Multimedia is unavailable")
        return()
    endif()
endif()

set(yandex_sources
    yandexapiutils.cpp
    yandexapiutils.h
    yandexplugin.cpp
    yandexplugin.h
    yandexsettingscontroller.cpp
    yandexsettingscontroller.h
    yandexspeechjob.cpp
    yandexspeechjob.h
    yandexspeechutils.cpp
    yandexspeechutils.h
    yandex.qrc
)

if(QTNOTE_BUILD_MOBILE)
    add_qtnote_bundled_plugin(yandex SOURCES ${yandex_sources})
    target_link_libraries(qtnote_bundled_yandex PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Network
        Qt${QT_VERSION_MAJOR}::Multimedia
    )
    return()
endif()

add_qtnote_plugin(yandex "Yandex cloud services" ON unix macosx windows android
    METADATA plugin-metadata.json ICON yandex-logo.svg SOURCES ${yandex_sources})

target_link_libraries(yandex
    ${EXTRA_LINK_TARGET}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Multimedia
)

install_qtnote_plugin(yandex)
