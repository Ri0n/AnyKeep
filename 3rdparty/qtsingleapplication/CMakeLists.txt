cmake_minimum_required(VERSION 3.10)

project(QtSingleApplication)

option(QT_SINGLE_APPLICATION_INSTALL
       "Disable installation of QtSingleApplication" ON)
option(QT_SINGLE_APPLICATION_WIDGET
       "Enable QApplication (Widget-based Application)" ON)

set(QT_COMPONENTS Core Gui Network)
if(QT_SINGLE_APPLICATION_WIDGET)
  list(APPEND QT_COMPONENTS Widgets)
endif()

find_package(
  Qt6
  COMPONENTS ${QT_COMPONENTS}
  REQUIRED)

set(SOURCES qtsinglecoreapplication.cpp qtlocalpeer.cpp)

set(HEADERS qtsinglecoreapplication.h qtlocalpeer.h)

if(QT_SINGLE_APPLICATION_WIDGET)
  list(APPEND SOURCES qtsingleapplication.cpp)
  list(APPEND HEADERS qtsingleapplication.h)
endif()

add_library(QtSingleApplication STATIC ${SOURCES} ${HEADERS})
target_include_directories(QtSingleApplication
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(QT_SINGLE_APPLICATION_INSTALL)
  install(TARGETS QtSingleApplication DESTINATION lib)
  install(FILES ${HEADERS} DESTINATION include/QtSingleApplication)
endif()

target_link_libraries(QtSingleApplication PRIVATE Qt6::Core Qt6::Gui
                                                  Qt6::Network)
if(QT_SINGLE_APPLICATION_WIDGET)
  target_link_libraries(QtSingleApplication PRIVATE Qt6::Widgets)
endif()
