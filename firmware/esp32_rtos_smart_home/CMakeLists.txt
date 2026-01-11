# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Enable debug logging
add_compile_definitions(DEBUG)

# Add common directory to component search path
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_rtos_smart_home)
