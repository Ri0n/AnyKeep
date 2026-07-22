include(QtNoteMacro)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)

add_qtnote_plugin(gemini "Gemini API speech recognition backend" ON unix macosx windows android SOURCES
    geminiplugin.cpp
    geminiplugin.h
)

target_link_libraries(gemini
    ${EXTRA_LINK_TARGET}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Network
)

install_qtnote_plugin(gemini)
