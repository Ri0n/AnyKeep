include(AnyKeepMacro)

find_package(
  Qt6
  COMPONENTS Network
  REQUIRED)
if(ANYKEEP_BUILD_MOBILE)
  find_package(Qt6 COMPONENTS Multimedia REQUIRED)
else()
  find_package(Qt6 COMPONENTS Multimedia QUIET)
  if(NOT TARGET Qt6::Multimedia)
    message(STATUS "Skipping Yandex plugin: Qt Multimedia is unavailable")
    return()
  endif()
endif()

set(yandex_sources
    yandexapiutils.cpp
    yandexapiutils.h
    yandexplugin.cpp
    yandexplugin.h
    yandexsettingscontroller.cpp
    yandexsettingscontroller.h
    yandexspeechjob.cpp
    yandexspeechjob.h
    yandexspeechutils.cpp
    yandexspeechutils.h
    yandex.qrc)

if(ANYKEEP_BUILD_MOBILE)
  add_anykeep_bundled_plugin(yandex SOURCES ${yandex_sources})
  target_link_libraries(anykeep_bundled_yandex PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::Multimedia)
  return()
endif()

add_anykeep_plugin(
  yandex
  "Yandex cloud services"
  ON
  unix
  macosx
  windows
  android
  METADATA
  plugin-metadata.json
  ICON
  yandex-logo.svg
  SOURCES
  ${yandex_sources})

target_link_libraries(yandex ${EXTRA_LINK_TARGET} Qt6::Core Qt6::Gui Qt6::Network Qt6::Multimedia)

install_anykeep_plugin(yandex)
