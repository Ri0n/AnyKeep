cmake_minimum_required(VERSION 3.20)
project(QSourceHighlite LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT DEFINED QSOURCEHIGHLITE_SOURCE_DIR)
    message(FATAL_ERROR "QSOURCEHIGHLITE_SOURCE_DIR is required")
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui)

add_library(QSourceHighlite STATIC
    "${QSOURCEHIGHLITE_SOURCE_DIR}/languagedata.cpp"
    "${QSOURCEHIGHLITE_SOURCE_DIR}/qsourcehighliter.cpp"
    "${QSOURCEHIGHLITE_SOURCE_DIR}/qsourcehighliterthemes.cpp"
)
target_include_directories(QSourceHighlite PUBLIC
    $<BUILD_INTERFACE:${QSOURCEHIGHLITE_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/QSourceHighlite>
)
target_link_libraries(QSourceHighlite PUBLIC Qt6::Core Qt6::Gui)
target_compile_features(QSourceHighlite PUBLIC cxx_std_17)
set_target_properties(QSourceHighlite PROPERTIES POSITION_INDEPENDENT_CODE ON DEBUG_POSTFIX "")

install(TARGETS QSourceHighlite
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES
    "${QSOURCEHIGHLITE_SOURCE_DIR}/languagedata.h"
    "${QSOURCEHIGHLITE_SOURCE_DIR}/qsourcehighliter.h"
    "${QSOURCEHIGHLITE_SOURCE_DIR}/qsourcehighliterthemes.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/QSourceHighlite"
)

if(EXISTS "${QSOURCEHIGHLITE_SOURCE_DIR}/LICENSE")
    install(FILES "${QSOURCEHIGHLITE_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/QSourceHighlite")
endif()
