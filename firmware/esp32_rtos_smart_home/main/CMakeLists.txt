# Build required components list based on configuration
# Base components always required
set(REQUIRED_COMPONENTS common web_server wifi ota)

# Debug: show what's enabled
message(STATUS "ENABLE_HEARTBEAT = ${ENABLE_HEARTBEAT}")
message(STATUS "ENABLE_RGB_LED = ${ENABLE_RGB_LED}")
message(STATUS "ENABLE_GUI = ${ENABLE_GUI}")

# All optional components are always included in REQUIRES.
# The #ifdef guards in main.cpp exclude unused code from compilation.
# This avoids CMake cache issues with conditional component lists on fresh builds.
# To enable/disable components, edit components_config.cmake.
list(APPEND REQUIRED_COMPONENTS heartbeat rgb_led touch_sensor gui light_sensor motion_sensor door_sensor)

message(STATUS "Enabled components: ${REQUIRED_COMPONENTS}")

idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES ${REQUIRED_COMPONENTS})
