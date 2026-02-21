# ESP32 Smart Home Web Dashboard

A Django-based web interface for managing ESP32 RTOS smart home devices.

## Features

- 🎨 **Beautiful Dark Theme UI** - Modern, responsive interface
- 🔌 **Multi-Device Support** - Manage multiple ESP32 devices from one dashboard
- 📊 **Real-time Parameter Control** - View and modify all component parameters via WebSocket
- ⚡ **Live Updates** - Subscribe to parameters for push notifications
- 🎯 **Action Invocation** - Execute component actions remotely

## Architecture

```
┌─────────────────┐                            ┌──────────────┐
│  Django Server  │                            │   ESP32      │
│  (Port 8000)    │                            │   Device     │
│                 │                            │  (Port 80)   │
│  - HTML/CSS/JS  │                            │              │
│  - Device list  │                            │  - WebSocket │
└────────┬────────┘                            │    /ws       │
         │                                     │  - Components│
         │ serves static files                 │  - Parameters│
         ▼                                     └──────▲───────┘
┌─────────────────┐                                   │
│     Browser     │ ─────── WebSocket ────────────────┘
│                 │      (direct to ESP32)
│  - device.js    │
│  - websocket.js │
└─────────────────┘
```

**Key Point:** The browser connects **directly** to the ESP32 via WebSocket.
Django just serves the HTML/JS files and stores the device list.

## Quick Start

### 1. Install Dependencies

```powershell
cd C:\Code\RTOS-ESP32\webserver
python -m venv venv
.\venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

### 2. Run Server

```powershell
python manage.py migrate
python manage.py runserver 0.0.0.0:8000
```

Visit: `http://localhost:8000`

### 3. Add ESP32 Devices

Use the web UI to add devices by entering their IP address.
The browser will connect directly to the ESP32 WebSocket.

## ESP32 WebSocket Protocol

The browser communicates directly with ESP32 via WebSocket at `ws://<esp32-ip>/ws`.

### Commands (sent from browser)

**Get Components:**
```json
{"type": "get_components", "id": 1}
→ {"id": 1, "components": [{"name": "LightSensor", "id": 0}, ...]}
```

**Get Component Parameters:**
```json
{"type": "get_component_params", "comp": "LightSensor", "id": 2}
→ {"id": 2, "int_params": [...], "float_params": [...], ...}
```

**Get Parameter Value:**
```json
{"type": "get_param", "param_id": 5, "row": 0, "col": 0, "id": 3}
→ {"id": 3, "value": 512}
```

**Set Parameter Value:**
```json
{"type": "set_param", "param_id": 5, "row": 0, "col": 0, "value": 100, "id": 4}
→ {"id": 4, "success": true}
```

**Subscribe to Updates:**
```json
{"type": "subscribe", "param_id": 5, "row": 0, "col": 0, "id": 5}
→ {"id": 5, "success": true}
// Later, push updates arrive:
→ {"type": "update", "param_id": 5, "row": 0, "col": 0, "value": 523}
```

## Project Structure

```
webserver/
├── esp32_hub/              # Django project settings
│   ├── settings.py
│   ├── urls.py
│   └── asgi.py
├── dashboard/              # Main app
│   ├── views.py            # Page views (serves templates)
│   ├── urls.py             # URL routing
│   ├── consumers.py        # Django Channels WebSocket (for future use)
│   └── templates/          # HTML templates
│       └── dashboard/
│           ├── index.html       # Device list
│           ├── device.html      # Component browser
│           └── component.html   # Parameter editor
├── static/
│   ├── css/style.css       # Dark theme styling
│   └── js/
│       ├── websocket.js    # ESP32 WebSocket client
│       ├── device.js       # Device/component UI
│       └── component.js    # Parameter controls
├── manage.py
└── requirements.txt
```

## Why Direct WebSocket?

**Django doesn't proxy to ESP32** - the browser talks directly:

- ✅ **Lower latency** - No middleman
- ✅ **Real-time subscriptions** - Push updates from ESP32
- ✅ **Simpler server** - Django just serves static files
- ✅ **Works offline** - Once loaded, only needs ESP32

## Related Projects

- **firmware/** - ESP32 firmware with WebSocket server
- **central_hub/** - Python automation hub (separate from web UI)
  - Monitors all ESP32s
  - Automation logic (Watcher, ActionManager)
  - Also uses WebSocket to ESP32

## Usage Tips

1. **mDNS Discovery**: ESP32 devices advertise as `esp32-*.local`
2. **Multiple Devices**: Add as many ESP32s as you want
3. **Real-time**: Use subscribe to get live updates
4. **Triggers**: Some components have trigger parameters (buttons/actions)
