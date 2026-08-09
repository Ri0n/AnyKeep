include(AnyKeepMacro)

set(nextcloud_sources
    nextcloudcategory.cpp
    nextcloudcategory.h
    nextclouddto.h
    nextcloudworker.cpp
    nextcloudworker.h
    nextcloudstorage.cpp
    nextcloudstorage.h
    nextcloudplugin.cpp
    nextcloudplugin.h
    nextcloudresources.qrc)

find_package(
  Qt6
  COMPONENTS Network
  REQUIRED)

if(ANYKEEP_BUILD_MOBILE)
  add_anykeep_bundled_plugin(nextcloud SOURCES ${nextcloud_sources})
  target_link_libraries(anykeep_bundled_nextcloud PUBLIC Qt6::Core Qt6::Gui Qt6::Network)
  return()
endif()

add_anykeep_plugin(
  nextcloud
  "Allows to load and save notes using Nextcloud Notes"
  ON
  unix
  macosx
  windows
  android
  METADATA
  plugin-metadata.json
  ICON
  nextcloud-notes.svg
  SOURCES
  ${nextcloud_sources})

target_link_libraries(nextcloud ${EXTRA_LINK_TARGET} Qt6::Core Qt6::Gui Qt6::Network)

install_anykeep_plugin(nextcloud)
