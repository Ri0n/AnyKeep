include(AnyKeepMacro)

find_package(
  Qt6
  COMPONENTS Network
  REQUIRED)

set(openaiwhisper_sources openaiwhisperplugin.cpp openaiwhisperplugin.h openaiwhisper.qrc)

if(ANYKEEP_BUILD_MOBILE)
  add_anykeep_bundled_plugin(openaiwhisper SOURCES ${openaiwhisper_sources})
  target_link_libraries(anykeep_bundled_openaiwhisper PUBLIC Qt6::Core Qt6::Gui Qt6::Network)
  return()
endif()

add_anykeep_plugin(
  openaiwhisper
  "OpenAI Whisper speech recognition backend"
  ON
  unix
  macosx
  windows
  android
  METADATA
  plugin-metadata.json
  ICON
  openai-logo.svg
  SOURCES
  ${openaiwhisper_sources})

target_link_libraries(openaiwhisper ${EXTRA_LINK_TARGET} Qt6::Core Qt6::Gui Qt6::Network)

install_anykeep_plugin(openaiwhisper)
