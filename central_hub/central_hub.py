"""
ESP32 Central Hub System
Connects to multiple ESP32 devices, subscribes to all parameters,
and maintains a mirrored state of the entire system.

Includes local components for control logic:
- NetworkActions: Send network messages (UDP, TCP, HTTP, WS)
- ActionManager: Queue and execute timed actions
- Watcher: Monitor variables and trigger actions on expression changes
- WebServer: WebSocket server exposing local components (same API as ESP32s)

Designed to run on a Raspberry Pi (or any Python 3.8+ environment).
"""

import asyncio
import json
import logging
import os
import socket
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Any
from datetime import datetime

import websockets
from websockets.client import WebSocketClientProtocol

from config import (
    ESP32_DEVICES, WS_PING_INTERVAL, WS_PING_TIMEOUT, 
    RECONNECT_DELAY, DISCOVERY_DELAY, SUBSCRIBE_DELAY, LOG_LEVEL,
    WS_SERVER_PORT
)
from components import (
    Component as BaseComponent,
    NetworkActionsComponent,
    ActionManagerComponent, 
    WatcherComponent,
    WebServerComponent
)

# Configure logging
logging.basicConfig(
    level=getattr(logging, LOG_LEVEL),
    format='%(asctime)s [%(levelname)s] %(name)s: %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger('CentralHub')


@dataclass
class Parameter:
    """Represents a single parameter from an ESP32 component."""
    param_id: int
    name: str
    param_type: str  # 'int', 'float', 'bool', 'str'
    rows: int
    cols: int
    read_only: bool
    min_val: Optional[float] = None
    max_val: Optional[float] = None
    values: Dict[tuple, Any] = field(default_factory=dict)  # (row, col) -> value
    last_updated: Optional[datetime] = None
    
    def set_value(self, row: int, col: int, value: Any):
        """Update a value and timestamp."""
        self.values[(row, col)] = value
        self.last_updated = datetime.now()
    
    def get_value(self, row: int = 0, col: int = 0) -> Any:
        """Get a value, defaulting to (0,0)."""
        return self.values.get((row, col))


@dataclass
class Component:
    """Represents a component on an ESP32 device."""
    name: str
    parameters: Dict[str, Parameter] = field(default_factory=dict)
    params_by_id: Dict[int, Parameter] = field(default_factory=dict)
    
    def add_parameter(self, param: Parameter):
        """Add a parameter to this component."""
        self.parameters[param.name] = param
        self.params_by_id[param.param_id] = param
    
    def get_param_by_id(self, param_id: int) -> Optional[Parameter]:
        """Look up parameter by ID."""
        return self.params_by_id.get(param_id)


@dataclass 
class ESP32Device:
    """Represents a connected ESP32 device."""
    ip: str
    name: str = ""
    components: Dict[str, Component] = field(default_factory=dict)
    connected: bool = False
    websocket: Optional[WebSocketClientProtocol] = None
    message_id: int = 0
    pending_requests: Dict[int, asyncio.Future] = field(default_factory=dict)
    
    def get_param_by_id(self, param_id: int) -> Optional[tuple]:
        """Find parameter by ID across all components. Returns (component, param) or None."""
        for comp in self.components.values():
            param = comp.get_param_by_id(param_id)
            if param:
                return (comp, param)
        return None


class CentralHub:
    """
    Central hub that connects to multiple ESP32 devices and maintains
    a complete mirror of their state.
    
    Also hosts local components for control logic (NetworkActions, 
    ActionManager, Watcher, WebServer).
    """
    
    def __init__(self, esp32_ips: List[str], ws_port: int = 8080):
        self.esp32_ips = esp32_ips
        self.ws_port = ws_port
        self.devices: Dict[str, ESP32Device] = {}  # ip -> device
        self.running = False
        
        # Local components for control logic
        self.local_components: Dict[str, BaseComponent] = {}
        
        # Cache for remote parameter values (used by Watcher)
        # Format: { "ip": { "Component.param[row,col]": value } }
        self.remote_state_cache: Dict[str, Dict[str, Any]] = {}
        
        # Initialize local components
        self._init_local_components()
    
    def _init_local_components(self):
        """Initialize the local control components."""
        # Create components
        self.network_actions = NetworkActionsComponent()
        self.action_manager = ActionManagerComponent()
        self.watcher = WatcherComponent()
        self.web_server = WebServerComponent(port=self.ws_port)
        
        # Register them
        self.local_components['NetworkActions'] = self.network_actions
        self.local_components['ActionManager'] = self.action_manager
        self.local_components['Watcher'] = self.watcher
        self.local_components['WebServer'] = self.web_server
        
        # Set hub reference on each component
        for comp in self.local_components.values():
            comp.hub = self
        
        # Share nickname map between components
        # When ActionManager updates nicknames, Watcher should see them too
        self.watcher.set_nickname_map(self.action_manager._nickname_map)
    
    def _get_local_ip(self) -> str:
        """Get the local IP address of this machine."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return "127.0.0.1"
        
    async def start(self):
        """Start the central hub - connect to all devices."""
        local_ip = self._get_local_ip()
        logger.info(f"Starting Central Hub at {local_ip}")
        logger.info(f"Configured to connect to {len(self.esp32_ips)} ESP32 devices")
        self.running = True
        
        # Initialize all local components
        for comp in self.local_components.values():
            await comp.initialize()
        
        # Start local component background tasks
        await self.action_manager.start()
        await self.watcher.start()
        await self.web_server.start()
        
        logger.info("Local components initialized and started")
        
        # Create tasks for each ESP32 connection
        tasks = [self._manage_device(ip) for ip in self.esp32_ips]
        
        # Also run a keep-alive task so the hub doesn't exit if no ESP32s configured
        async def keep_alive():
            while self.running:
                await asyncio.sleep(1)
        
        tasks.append(keep_alive())
        
        await asyncio.gather(*tasks, return_exceptions=True)
    
    async def stop(self):
        """Stop the central hub."""
        logger.info("Stopping Central Hub")
        self.running = False
        
        # Stop local component background tasks
        await self.web_server.stop()
        await self.watcher.stop()
        await self.action_manager.stop()
        
        # Close all connections
        for device in self.devices.values():
            if device.websocket:
                await device.websocket.close()
    
    async def _manage_device(self, ip: str):
        """Manage connection to a single ESP32 device with auto-reconnect."""
        device = ESP32Device(ip=ip)
        self.devices[ip] = device
        
        while self.running:
            try:
                await self._connect_and_subscribe(device)
            except Exception as e:
                import traceback
                logger.error(f"[{ip}] Connection error: {type(e).__name__}: {e}")
                logger.debug(traceback.format_exc())
                device.connected = False
                
            if self.running:
                logger.info(f"[{ip}] Reconnecting in {RECONNECT_DELAY} seconds...")
                await asyncio.sleep(RECONNECT_DELAY)
    
    async def _connect_and_subscribe(self, device: ESP32Device):
        """Connect to device, discover components/params, subscribe to all."""
        ip = device.ip
        uri = f"ws://{ip}/ws"
        
        logger.info(f"[{ip}] Connecting to {uri}...")
        
        async with websockets.connect(uri, ping_interval=WS_PING_INTERVAL, ping_timeout=WS_PING_TIMEOUT) as ws:
            device.websocket = ws
            device.connected = True
            logger.info(f"[{ip}] Connected!")
            
            # Start listener task FIRST so we can receive responses
            listener_task = asyncio.create_task(self._listen_for_updates(device))
            
            try:
                # Discover all components and parameters
                await self._discover_device(device)
                
                # Subscribe to all parameters
                await self._subscribe_all(device)
                
                # Wait for listener to complete (will run until disconnect)
                await listener_task
            except Exception as e:
                listener_task.cancel()
                raise
    
    async def _send_request(self, device: ESP32Device, message: dict, timeout: float = 10.0) -> dict:
        """Send a request and wait for response."""
        if not device.websocket:
            raise ConnectionError("Not connected")
        
        # Assign message ID
        msg_id = device.message_id
        device.message_id += 1
        message['id'] = msg_id
        
        # Create future for response
        future = asyncio.get_running_loop().create_future()
        device.pending_requests[msg_id] = future
        
        try:
            msg_str = json.dumps(message)
            logger.debug(f"[{device.ip}] Sending: {msg_str}")
            await device.websocket.send(msg_str)
            response = await asyncio.wait_for(future, timeout=timeout)
            logger.debug(f"[{device.ip}] Received: {response}")
            return response
        except asyncio.TimeoutError:
            logger.error(f"[{device.ip}] Request timeout for: {message}")
            raise
        finally:
            device.pending_requests.pop(msg_id, None)
    
    async def _discover_device(self, device: ESP32Device):
        """Discover all components and parameters on a device."""
        ip = device.ip
        logger.info(f"[{ip}] Discovering components...")
        
        # Get list of components
        response = await self._send_request(device, {'type': 'get_components'})
        components_raw = response.get('components', [])
        
        # Components can be dicts {'name': 'X', 'id': Y} or just strings
        component_names = []
        for c in components_raw:
            if isinstance(c, dict):
                component_names.append(c['name'])
            else:
                component_names.append(c)
        
        logger.info(f"[{ip}] Found {len(component_names)} components: {component_names}")
        
        for comp_name in component_names:
            component = Component(name=comp_name)
            device.components[comp_name] = component
            
            # Discover parameters for each type
            for param_type in ['int', 'float', 'bool', 'str']:
                await self._discover_params_of_type(device, component, param_type)
                await asyncio.sleep(DISCOVERY_DELAY)  # Small delay to not overwhelm ESP32
        
        # Log summary
        total_params = sum(len(c.parameters) for c in device.components.values())
        logger.info(f"[{ip}] Discovery complete: {len(device.components)} components, {total_params} parameters")
    
    async def _discover_params_of_type(self, device: ESP32Device, component: Component, param_type: str):
        """Discover all parameters of a given type for a component."""
        ip = device.ip
        
        # Get count first
        response = await self._send_request(device, {
            'type': 'get_param_info',
            'comp': component.name,
            'param_type': param_type,
            'idx': -1
        })
        count = response.get('count', 0)
        
        if count == 0:
            return
        
        # Fetch each parameter
        for idx in range(count):
            response = await self._send_request(device, {
                'type': 'get_param_info',
                'comp': component.name,
                'param_type': param_type,
                'idx': idx
            })
            
            if 'name' not in response:
                continue
            
            param = Parameter(
                param_id=response.get('param_id', 0),
                name=response['name'],
                param_type=param_type,
                rows=response.get('rows', 1),
                cols=response.get('cols', 1),
                read_only=response.get('readOnly', False),
                min_val=response.get('min'),
                max_val=response.get('max')
            )
            
            component.add_parameter(param)
            await asyncio.sleep(DISCOVERY_DELAY)
    
    async def _subscribe_all(self, device: ESP32Device):
        """Subscribe to all parameters on the device."""
        ip = device.ip
        subscription_count = 0
        
        for comp in device.components.values():
            for param in comp.parameters.values():
                # Subscribe to each cell of the parameter
                for row in range(param.rows):
                    for col in range(param.cols):
                        try:
                            response = await self._send_request(device, {
                                'type': 'subscribe',
                                'param_id': param.param_id,
                                'row': row,
                                'col': col
                            })
                            
                            # Store initial value
                            if 'value' in response:
                                param.set_value(row, col, response['value'])
                            
                            subscription_count += 1
                            await asyncio.sleep(SUBSCRIBE_DELAY)  # Rate limit
                            
                        except Exception as e:
                            logger.warning(f"[{ip}] Failed to subscribe to {comp.name}.{param.name}[{row}][{col}]: {e}")
        
        logger.info(f"[{ip}] Subscribed to {subscription_count} parameter cells")
    
    async def _listen_for_updates(self, device: ESP32Device):
        """Listen for parameter updates from the device."""
        ip = device.ip
        
        async for message in device.websocket:
            try:
                data = json.loads(message)
                
                # Check if this is a response to a pending request
                if 'id' in data and data['id'] in device.pending_requests:
                    future = device.pending_requests[data['id']]
                    if not future.done():
                        future.set_result(data)
                    continue
                
                # Handle push updates
                if data.get('type') == 'param_update':
                    self._handle_param_update(device, data)
                    
            except json.JSONDecodeError as e:
                logger.error(f"[{ip}] Invalid JSON: {e}")
            except Exception as e:
                logger.error(f"[{ip}] Error handling message: {e}")
    
    def _handle_param_update(self, device: ESP32Device, data: dict):
        """Handle a parameter update push message."""
        ip = device.ip
        param_id = data.get('param_id')
        row = data.get('row', 0)
        col = data.get('col', 0)
        value = data.get('value')
        
        # Find the parameter
        result = device.get_param_by_id(param_id)
        if not result:
            logger.warning(f"[{ip}] Update for unknown param_id {param_id}")
            return
        
        component, param = result
        old_value = param.get_value(row, col)
        param.set_value(row, col, value)
        
        # Update remote state cache for Watcher
        if ip not in self.remote_state_cache:
            self.remote_state_cache[ip] = {}
        cache_key = f"{component.name}.{param.name}[{row},{col}]"
        self.remote_state_cache[ip][cache_key] = value
        
        # Also store by param_id for faster lookups
        cache_key_id = f"param_{param_id}[{row},{col}]"
        self.remote_state_cache[ip][cache_key_id] = value
        
        # Update device info with cached_state reference for Watcher
        if ip in self.devices:
            # Ensure devices dict has easy access to cached state
            pass  # Already stored in remote_state_cache
        
        # Print the update
        timestamp = datetime.now().strftime('%H:%M:%S.%f')[:-3]
        logger.info(
            f"[{timestamp}] {ip} / {component.name} / {param.name}[{row}][{col}]: "
            f"{old_value} -> {value}"
        )
    
    def get_state_snapshot(self) -> dict:
        """Get a complete snapshot of all device states."""
        snapshot = {}
        
        for ip, device in self.devices.items():
            device_data = {
                'connected': device.connected,
                'components': {}
            }
            
            for comp_name, comp in device.components.items():
                comp_data = {}
                for param_name, param in comp.parameters.items():
                    comp_data[param_name] = {
                        'type': param.param_type,
                        'read_only': param.read_only,
                        'values': {f"{r},{c}": v for (r, c), v in param.values.items()},
                        'last_updated': param.last_updated.isoformat() if param.last_updated else None
                    }
                device_data['components'][comp_name] = comp_data
            
            snapshot[ip] = device_data
        
        return snapshot
    
    def print_state(self):
        """Print current state of all devices."""
        print("\n" + "=" * 80)
        print("CENTRAL HUB STATE SNAPSHOT")
        print("=" * 80)
        
        for ip, device in self.devices.items():
            status = "✓ Connected" if device.connected else "✗ Disconnected"
            print(f"\n[{ip}] {status}")
            print("-" * 40)
            
            for comp_name, comp in device.components.items():
                print(f"  📦 {comp_name}")
                for param_name, param in comp.parameters.items():
                    ro = "🔒" if param.read_only else "✏️"
                    for (row, col), value in param.values.items():
                        print(f"    {ro} {param_name}[{row}][{col}] = {value}")
        
        print("\n" + "=" * 80)


async def main():
    """Main entry point."""
    import sys
    
    # Get device list from command line first, then env, then config
    if len(sys.argv) > 1:
        devices = sys.argv[1:]
    elif os.environ.get('ESP32_IPS'):
        devices = [ip.strip() for ip in os.environ['ESP32_IPS'].split(',')]
    else:
        devices = ESP32_DEVICES.copy() if ESP32_DEVICES else []
    
    # Allow running with no ESP32s (hub-only mode for testing)
    if not devices:
        print("No ESP32 devices configured - running in hub-only mode")
        print("The hub's local components will be available via WebSocket")
        print()
        devices = []
    
    logger.info(f"Configured ESP32 devices: {devices if devices else '(none)'}")
    
    hub = CentralHub(devices, ws_port=WS_SERVER_PORT)
    
    try:
        await hub.start()
    except KeyboardInterrupt:
        logger.info("Received shutdown signal")
    finally:
        await hub.stop()


if __name__ == '__main__':
    asyncio.run(main())
