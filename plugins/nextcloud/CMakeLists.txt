include(QtNoteMacro)

add_qtnote_plugin(
    nextcloud
    "Allows to load and save notes using Nextcloud Notes"
    ON
    unix macosx windows android
    SOURCES
        nextclouddto.h
        nextcloudworker.cpp
        nextcloudworker.h
        nextcloudstorage.cpp
        nextcloudstorage.h
        nextcloudplugin.cpp
        nextcloudplugin.h
)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)

target_link_libraries(
    nextcloud
    ${EXTRA_LINK_TARGET}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Network
)

install_qtnote_plugin(nextcloud)
