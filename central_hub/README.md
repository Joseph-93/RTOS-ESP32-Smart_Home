# ESP32 Central Hub System

A Python-based central hub that connects to multiple ESP32 devices, subscribes to all their parameters, and maintains a complete mirror of the system state.

**Includes built-in control components:**
- **NetworkActions** - Send configurable network messages (UDP, TCP, HTTP, HTTPS, WS, WSS)
- **ActionManager** - Queue and execute timed actions across devices  
- **Watcher** - Monitor variables and trigger actions on expression changes

## Features

- Connects to multiple ESP32 devices simultaneously via WebSocket
- Auto-discovers all components and parameters on each device
- Subscribes to every parameter for real-time updates
- Maintains a hierarchical state mirror: `ESP32 -> Component -> Parameter`
- Auto-reconnects on connection loss
- Prints all parameter changes in real-time
- **Local component system** for control logic (runs on the hub itself)

## Requirements

- Python 3.8+
- websockets library
- aiohttp library

## Installation

```bash
cd central_hub
pip install -r requirements.txt
```

## Usage

### Configure ESP32 Addresses

Edit `config.py` and update the `ESP32_DEVICES` list:

```python
ESP32_DEVICES = [
    "192.168.1.100",
    "192.168.1.101",
    # Add more IPs as needed
]
```

Or pass via command line:

```bash
python central_hub.py 192.168.1.100 192.168.1.101
```

Or set via environment variable:

```bash
export ESP32_IPS="192.168.1.100,192.168.1.101,192.168.1.102"
python central_hub.py
```

### Run the Hub

```bash
python central_hub.py
```

## Output

The hub will print all parameter changes in real-time:

```
2026-01-31 10:30:45 [INFO] CentralHub: Starting Central Hub with 2 ESP32 devices
2026-01-31 10:30:45 [INFO] CentralHub: [192.168.1.100] Connecting to ws://192.168.1.100/ws...
2026-01-31 10:30:45 [INFO] CentralHub: [192.168.1.100] Connected!
2026-01-31 10:30:46 [INFO] CentralHub: [192.168.1.100] Found 4 components: ['gui', 'heartbeat', 'light_sensor', 'motion_sensor']
2026-01-31 10:30:47 [INFO] CentralHub: [192.168.1.100] Discovery complete: 4 components, 15 parameters
2026-01-31 10:30:48 [INFO] CentralHub: [192.168.1.100] Subscribed to 15 parameter cells

[10:30:49.123] 192.168.1.100 / heartbeat / pulse[0][0]: False -> True
[10:30:49.273] 192.168.1.100 / heartbeat / pulse[0][0]: True -> False
[10:30:50.145] 192.168.1.100 / light_sensor / light_level[0][0]: 512 -> 523
[10:30:51.234] 192.168.1.100 / gui / button_0_pressed[0][0]: False -> True
[10:30:51.384] 192.168.1.100 / gui / button_0_pressed[0][0]: True -> False
```

## Data Structure

The hub maintains state in this hierarchy:

```
CentralHub
└── devices (Dict[ip, ESP32Device])
    └── ESP32Device
        ├── ip: str
        ├── connected: bool
        └── components (Dict[name, Component])
            └── Component
                ├── name: str
                └── parameters (Dict[name, Parameter])
                    └── Parameter
                        ├── param_id: int
                        ├── name: str
                        ├── param_type: str
                        ├── read_only: bool
                        └── values: Dict[(row, col), value]
```

## API

### CentralHub

- `start()` - Start connecting to all ESP32s
- `stop()` - Disconnect from all devices
- `get_state_snapshot()` - Get full state as dict
- `print_state()` - Print formatted state to console

### Accessing State

```python
hub = CentralHub(ips)
await hub.start()

# Get a specific value from a remote ESP32
device = hub.devices['192.168.1.100']
component = device.components['light_sensor']
param = component.parameters['light_level']
value = param.get_value(0, 0)

# Get full snapshot
snapshot = hub.get_state_snapshot()
```

## Local Components

The hub includes three local components for control logic:

### NetworkActions

Send configurable network messages. Supports 100 message slots.

```python
# Configure a message slot
hub.network_actions.set_message_config(0, {
    "protocol": "HTTP",
    "host": "192.168.1.200",
    "port": 80,
    "path": "/api/control",
    "method": "POST",
    "body": {"command": "on"},
    "await_response": True,
    "timeout_ms": 5000
})

# Trigger it
await hub.network_actions.send_message(0)

# Or via parameter
hub.network_actions.trigger.set_value(0, 0, 0)  # Triggers message 0
```

Supported protocols: `UDP`, `TCP`, `HTTP`, `HTTPS`, `WS`, `WSS`

### ActionManager

Queue and execute timed actions across devices.

```python
# Queue an action to set a parameter on a remote device
hub.action_manager.queue_action(
    device="192.168.1.100",
    param_id=5,
    row=0, col=0,
    value=100,
    wait_after_ms=1000  # Wait 1 second before next action
)

# Add device nicknames for easier targeting
hub.action_manager.add_nickname("living_room", "192.168.1.100")
hub.action_manager.add_nickname("kitchen", "192.168.1.101")

# Queue action using nickname
hub.action_manager.queue_action(
    device="living_room",
    component="gui",
    param="brightness",
    value=75
)
```

### Watcher

Monitor variables and trigger actions when expressions change.

```python
# Define variables to watch
hub.watcher.set_variable(
    name="light_level",
    device="192.168.1.100",
    component="light_sensor",
    param="current_light_level"
)

hub.watcher.set_variable(
    name="motion",
    device="192.168.1.100", 
    component="motion_sensor",
    param="motion_detected"
)

# Set up a watch expression with rising/falling edge actions
hub.watcher.set_watch(
    slot=0,
    expression="light_level < 30 and motion == true",
    rising_actions=[  # When expression becomes True
        {"device": "living_room", "param_id": 10, "value": 100}
    ],
    falling_actions=[  # When expression becomes False
        {"device": "living_room", "param_id": 10, "value": 0}
    ]
)
```

Supported expression operators: `and`, `or`, `not`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `+`, `-`, `*`, `/`

## Data Structure

The hub maintains state in this hierarchy:

```
CentralHub
├── local_components (Dict[name, Component])  # Local control components
│   ├── NetworkActions
│   ├── ActionManager
│   └── Watcher
├── remote_state_cache (Dict[ip, Dict[key, value]])  # Cached values for Watcher
└── devices (Dict[ip, ESP32Device])
    └── ESP32Device
        ├── ip: str
        ├── connected: bool
        └── components (Dict[name, Component])
            └── Component
                ├── name: str
                └── parameters (Dict[name, Parameter])
                    └── Parameter
                        ├── param_id: int
                        ├── name: str
                        ├── param_type: str
                        ├── read_only: bool
                        └── values: Dict[(row, col), value]
```

## Future Extensions

- WebSocket server to expose local components (same API as ESP32)
- MQTT publishing for Home Assistant integration
- REST API for external access
- State persistence to database
- Web dashboard
