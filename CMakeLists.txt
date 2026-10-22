cmake_minimum_required(VERSION 3.21)

project(berry-kiosk
    VERSION 0.1.0
    DESCRIPTION "Bilderrahmen und Kiosk-Anzeige für den Raspberry Pi"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BERRY_BUILD_TESTS "Build the unit tests" ON)

# Qt 6.5 is the oldest version with qt_add_qml_module/loadFromModule as used here.
# Raspberry Pi OS based on Debian 13 (trixie) ships Qt 6.8.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick)
qt_standard_project_setup(REQUIRES 6.5)

add_subdirectory(core)
add_subdirectory(display)

if(BERRY_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
