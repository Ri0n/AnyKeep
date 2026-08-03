include(AnyKeepMacro)

if("${CMAKE_MODULE_PATH}" MATCHES "^.*conan.*$")
    message(STATUS "Conan detected. Trying to get hunspell from there")
    find_package(hunspell)
    set(HUNSPELL_DEP hunspell::hunspell)
    set(HUNSPEL_ENABLED ${hunspell_FOUND})
else()
    anykeep_optional_pkgconfig(Hunspell IMPORTED_TARGET hunspell)
    set(HUNSPELL_DEP  PkgConfig::Hunspell)
    set(HUNSPEL_ENABLED ${Hunspell_FOUND})
endif()
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network)
if (NOT Qt${QT_VERSION_MAJOR}Network_FOUND)
    set(HUNSPEL_ENABLED FALSE)
endif()


add_anykeep_plugin(spellchecker "Check for spelling errors" ${HUNSPEL_ENABLED} unix macosx windows android METADATA plugin-metadata.json ICON logo.png SOURCES
    engineinterface.cpp
    engineinterface.h
    hunspellengine.cpp
    hunspellengine.h
    spellchecker.qrc
    spellcheckplugin.cpp
    spellcheckplugin.h
)

target_sources(spellchecker
  PRIVATE
    hunspelldownloader.h hunspelldownloader.cpp
    dictionarydownloader.h dictionarydownloader.cpp
)


if(WIN32)
    add_compile_definitions(spellchecker PRIVATE DICTSUBDIR=/spellcheck/myspell)
endif()

target_link_libraries(spellchecker
    ${EXTRA_LINK_TARGET}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Network
    ${HUNSPELL_DEP}
)

install_anykeep_plugin(spellchecker)
