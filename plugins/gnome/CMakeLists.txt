include(QtNoteMacro)
include(GNUInstallDirs)

set(qtnote_gnome_shell_extension_uuid qtnote@ri0n.github.io)
set(qtnote_gnome_gettext_domain qtnote-gnome)

set(_gnome_sources
    gnome.cpp
    gnome.h
    gnome.qrc
)
if(QTNOTE_ENABLE_X11)
    list(APPEND _gnome_sources x11util.cpp x11util.h)
endif()

add_qtnote_plugin(gnome "Gnome integration" ON unix METADATA plugin-metadata.json ICON logo.png SOURCES ${_gnome_sources})

target_link_libraries(gnome ${EXTRA_LINK_TARGET} Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
if(QTNOTE_ENABLE_X11 AND UNIXLIKE AND ${QT_VERSION_MAJOR} LESS 6)
    target_compile_definitions(gnome PRIVATE QTNOTE_ENABLE_X11)
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS X11Extras REQUIRED)
    target_link_libraries(gnome Qt${QT_VERSION_MAJOR}::X11Extras)
elseif(QTNOTE_ENABLE_X11 AND UNIXLIKE)
    target_compile_definitions(gnome PRIVATE QTNOTE_ENABLE_X11)
    find_package(X11 REQUIRED)
    target_link_libraries(gnome X11::X11)
endif()

install(
    DIRECTORY shellextension/${qtnote_gnome_shell_extension_uuid}/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${qtnote_gnome_shell_extension_uuid}
    COMPONENT Executable
    PATTERN "po" EXCLUDE
)

install(
    FILES
        ${CMAKE_SOURCE_DIR}/libqtnote/images/qtnote.svg
        ${CMAKE_SOURCE_DIR}/libqtnote/images/qtnote-symbolic.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${qtnote_gnome_shell_extension_uuid}/icons
    COMPONENT Executable
)

find_program(GLIB_COMPILE_SCHEMAS_EXECUTABLE glib-compile-schemas)
if(GLIB_COMPILE_SCHEMAS_EXECUTABLE)
    set(qtnote_gnome_SCHEMA_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/shellextension/${qtnote_gnome_shell_extension_uuid}/schemas"
    )
    set(qtnote_gnome_COMPILED_SCHEMA_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/shellextension-schemas"
    )
    set(qtnote_gnome_COMPILED_SCHEMA
        "${qtnote_gnome_COMPILED_SCHEMA_DIR}/gschemas.compiled"
    )

    file(GLOB qtnote_gnome_SCHEMA_FILES "${qtnote_gnome_SCHEMA_DIR}/*.gschema.xml")
    add_custom_command(
        OUTPUT ${qtnote_gnome_COMPILED_SCHEMA}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${qtnote_gnome_COMPILED_SCHEMA_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${qtnote_gnome_SCHEMA_FILES} ${qtnote_gnome_COMPILED_SCHEMA_DIR}
        COMMAND ${GLIB_COMPILE_SCHEMAS_EXECUTABLE} ${qtnote_gnome_COMPILED_SCHEMA_DIR}
        DEPENDS ${qtnote_gnome_SCHEMA_FILES}
        VERBATIM
    )
    add_custom_target(qtnote_gnome_shell_extension_schemas ALL DEPENDS ${qtnote_gnome_COMPILED_SCHEMA})
    add_dependencies(gnome qtnote_gnome_shell_extension_schemas)

    install(
        FILES ${qtnote_gnome_COMPILED_SCHEMA}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${qtnote_gnome_shell_extension_uuid}/schemas
        COMPONENT Executable
    )
else()
    message(WARNING "glib-compile-schemas was not found, GNOME Shell extension settings schema will not be compiled")
endif()

find_program(GETTEXT_MSGFMT_EXECUTABLE msgfmt)
if(GETTEXT_MSGFMT_EXECUTABLE)
    file(GLOB qtnote_gnome_PO_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/shellextension/${qtnote_gnome_shell_extension_uuid}/po/*.po"
    )

    set(qtnote_gnome_MO_FILES)
    foreach(qtnote_gnome_PO_FILE ${qtnote_gnome_PO_FILES})
        get_filename_component(qtnote_gnome_LANG ${qtnote_gnome_PO_FILE} NAME_WE)
        set(qtnote_gnome_MO_FILE
            "${CMAKE_CURRENT_BINARY_DIR}/shellextension-locale/${qtnote_gnome_LANG}/LC_MESSAGES/${qtnote_gnome_gettext_domain}.mo"
        )
        add_custom_command(
            OUTPUT ${qtnote_gnome_MO_FILE}
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_CURRENT_BINARY_DIR}/shellextension-locale/${qtnote_gnome_LANG}/LC_MESSAGES"
            COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} --check --output-file=${qtnote_gnome_MO_FILE} ${qtnote_gnome_PO_FILE}
            DEPENDS ${qtnote_gnome_PO_FILE}
            VERBATIM
        )
        list(APPEND qtnote_gnome_MO_FILES ${qtnote_gnome_MO_FILE})

        install(
            FILES ${qtnote_gnome_MO_FILE}
            DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${qtnote_gnome_shell_extension_uuid}/locale/${qtnote_gnome_LANG}/LC_MESSAGES
            COMPONENT Executable
        )
    endforeach()

    if(qtnote_gnome_MO_FILES)
        add_custom_target(qtnote_gnome_shell_extension_translations ALL DEPENDS ${qtnote_gnome_MO_FILES})
        add_dependencies(gnome qtnote_gnome_shell_extension_translations)
    endif()
else()
    message(WARNING "msgfmt was not found, GNOME Shell extension translations will not be built")
endif()

install_qtnote_plugin(gnome)
