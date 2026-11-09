include(AnyKeepMacro)

find_package(
  Qt6
  COMPONENTS Network
  REQUIRED)
find_package(Qt6 COMPONENTS Multimedia QUIET)
set(_yandex_buildable OFF)
if(TARGET Qt6::Multimedia)
  set(_yandex_buildable ON)
endif()
if(ANYKEEP_BUILD_MOBILE AND NOT _yandex_buildable)
  anykeep_plugin_mode(yandex _yandex_mode)
  if(_yandex_mode STREQUAL "ON")
    message(FATAL_ERROR "ANYKEEP_PLUGIN_ENABLE_yandex=ON requires Qt6 Multimedia")
  endif()
  message(STATUS "Skipping bundled Yandex plugin: Qt6 Multimedia is unavailable")
  return()
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
  ${_yandex_buildable}
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
