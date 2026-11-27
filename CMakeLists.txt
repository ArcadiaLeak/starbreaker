cmake_minimum_required(VERSION 3.16)
project(minimal-gui LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Gui)
qt_standard_project_setup()

qt_add_executable(minimal-gui main.cpp)
target_link_libraries(minimal-gui PRIVATE Qt6::Gui)
