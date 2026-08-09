include(AnyKeepMacro)
include(GNUInstallDirs)

set(anykeep_gnome_shell_extension_uuid anykeep@ri0n.github.io)
set(anykeep_gnome_gettext_domain anykeep-gnome)

set(_gnome_sources gnome.cpp gnome.h gnome.qrc)
if(ANYKEEP_ENABLE_X11)
  list(APPEND _gnome_sources x11util.cpp x11util.h)
endif()

add_anykeep_plugin(
  gnome
  "Gnome integration"
  ON
  unix
  METADATA
  plugin-metadata.json
  ICON
  logo.png
  SOURCES
  ${_gnome_sources})

target_link_libraries(gnome ${EXTRA_LINK_TARGET} Qt6::Core Qt6::Gui Qt6::Widgets)
if(ANYKEEP_ENABLE_X11 AND UNIXLIKE)
  target_compile_definitions(gnome PRIVATE ANYKEEP_ENABLE_X11)
  find_package(X11 REQUIRED)
  target_link_libraries(gnome X11::X11)
endif()

install(
  DIRECTORY shellextension/${anykeep_gnome_shell_extension_uuid}/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${anykeep_gnome_shell_extension_uuid}
  COMPONENT Executable
  PATTERN "po" EXCLUDE)

install(
  FILES ${CMAKE_SOURCE_DIR}/libanykeep/images/anykeep.svg ${CMAKE_SOURCE_DIR}/libanykeep/images/anykeep-symbolic.svg
  DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${anykeep_gnome_shell_extension_uuid}/icons
  COMPONENT Executable)

find_program(GLIB_COMPILE_SCHEMAS_EXECUTABLE glib-compile-schemas)
if(GLIB_COMPILE_SCHEMAS_EXECUTABLE)
  set(anykeep_gnome_SCHEMA_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/shellextension/${anykeep_gnome_shell_extension_uuid}/schemas")
  set(anykeep_gnome_COMPILED_SCHEMA_DIR "${CMAKE_CURRENT_BINARY_DIR}/shellextension-schemas")
  set(anykeep_gnome_COMPILED_SCHEMA "${anykeep_gnome_COMPILED_SCHEMA_DIR}/gschemas.compiled")

  file(GLOB anykeep_gnome_SCHEMA_FILES "${anykeep_gnome_SCHEMA_DIR}/*.gschema.xml")
  add_custom_command(
    OUTPUT ${anykeep_gnome_COMPILED_SCHEMA}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${anykeep_gnome_COMPILED_SCHEMA_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${anykeep_gnome_SCHEMA_FILES} ${anykeep_gnome_COMPILED_SCHEMA_DIR}
    COMMAND ${GLIB_COMPILE_SCHEMAS_EXECUTABLE} ${anykeep_gnome_COMPILED_SCHEMA_DIR}
    DEPENDS ${anykeep_gnome_SCHEMA_FILES}
    VERBATIM)
  add_custom_target(anykeep_gnome_shell_extension_schemas ALL DEPENDS ${anykeep_gnome_COMPILED_SCHEMA})
  add_dependencies(gnome anykeep_gnome_shell_extension_schemas)

  install(
    FILES ${anykeep_gnome_COMPILED_SCHEMA}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${anykeep_gnome_shell_extension_uuid}/schemas
    COMPONENT Executable)
else()
  message(WARNING "glib-compile-schemas was not found, GNOME Shell extension settings schema will not be compiled")
endif()

find_program(GETTEXT_MSGFMT_EXECUTABLE msgfmt)
if(GETTEXT_MSGFMT_EXECUTABLE)
  file(GLOB anykeep_gnome_PO_FILES
       "${CMAKE_CURRENT_SOURCE_DIR}/shellextension/${anykeep_gnome_shell_extension_uuid}/po/*.po")

  set(anykeep_gnome_MO_FILES)
  foreach(anykeep_gnome_PO_FILE ${anykeep_gnome_PO_FILES})
    get_filename_component(anykeep_gnome_LANG ${anykeep_gnome_PO_FILE} NAME_WE)
    set(anykeep_gnome_MO_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/shellextension-locale/${anykeep_gnome_LANG}/LC_MESSAGES/${anykeep_gnome_gettext_domain}.mo"
    )
    add_custom_command(
      OUTPUT ${anykeep_gnome_MO_FILE}
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "${CMAKE_CURRENT_BINARY_DIR}/shellextension-locale/${anykeep_gnome_LANG}/LC_MESSAGES"
      COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} --check --output-file=${anykeep_gnome_MO_FILE} ${anykeep_gnome_PO_FILE}
      DEPENDS ${anykeep_gnome_PO_FILE}
      VERBATIM)
    list(APPEND anykeep_gnome_MO_FILES ${anykeep_gnome_MO_FILE})

    install(
      FILES ${anykeep_gnome_MO_FILE}
      DESTINATION
        ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${anykeep_gnome_shell_extension_uuid}/locale/${anykeep_gnome_LANG}/LC_MESSAGES
      COMPONENT Executable)
  endforeach()

  if(anykeep_gnome_MO_FILES)
    add_custom_target(anykeep_gnome_shell_extension_translations ALL DEPENDS ${anykeep_gnome_MO_FILES})
    add_dependencies(gnome anykeep_gnome_shell_extension_translations)
  endif()
else()
  message(WARNING "msgfmt was not found, GNOME Shell extension translations will not be built")
endif()

install_anykeep_plugin(gnome)
