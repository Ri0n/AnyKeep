cmake_minimum_required(VERSION 3.16)
project(anykeep_nextcloud_live_tests LANGUAGES CXX)

# Build the plugin's real HTTP implementation without pulling in the desktop
# app, QML, QCA, Iris, or other unrelated integration dependencies.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(CTest)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Test)

add_executable(nextcloud_live_test
    nextcloud_live_test.cpp
    ../../plugins/nextcloud/nextclouddto.h
    ../../plugins/nextcloud/nextcloudworker.cpp
    ../../plugins/nextcloud/nextcloudworker.h
)
target_include_directories(nextcloud_live_test PRIVATE ../../plugins/nextcloud)
target_link_libraries(nextcloud_live_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)

add_test(NAME nextcloud_live_test COMMAND nextcloud_live_test)
set_tests_properties(nextcloud_live_test PROPERTIES TIMEOUT 240 LABELS "nextcloud;integration")
