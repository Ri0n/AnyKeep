include(QtNoteMacro)

add_qtnote_plugin(tomboy "Allows to load notes from Tomboy" ON unix macosx windows METADATA plugin-metadata.json ICON tomboy22.png SOURCES
    tomboyplugin.cpp
    tomboyplugin.h
    tomboy.qrc
    tomboynoteformat.cpp
    tomboynoteformat.h
    tomboystorage.cpp
    tomboystorage.h
)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Xml REQUIRED)
target_link_libraries(tomboy ${EXTRA_LINK_TARGET} Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Xml)

install_qtnote_plugin(tomboy)
