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
from typing import Dict, List, Optional, Any, Set, Tuple
from datetime import datetime

import websockets
from websockets.client import WebSocketClientProtocol

from config import (
    ESP32_DEVICES, WS_PING_INTERVAL, WS_PING_TIMEOUT, 
    RECONNECT_DELAY, DISCOVERY_DELAY, SUBSCRIBE_DELAY, LOG_LEVEL,
    WS_SERVER_PORT, USE_MDNS_DISCOVERY, MDNS_DISCOVERY_TIMEOUT, MDNS_SERVICE_TYPE
)
from components import (
    Component as BaseComponent,
    NetworkActionsComponent,
    ActionManagerComponent, 
    WatcherComponent,
    WebServerComponent
)
from persistence import PersistenceManager

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
    # Track active subscriptions: set of (component_name, param_name, row, col)
    active_subscriptions: Set[Tuple[str, str, int, int]] = field(default_factory=set)
    
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
        
        # Store our own IP so we don't accidentally try to connect to ourselves
        self.local_ip = self._get_local_ip()
        
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
        
        # Initialize persistence manager
        self.persistence = PersistenceManager(self)
    
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
        logger.info(f"Starting Central Hub at {self.local_ip}")
        logger.info(f"Configured to connect to {len(self.esp32_ips)} ESP32 devices")
        self.running = True
        
        # Initialize all local components
        for comp in self.local_components.values():
            await comp.initialize()
        
        # Load saved state AFTER components are initialized
        self.persistence.load()
        
        # Start local component background tasks
        await self.action_manager.start()
        await self.watcher.start()
        await self.web_server.start()
        
        # Start persistence manager (periodic saves)
        await self.persistence.start()
        
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
        
        # Stop persistence manager (does final save)
        await self.persistence.stop()
        
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
        """Connect to device and discover components/params (no automatic subscriptions)."""
        ip = device.ip
        uri = f"ws://{ip}/ws"
        
        logger.info(f"[{ip}] Connecting to {uri}...")
        
        async with websockets.connect(uri, ping_interval=WS_PING_INTERVAL, ping_timeout=WS_PING_TIMEOUT) as ws:
            device.websocket = ws
            device.connected = True
            device.active_subscriptions = set()  # Reset subscriptions on reconnect
            logger.info(f"[{ip}] Connected!")
            
            # Start listener task FIRST so we can receive responses
            listener_task = asyncio.create_task(self._listen_for_updates(device))
            
            try:
                # Discover all components and parameters
                await self._discover_device(device)
                
                # Subscribe to needed parameters (based on Watcher variables)
                await self._subscribe_needed(device)
                
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
    
    async def _subscribe_needed(self, device: ESP32Device):
        """Subscribe only to parameters that are actually needed (used by Watcher)."""
        ip = device.ip
        subscription_count = 0
        
        # Get the list of needed subscriptions from Watcher
        needed = self._get_needed_subscriptions_for_device(ip)
        
        if not needed:
            logger.info(f"[{ip}] No subscriptions needed for this device")
            return
        
        logger.info(f"[{ip}] Subscribing to {len(needed)} needed parameters...")
        
        for comp_name, param_name, row, col in needed:
            # Check if already subscribed
            sub_key = (comp_name, param_name, row, col)
            if sub_key in device.active_subscriptions:
                continue
            
            # Find the parameter
            if comp_name not in device.components:
                logger.warning(f"[{ip}] Component not found for subscription: {comp_name}")
                continue
            
            comp = device.components[comp_name]
            if param_name not in comp.parameters:
                logger.warning(f"[{ip}] Parameter not found for subscription: {comp_name}.{param_name}")
                continue
            
            param = comp.parameters[param_name]
            
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
                    
                    # Update cache
                    if ip not in self.remote_state_cache:
                        self.remote_state_cache[ip] = {}
                    cache_key = f"{comp_name}.{param_name}[{row},{col}]"
                    self.remote_state_cache[ip][cache_key] = response['value']
                
                device.active_subscriptions.add(sub_key)
                subscription_count += 1
                await asyncio.sleep(SUBSCRIBE_DELAY)
                
            except asyncio.TimeoutError:
                logger.warning(f"[{ip}] Timeout subscribing to {comp_name}.{param_name}[{row}][{col}]")
            except websockets.exceptions.ConnectionClosed as e:
                logger.error(f"[{ip}] Connection closed during subscribe: {e}")
                raise
            except Exception as e:
                logger.warning(f"[{ip}] Failed to subscribe to {comp_name}.{param_name}[{row}][{col}]: {e}")
        
        logger.info(f"[{ip}] Subscribed to {subscription_count} parameter cells")
    
    def _get_needed_subscriptions_for_device(self, ip: str) -> List[Tuple[str, str, int, int]]:
        """Get list of (component, param, row, col) that need to be subscribed for a device."""
        needed = []
        
        # Check Watcher variables
        if hasattr(self, 'watcher') and self.watcher._var_defs:
            for var_name, var_def in self.watcher._var_defs.items():
                device = var_def.get('device', 'self')
                
                # Resolve device (might be nickname)
                resolved_device = device
                if device != 'self' and hasattr(self, 'action_manager'):
                    resolved_device = self.action_manager._nickname_map.get(device, device)
                
                if resolved_device == ip:
                    comp = var_def.get('component')
                    param = var_def.get('param')
                    row = var_def.get('row', 0)
                    col = var_def.get('col', 0)
                    if comp and param:
                        needed.append((comp, param, row, col))
        
        return needed
    
    async def subscribe_to_param(self, ip: str, component_name: str, param_name: str, row: int = 0, col: int = 0) -> Optional[Any]:
        """
        Dynamically subscribe to a specific parameter.
        Called by Watcher when new variables are added.
        Returns the value if successful, None if failed.
        """
        if ip not in self.devices:
            logger.warning(f"Cannot subscribe to param on unknown device: {ip}")
            return None
        
        device = self.devices[ip]
        if not device.connected or not device.websocket:
            logger.warning(f"[{ip}] Cannot subscribe - device not connected")
            return None
        
        # Check if already subscribed
        sub_key = (component_name, param_name, row, col)
        if sub_key in device.active_subscriptions:
            # Already subscribed, just return cached value if available
            cache_key = f"{component_name}.{param_name}[{row},{col}]"
            if ip in self.remote_state_cache and cache_key in self.remote_state_cache[ip]:
                return self.remote_state_cache[ip][cache_key]
        
        # Find the parameter
        if component_name not in device.components:
            logger.warning(f"[{ip}] Component not found: {component_name}")
            return None
        
        comp = device.components[component_name]
        if param_name not in comp.parameters:
            logger.warning(f"[{ip}] Parameter not found: {component_name}.{param_name}")
            return None
        
        param = comp.parameters[param_name]
        
        try:
            response = await self._send_request(device, {
                'type': 'subscribe',
                'param_id': param.param_id,
                'row': row,
                'col': col
            })
            
            if 'value' in response:
                value = response['value']
                param.set_value(row, col, value)
                
                # Update cache
                if ip not in self.remote_state_cache:
                    self.remote_state_cache[ip] = {}
                cache_key = f"{component_name}.{param_name}[{row},{col}]"
                self.remote_state_cache[ip][cache_key] = value
                
                device.active_subscriptions.add(sub_key)
                logger.info(f"[{ip}] Subscribed to {component_name}.{param_name}[{row}][{col}] = {value}")
                return value
            else:
                logger.warning(f"[{ip}] Subscribe response had no value: {response}")
                return None
                
        except Exception as e:
            logger.error(f"[{ip}] Failed to subscribe to {component_name}.{param_name}: {e}")
            return None
    
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
        
        # Verbose logging disabled - uncomment for debugging parameter updates
        # timestamp = datetime.now().strftime('%H:%M:%S.%f')[:-3]
        # logger.info(
        #     f"[{timestamp}] {ip} / {component.name} / {param.name}[{row}][{col}]: "
        #     f"{old_value} -> {value}"
        # )
    
    async def request_reconnect(self, ip: str) -> bool:
        """
        Request reconnection to a device. Returns True if device exists.
        The _manage_device loop will handle the actual reconnection.
        """
        if ip not in self.devices:
            logger.warning(f"Cannot reconnect to unknown device: {ip}")
            return False
        
        device = self.devices[ip]
        if device.connected and device.websocket:
            logger.info(f"[{ip}] Forcing reconnection...")
            try:
                await device.websocket.close()
            except Exception as e:
                logger.debug(f"[{ip}] Error closing websocket: {e}")
            device.connected = False
        else:
            logger.info(f"[{ip}] Device already disconnected, reconnect will happen automatically")
        return True
    
    async def subscribe_param_by_name(self, ip: str, component_name: str, param_name: str, row: int = 0, col: int = 0) -> Optional[Any]:
        """
        Subscribe to a specific parameter by name (not UUID).
        Returns the value if successful, None if failed.
        Alias for subscribe_to_param for backwards compatibility.
        """
        return await self.subscribe_to_param(ip, component_name, param_name, row, col)
    
    def is_device_connected(self, ip: str) -> bool:
        """Check if a device is currently connected."""
        if ip not in self.devices:
            return False
        return self.devices[ip].connected
    
    async def add_device_dynamically(self, ip: str) -> dict:
        """
        Add a device by IP address at runtime.
        Returns status dict with success/error info.
        """
        # Don't add ourselves!
        if ip == self.local_ip or ip == '127.0.0.1' or ip == 'localhost':
            return {'success': False, 'error': f'Cannot add self ({ip}) as remote device'}
        
        # Check if already exists
        if ip in self.devices:
            if self.devices[ip].connected:
                return {'success': True, 'message': f'Device {ip} already connected'}
            else:
                return {'success': True, 'message': f'Device {ip} exists, reconnecting...'}
        
        # Add to IP list and start managing it
        if ip not in self.esp32_ips:
            self.esp32_ips.append(ip)
        
        # Start managing the new device (this creates the connection task)
        asyncio.create_task(self._manage_device(ip))
        
        logger.info(f"Added new device: {ip}")
        return {'success': True, 'message': f'Connecting to {ip}...'}

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


# ============================================================================
# mDNS Discovery
# ============================================================================

def discover_esp32_devices(timeout: float = 5.0, service_type: str = "_ws._tcp.local.") -> Dict[str, str]:
    """
    Discover ESP32 devices via mDNS hostname resolution.
    
    Returns:
        Dict mapping device names to IP addresses.
    """
    discovered = {}
    
    # Just resolve known hostnames - much more reliable on Windows than ServiceBrowser
    known_hostnames = ['esp32.local', 'esp32-sensor.local', 'esp32-light.local', 'esp32-hub.local']
    
    logger.info(f"Looking for ESP32 devices via hostname resolution...")
    
    for hostname in known_hostnames:
        try:
            ip = socket.gethostbyname(hostname)
            device_name = hostname.replace('.local', '')
            discovered[device_name] = ip
            logger.info(f"Found: {device_name} at {ip}")
        except socket.gaierror:
            pass  # Hostname didn't resolve
    
    if discovered:
        logger.info(f"Discovered {len(discovered)} device(s)")
    else:
        logger.warning("No devices found via hostname resolution")
    
    return discovered


async def main():
    """Main entry point."""
    import sys
    
    # Get device list from command line first, then env, then config/discovery
    if len(sys.argv) > 1:
        devices = sys.argv[1:]
        logger.info("Using devices from command line arguments")
    elif os.environ.get('ESP32_IPS'):
        devices = [ip.strip() for ip in os.environ['ESP32_IPS'].split(',')]
        logger.info("Using devices from ESP32_IPS environment variable")
    else:
        # Start with static devices from config
        devices = ESP32_DEVICES.copy() if ESP32_DEVICES else []
        
        # Add mDNS discovered devices if enabled
        if USE_MDNS_DISCOVERY:
            discovered = discover_esp32_devices(
                timeout=MDNS_DISCOVERY_TIMEOUT,
                service_type=MDNS_SERVICE_TYPE
            )
            # Add discovered IPs that aren't already in the list
            for name, ip in discovered.items():
                if ip not in devices:
                    devices.append(ip)
                    logger.info(f"Added discovered device: {name} ({ip})")
    
    # Allow running with no ESP32s (hub-only mode for testing)
    if not devices:
        print("No ESP32 devices configured or discovered - running in hub-only mode")
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
