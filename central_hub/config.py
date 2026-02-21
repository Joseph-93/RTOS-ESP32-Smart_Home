"""
Configuration file for the Central Hub.
Edit this file to configure ESP32 device discovery.
"""

# mDNS Discovery settings
# If True, automatically discover ESP32 devices via mDNS (_ws._tcp.local.)
# If False, use only the static ESP32_DEVICES list below
USE_MDNS_DISCOVERY = True
MDNS_DISCOVERY_TIMEOUT = 5.0  # seconds to wait for mDNS responses
MDNS_SERVICE_TYPE = "_ws._tcp.local."

# List of ESP32 device IP addresses (used when USE_MDNS_DISCOVERY is False,
# or as additional devices to connect to alongside discovered ones)
ESP32_DEVICES = [
    # "10.0.0.46",  # Add static IPs here if needed
]

# WebSocket Server settings (for incoming connections from web dashboard)
WS_SERVER_PORT = 80 # Port for the hub's WebSocket server (use 80 if running as root)

# WebSocket Client settings (for outgoing connections to ESP32s)
WS_PING_INTERVAL = 20  # seconds
WS_PING_TIMEOUT = 20   # seconds
RECONNECT_DELAY = 5    # seconds

# Rate limiting (to avoid overwhelming ESP32)
DISCOVERY_DELAY = 0.15   # seconds between param info requests
SUBSCRIBE_DELAY = 0.2    # seconds between subscribe requests (higher to prevent memory exhaustion)

# Logging level: 'DEBUG', 'INFO', 'WARNING', 'ERROR'
LOG_LEVEL = 'INFO'
