project(plugins)

if(ANYKEEP_BUILD_MOBILE)
  # Mobile has an explicit allow-list. These subdirectories create bundled static libraries rather than
  # desktop-discoverable shared plugins.
  set(plugins_list gemini nextcloud openaiwhisper yandex xmpppubsub)
  # Do not silently ignore explicitly requested desktop-only plugins.
  foreach(_anykeep_desktop_plugin IN ITEMS baseintegration cinnamon gnome kdeintegration macosx spellchecker tomboy)
    anykeep_plugin_mode("${_anykeep_desktop_plugin}" _anykeep_desktop_mode)
    if(_anykeep_desktop_mode STREQUAL "ON")
      message(FATAL_ERROR
              "ANYKEEP_PLUGIN_ENABLE_${_anykeep_desktop_plugin}=ON was requested, but the plugin is not supported by the mobile build")
    endif()
  endforeach()
else()
  set(plugins_list
      baseintegration
      cinnamon
      gemini
      gnome
      kdeintegration
      macosx
      nextcloud
      openaiwhisper
      spellchecker
      tomboy
      yandex
      xmpppubsub)
endif()

foreach(plugin ${plugins_list})
  set(_anykeep_option "ANYKEEP_PLUGIN_ENABLE_${plugin}")
  set(${_anykeep_option} AUTO CACHE STRING "Build ${plugin} plugin: AUTO (detect), ON (require) or OFF")
  set_property(CACHE ${_anykeep_option} PROPERTY STRINGS AUTO ON OFF)
  anykeep_plugin_mode("${plugin}" _anykeep_mode)

  # An explicit OFF must not even probe this plugin's optional dependencies.
  # Keep an empty target for translation-source discovery on desktop builds.
  if(_anykeep_mode STREQUAL "OFF")
    message(STATUS "PLUGIN ${plugin} explicitly disabled")
    if(NOT ANYKEEP_BUILD_MOBILE)
      add_custom_target(${plugin})
    endif()
    continue()
  endif()

  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${plugin}")
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/${plugin}")
  else()
    message(FATAL_ERROR "Invalid plugins list. Stumbled on ${plugin}")
  endif()
endforeach()
