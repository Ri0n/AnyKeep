include_guard(GLOBAL)

function(anykeep_install_prebuilt_sdk_runtime)
  if(NOT ANYKEEP_QCA_SDK_ROOT OR NOT ANYKEEP_IRIS_SDK_ROOT)
    message(FATAL_ERROR
            "ANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME requires both ANYKEEP_QCA_SDK_ROOT and ANYKEEP_IRIS_SDK_ROOT")
  endif()

  if(WIN32)
    file(GLOB _anykeep_qca_runtime CONFIGURE_DEPENDS "${ANYKEEP_QCA_SDK_ROOT}/bin/*.dll")
    file(GLOB _anykeep_qca_plugins CONFIGURE_DEPENDS "${ANYKEEP_QCA_SDK_ROOT}/lib/qca3-qt6/crypto/*.dll")
    file(GLOB _anykeep_iris_runtime CONFIGURE_DEPENDS "${ANYKEEP_IRIS_SDK_ROOT}/bin/*iris*.dll")
    if(NOT _anykeep_qca_runtime)
      message(FATAL_ERROR "No QCA runtime DLLs found under ${ANYKEEP_QCA_SDK_ROOT}/bin")
    endif()
    if(NOT _anykeep_qca_plugins)
      message(FATAL_ERROR "No QCA crypto plugins found under ${ANYKEEP_QCA_SDK_ROOT}/lib/qca3-qt6/crypto")
    endif()
    if(NOT _anykeep_iris_runtime)
      message(FATAL_ERROR "No Iris runtime DLL found under ${ANYKEEP_IRIS_SDK_ROOT}/bin")
    endif()
    install(FILES ${_anykeep_qca_runtime} ${_anykeep_iris_runtime} DESTINATION "." COMPONENT Libraries)
    install(FILES ${_anykeep_qca_plugins} DESTINATION "crypto" COMPONENT Libraries)
  elseif(APPLE)
    # macOS packaging relocates these libraries into AnyKeep.app/Contents/Frameworks
    # after installation so install_name_tool/macdeployqt can rewrite their paths.
    message(STATUS "Prebuilt QCA/Iris runtime staging for macOS is handled by the package workflow")
  elseif(ANDROID)
    message(STATUS "Prebuilt QCA/Iris Android runtime staging is attached to the mobile target")
  endif()
endfunction()

function(anykeep_configure_prebuilt_sdk_runtime_target target)
  if(NOT ANDROID OR NOT ANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME)
    return()
  endif()
  if(NOT TARGET ${target})
    message(FATAL_ERROR "Cannot attach prebuilt SDK runtime to missing target: ${target}")
  endif()

  file(GLOB _anykeep_qca_plugins CONFIGURE_DEPENDS "${ANYKEEP_QCA_SDK_ROOT}/lib/qca3-qt6/crypto/*.so")
  if(NOT _anykeep_qca_plugins)
    message(FATAL_ERROR "No Android QCA crypto plugins found in ${ANYKEEP_QCA_SDK_ROOT}/lib/qca3-qt6/crypto")
  endif()
  if(NOT CMAKE_ANDROID_ARCH_ABI)
    message(FATAL_ERROR "CMAKE_ANDROID_ARCH_ABI is required to package prebuilt QCA Android plugins")
  endif()

  # Android cannot preserve QCA's qca3-qt6/crypto subdirectory in the native
  # library directory. Stage providers with Qt's Android plugin filename
  # convention; androiddeployqt then copies them to the ABI directory, where
  # QCA's Android plugin scan can identify the "crypto" prefix.
  set(_anykeep_qca_plugin_root "${CMAKE_BINARY_DIR}/_deps/qca-android-plugins/plugins/qca")
  file(MAKE_DIRECTORY "${_anykeep_qca_plugin_root}")
  foreach(_anykeep_qca_plugin IN LISTS _anykeep_qca_plugins)
    get_filename_component(_anykeep_qca_plugin_name "${_anykeep_qca_plugin}" NAME_WE)
    string(REGEX REPLACE "^lib" "" _anykeep_qca_plugin_name "${_anykeep_qca_plugin_name}")
    string(REPLACE "-" "_" _anykeep_qca_plugin_name "${_anykeep_qca_plugin_name}")
    set(_anykeep_qca_android_name
        "libplugins_crypto_${_anykeep_qca_plugin_name}_${CMAKE_ANDROID_ARCH_ABI}.so")
    configure_file("${_anykeep_qca_plugin}"
                   "${_anykeep_qca_plugin_root}/${_anykeep_qca_android_name}" COPYONLY)
  endforeach()

  get_target_property(_anykeep_android_extra_plugins ${target} QT_ANDROID_EXTRA_PLUGINS)
  if(NOT _anykeep_android_extra_plugins
     OR _anykeep_android_extra_plugins STREQUAL "_anykeep_android_extra_plugins-NOTFOUND")
    set(_anykeep_android_extra_plugins "")
  endif()
  list(APPEND _anykeep_android_extra_plugins "${_anykeep_qca_plugin_root}")
  list(REMOVE_DUPLICATES _anykeep_android_extra_plugins)
  set_property(TARGET ${target} PROPERTY QT_ANDROID_EXTRA_PLUGINS "${_anykeep_android_extra_plugins}")
endfunction()
