# Component Selection Configuration
# Copy this file to components_config.cmake and customize
#
# Set each component to ON or OFF to include/exclude from build
# Excluding unused components saves flash space and RAM

# Core components (always required)
# - common: Base component system
# - wifi: Network connectivity  
# - web_server: WebSocket API for remote control

# Optional sensor/actuator components
set(ENABLE_GUI OFF CACHE BOOL "Enable LCD/Touch GUI component")
set(ENABLE_HEARTBEAT ON CACHE BOOL "Enable heartbeat LED indicator")
set(ENABLE_LIGHT_SENSOR OFF CACHE BOOL "Enable ambient light sensor")
set(ENABLE_MOTION_SENSOR OFF CACHE BOOL "Enable PIR motion sensor")
set(ENABLE_DOOR_SENSOR OFF CACHE BOOL "Enable magnetic door sensor")
set(ENABLE_RGB_LED ON CACHE BOOL "Enable WS2812 RGB LED strip")

# Memory optimization for RGB LED animations
# When GUI/sensors disabled, more RAM available for animation frames
# Approximate free RAM with all sensors OFF: ~200KB
# Each animation frame: (led_count * 3) + 2 bytes
