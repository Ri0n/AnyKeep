cmake_minimum_required(VERSION 3.22)

add_library(
  QSourceHighlite STATIC
  languagedata.cpp
  qsourcehighliter.cpp
  qsourcehighliterthemes.cpp)
add_library(QSourceHighlite::QSourceHighlite ALIAS QSourceHighlite)

target_include_directories(QSourceHighlite PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(QSourceHighlite PUBLIC Qt6::Core Qt6::Gui)
target_compile_features(QSourceHighlite PUBLIC cxx_std_17)
set_target_properties(QSourceHighlite PROPERTIES POSITION_INDEPENDENT_CODE ON)
