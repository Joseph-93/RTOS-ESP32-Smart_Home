# ESP32 Smart Home Web Server

A Django-based web interface for managing ESP32 RTOS smart home devices.

## Features

- 🎨 **Beautiful Dark Theme UI** - Modern, responsive interface
- 🔌 **Multi-Device Support** - Manage multiple ESP32 devices from one dashboard
- 📊 **Real-time Parameter Control** - View and modify all component parameters
- ⚡ **Lightweight Protocol** - Efficient JSON-over-TCP communication
- 🎯 **Action Invocation** - Execute component actions remotely

## Architecture

```
┌─────────────────┐         TCP/JSON          ┌──────────────┐
│  Django Server  │ ◄────────────────────────► │   ESP32      │
│  (This folder)  │      Port 8888             │   Device     │
│                 │                            │              │
│  - Web UI       │                            │  - TCP Server│
│  - JavaScript   │                            │  - Components│
│  - API Endpoints│                            │  - Parameters│
└─────────────────┘                            └──────────────┘
```

## Quick Start

### 1. Install Dependencies

```powershell
cd C:\Code\RTOS-ESP32\webserver
python -m venv venv
.\venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

### 2. Configure ESP32 Devices

Edit `dashboard/apps.py`:

```python
def ready(self):
    from .esp32_client import esp32_manager
    
    # Add your ESP32 devices (IP will be shown on LCD)
    esp32_manager.add_device('esp32-main', '192.168.1.100', 8888)
    # Add more devices as needed
```

### 3. Run Server

```powershell
python manage.py migrate
python manage.py runserver 0.0.0.0:8000
```

Visit: `http://localhost:8000`

## ESP32 Protocol

The ESP32 communicates via JSON over TCP (port 8888). Each command is a JSON object followed by a newline.

### Commands

**Get Components:**
```json
{"cmd": "get_components"}
→ {"components": ["DoorSensor", "MotionSensor", ...]}
```

**Get Parameter Info:**
```json
{"cmd": "get_param_info", "comp": "NetworkActions"}
→ {
    "int_params": [{"name": "retry_count", "rows": 1, "cols": 1}],
    "float_params": [...],
    "bool_params": [...],
    "string_params": [{"name": "http_messages", "rows": 31, "cols": 1}],
    "actions": ["send_http", "connect_websocket"]
}
```

**Get Parameter Value:**
```json
{"cmd": "get_param", "comp": "NetworkActions", "type": "str", "idx": 0, "row": 5, "col": 0}
→ {"value": "{\"name\":\"...\", \"url\":\"...\"}"}
```

**Set Parameter Value:**
```json
{"cmd": "set_param", "comp": "NetworkActions", "type": "int", "idx": 0, "row": 0, "col": 0, "value": 5}
→ {"success": true}
```

**Invoke Action:**
```json
{"cmd": "invoke_action", "comp": "NetworkActions", "action": "send_http"}
→ {"success": true}
```

## Project Structure

```
webserver/
├── esp32_hub/          # Django project settings
│   ├── settings.py
│   ├── urls.py
│   └── asgi.py
├── dashboard/          # Main app
│   ├── views.py        # HTTP request handlers
│   ├── urls.py         # URL routing
│   ├── esp32_client.py # ESP32 TCP communication
│   ├── templates/      # HTML templates
│   └── static/         # CSS/JS assets
├── static/
│   ├── css/style.css   # Dark theme styling
│   └── js/device.js    # Dynamic UI JavaScript
├── manage.py
└── requirements.txt
```

## Why This Architecture?

**Before:** ESP32 tried to generate full HTML pages
- ❌ Limited memory (~7KB free heap)
- ❌ Complex HTML generation
- ❌ Buffer overflows
- ❌ Out of memory errors

**Now:** ESP32 only handles data
- ✅ Simple request/response protocol
- ✅ Minimal memory usage (few KB per request)
- ✅ Django handles all UI complexity
- ✅ Beautiful, responsive interface
- ✅ Can manage multiple ESP32 devices

## Development

The web server runs independently of the ESP32. You can:
- Develop UI without flashing ESP32
- Test with mock data
- Connect to multiple devices
- Add features without memory constraints

## Next Steps

1. Flash the updated ESP32 firmware with TCP server (instead of HTTP)
2. Note the IP address shown on the LCD
3. Add the device to `dashboard/apps.py`
4. Start the Django server
5. Access the beautiful web interface! 🎉
