include(AnyKeepMacro)

find_package(Qt6 COMPONENTS Xml QUIET)
set(_tomboy_buildable OFF)
if(TARGET Qt6::Xml)
  set(_tomboy_buildable ON)
endif()

add_anykeep_plugin(
  tomboy
  "Allows to load notes from Tomboy"
  ${_tomboy_buildable}
  unix
  macosx
  windows
  METADATA
  plugin-metadata.json
  ICON
  tomboy22.png
  SOURCES
  tomboyplugin.cpp
  tomboyplugin.h
  tomboy.qrc
  tomboynoteformat.cpp
  tomboynoteformat.h
  tomboystorage.cpp
  tomboystorage.h)
target_link_libraries(tomboy ${EXTRA_LINK_TARGET} Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Xml)

install_anykeep_plugin(tomboy)
