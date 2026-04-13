# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Enable debug logging
# add_compile_definitions(DEBUG)

# Load component configuration (copy components_config.example.cmake to components_config.cmake)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/components_config.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/components_config.cmake")
    message(STATUS "Loaded components_config.cmake")
else()
    message(STATUS "No components_config.cmake found, using defaults (all components enabled)")
    set(ENABLE_GUI ON)
    set(ENABLE_HEARTBEAT ON)
    set(ENABLE_LIGHT_SENSOR ON)
    set(ENABLE_MOTION_SENSOR ON)
    set(ENABLE_DOOR_SENSOR ON)
    set(ENABLE_RGB_LED ON)
    set(ENABLE_TOUCH_SENSOR ON)
endif()

# Pass component enables as compile definitions
if(ENABLE_GUI)
    add_compile_definitions(ENABLE_GUI)
endif()
if(ENABLE_HEARTBEAT)
    add_compile_definitions(ENABLE_HEARTBEAT)
endif()
if(ENABLE_LIGHT_SENSOR)
    add_compile_definitions(ENABLE_LIGHT_SENSOR)
endif()
if(ENABLE_MOTION_SENSOR)
    add_compile_definitions(ENABLE_MOTION_SENSOR)
endif()
if(ENABLE_DOOR_SENSOR)
    add_compile_definitions(ENABLE_DOOR_SENSOR)
endif()
if(ENABLE_RGB_LED)
    add_compile_definitions(ENABLE_RGB_LED)
endif()
if(ENABLE_TOUCH_SENSOR)
    add_compile_definitions(ENABLE_TOUCH_SENSOR)
endif()
if(ENABLE_STEPPER_MOTOR)
    add_compile_definitions(ENABLE_STEPPER_MOTOR)
endif()

# Add common directory to component search path
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/common")

# ---------------------------------------------------------------------------
# Patch LVGL: remove circular "REQUIRES main" that breaks link order in IDF v5.x
# Handles incremental builds. For post-fullclean builds, use build.ps1 which
# runs idf.py reconfigure first (downloads LVGL), patches it, then builds.
# ---------------------------------------------------------------------------
set(_LVGL_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/lvgl__lvgl/CMakeLists.txt")
if(EXISTS "${_LVGL_CMAKE}")
    file(READ "${_LVGL_CMAKE}" _LVGL_CONTENT)
    if("${_LVGL_CONTENT}" MATCHES "REQUIRES main")
        string(REPLACE "REQUIRES main" "" _LVGL_PATCHED "${_LVGL_CONTENT}")
        file(WRITE "${_LVGL_CMAKE}" "${_LVGL_PATCHED}")
        message(STATUS "Patched lvgl__lvgl: removed circular 'REQUIRES main'")
    endif()
endif()
unset(_LVGL_CMAKE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_rtos_smart_home)

