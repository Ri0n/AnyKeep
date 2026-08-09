include(AnyKeepMacro)

find_package(
  Qt6
  COMPONENTS Network
  REQUIRED)

set(gemini_sources geminiplugin.cpp geminiplugin.h gemini.qrc)

if(ANYKEEP_BUILD_MOBILE)
  add_anykeep_bundled_plugin(gemini SOURCES ${gemini_sources})
  target_link_libraries(anykeep_bundled_gemini PUBLIC Qt6::Core Qt6::Gui Qt6::Network)
  return()
endif()

add_anykeep_plugin(
  gemini
  "Gemini API speech recognition backend"
  ON
  unix
  macosx
  windows
  android
  METADATA
  plugin-metadata.json
  ICON
  google-gemini.svg
  SOURCES
  ${gemini_sources})

target_link_libraries(gemini ${EXTRA_LINK_TARGET} Qt6::Core Qt6::Gui Qt6::Network)

install_anykeep_plugin(gemini)
