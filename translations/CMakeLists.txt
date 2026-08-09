project(translations)

find_package(
  Qt6
  COMPONENTS LinguistTools
  REQUIRED)

file(GLOB anykeep_TRS "${CMAKE_CURRENT_SOURCE_DIR}/anykeep_*.ts")
if(WIN32)
  set_source_files_properties(${anykeep_TRS} PROPERTIES OUTPUT_LOCATION "${EXECUTABLE_OUTPUT_PATH}/translations")
endif()

if(Qt6LinguistTools_VERSION VERSION_LESS 6.7.0)
  set(PROJECT_DIRS ${CMAKE_SOURCE_DIR} ${anykeepxe_SOURCE_DIR} ${libanykeep_SOURCE_DIR} ${plugins_SOURCE_DIR})

  # update translations with: make lupdate
  add_custom_target(lupdate)
  foreach(_LANG ${anykeep_TRS})
    get_filename_component(_SHORT_LANG ${_LANG} NAME_WE)
    string(REPLACE "@" "_" _SHORT_LANG ${_SHORT_LANG})
    add_custom_command(
      TARGET lupdate
      PRE_BUILD
      COMMAND Qt6::lupdate ARGS -no-obsolete -recursive ${PROJECT_DIRS} -ts ${_LANG}
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      VERBATIM)
  endforeach()
  qt_add_translation(anykeep_QM ${anykeep_TRS})

  add_custom_target(build_translations ALL DEPENDS ${anykeep_QM})
else()
  set(LANGS_SOURCE_TARGETS)
  foreach(trg baseintegration gnome kdeintegration spellchecker tomboy macosx)
    # if(TARGET ${trg})
    list(APPEND LANGS_SOURCE_TARGETS ${trg})
    # endif()
  endforeach()
  set(PLASMOID_TRANSLATION_SOURCES
      ${CMAKE_SOURCE_DIR}/plugins/kdeintegration/plasmoid/package/contents/ui/FullRepresentation.qml
      ${CMAKE_SOURCE_DIR}/plugins/kdeintegration/plasmoid/package/contents/ui/main.qml)
  qt_add_translations(
    TARGETS
    anykeepexe
    SOURCE_TARGETS
    anykeepexe
    anykeep
    ${LANGS_SOURCE_TARGETS}
    SOURCES
    ${PLASMOID_TRANSLATION_SOURCES}
    TS_FILE_BASE
    anykeep
    TS_FILES
    ${anykeep_TRS}
    QM_FILES_OUTPUT_VARIABLE
    anykeep_QM)
endif()

install(
  FILES ${anykeep_QM}
  DESTINATION ${TRANSLATIONSDIR}
  COMPONENT Executable)
