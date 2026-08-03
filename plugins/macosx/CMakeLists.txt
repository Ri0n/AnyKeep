include(AnyKeepMacro)

add_anykeep_plugin(macosx "Mac OS X integration" ON macosx METADATA plugin-metadata.json ICON logo.png SOURCES
    macosx.cpp
    macosx.h
    macosx.qrc
    macosxtray.cpp
    macosxtray.h
)
target_link_libraries(macosx ${EXTRA_LINK_TARGET} Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

install_anykeep_plugin(macosx)
