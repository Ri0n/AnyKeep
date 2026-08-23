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

  # Iris and QCA themselves are linked through imported CMake targets and are
  # therefore discovered by Qt's Android deployment machinery. QCA providers
  # are loaded dynamically, so declare them explicitly.
  get_target_property(_anykeep_android_extra_libs ${target} QT_ANDROID_EXTRA_LIBS)
  if(NOT _anykeep_android_extra_libs OR _anykeep_android_extra_libs STREQUAL "_anykeep_android_extra_libs-NOTFOUND")
    set(_anykeep_android_extra_libs "")
  endif()
  list(APPEND _anykeep_android_extra_libs ${_anykeep_qca_plugins})
  list(REMOVE_DUPLICATES _anykeep_android_extra_libs)
  set_property(TARGET ${target} PROPERTY QT_ANDROID_EXTRA_LIBS "${_anykeep_android_extra_libs}")
endfunction()
