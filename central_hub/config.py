"""
Configuration file for the Central Hub.
Edit this file to set your ESP32 device IPs.
"""

# List of ESP32 device IP addresses to connect to
ESP32_DEVICES = [
    "10.0.0.46",
]

# WebSocket Server settings (for incoming connections from web dashboard)
WS_SERVER_PORT = 80 # Port for the hub's WebSocket server (use 80 if running as root)

# WebSocket Client settings (for outgoing connections to ESP32s)
WS_PING_INTERVAL = 20  # seconds
WS_PING_TIMEOUT = 20   # seconds
RECONNECT_DELAY = 5    # seconds

# Rate limiting (to avoid overwhelming ESP32)
DISCOVERY_DELAY = 0.05   # seconds between param info requests
SUBSCRIBE_DELAY = 0.02   # seconds between subscribe requests

# Logging level: 'DEBUG', 'INFO', 'WARNING', 'ERROR'
LOG_LEVEL = 'INFO'
