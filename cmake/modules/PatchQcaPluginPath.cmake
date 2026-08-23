if(NOT DEFINED QCA_CMAKE_FILE OR NOT DEFINED QCA_RUNTIME_PLUGIN_PATH)
  message(FATAL_ERROR "QCA_CMAKE_FILE and QCA_RUNTIME_PLUGIN_PATH are required")
endif()

file(READ "${QCA_CMAKE_FILE}" _qca_cmake)
set(_qca_runtime_definition "add_definitions(-DQCA_PLUGIN_PATH=\"${QCA_RUNTIME_PLUGIN_PATH}\")")
set(_qca_relative_definition [[add_definitions(-DQCA_PLUGIN_PATH="${QCA_PREFIX_INSTALL_DIR}/${QCA_PLUGINS_INSTALL_DIR}")]])
set(_qca_absolute_definition [[add_definitions(-DQCA_PLUGIN_PATH="${QCA_PLUGINS_INSTALL_DIR}")]])

string(REPLACE "${_qca_relative_definition}" "${_qca_runtime_definition}" _qca_cmake "${_qca_cmake}")
string(REPLACE "${_qca_absolute_definition}" "${_qca_runtime_definition}" _qca_cmake "${_qca_cmake}")
file(WRITE "${QCA_CMAKE_FILE}" "${_qca_cmake}")
