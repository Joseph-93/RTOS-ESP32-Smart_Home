# Build required components list based on configuration
# Base components always required
set(REQUIRED_COMPONENTS common web_server wifi)

# Debug: show what's enabled
message(STATUS "ENABLE_HEARTBEAT = ${ENABLE_HEARTBEAT}")
message(STATUS "ENABLE_RGB_LED = ${ENABLE_RGB_LED}")
message(STATUS "ENABLE_GUI = ${ENABLE_GUI}")

# For now, include ALL optional components unconditionally in REQUIRES
# The #ifdef guards in main.cpp will exclude unused code from compilation
# This avoids CMake cache issues with conditional component lists
list(APPEND REQUIRED_COMPONENTS heartbeat rgb_led)

# Only add these if they exist and are explicitly enabled
if(ENABLE_GUI)
    list(APPEND REQUIRED_COMPONENTS gui)
endif()
if(ENABLE_LIGHT_SENSOR)
    list(APPEND REQUIRED_COMPONENTS light_sensor)
endif()
if(ENABLE_MOTION_SENSOR)
    list(APPEND REQUIRED_COMPONENTS motion_sensor)
endif()
if(ENABLE_DOOR_SENSOR)
    list(APPEND REQUIRED_COMPONENTS door_sensor)
endif()

message(STATUS "Enabled components: ${REQUIRED_COMPONENTS}")

idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES ${REQUIRED_COMPONENTS})

