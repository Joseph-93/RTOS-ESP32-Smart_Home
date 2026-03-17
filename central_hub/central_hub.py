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
    ESP32_DEVICES, WS_PING_INTERVAL, WS_PING_TIMEOUT, WS_CONNECT_TIMEOUT,
    RECONNECT_DELAY, DISCOVERY_DELAY, SUBSCRIBE_DELAY, LOG_LEVEL,
    WS_SERVER_PORT, USE_MDNS_DISCOVERY, MDNS_DISCOVERY_TIMEOUT, MDNS_SERVICE_TYPE,
    MDNS_REDISCOVERY_INTERVAL
)
from components import (
    Component as BaseComponent,
    NetworkActionsComponent,
    ActionManagerComponent, 
    WatcherComponent,
    WebServerComponent,
    LogCollectorComponent
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
    device_id: str = ""  # Unique device identifier (MAC-based, persists across IP changes)
    hostname: str = ""   # mDNS hostname
    mac: str = ""        # Full MAC address
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
        self.devices_by_id: Dict[str, ESP32Device] = {}  # device_id -> device (same objects as above)
        self.running = False
        
        # Track active device management tasks to prevent duplicates
        self._device_tasks: Dict[str, asyncio.Task] = {}  # ip -> task
        
        # Store our own IP so we don't accidentally try to connect to ourselves
        self.local_ip = self._get_local_ip()
        
        # Local components for control logic
        self.local_components: Dict[str, BaseComponent] = {}
        
        # Cache for remote parameter values (used by Watcher)
        # Format: { "ip": { "Component.param[row,col]": value } }
        self.remote_state_cache: Dict[str, Dict[str, Any]] = {}
        
        # Callbacks for device connection events
        self._on_device_connected_callbacks: List = []
        self._on_device_disconnected_callbacks: List = []
        
        # Initialize local components
        self._init_local_components()
    
    def _init_local_components(self):
        """Initialize the local control components."""
        # Create components
        self.network_actions = NetworkActionsComponent()
        self.action_manager = ActionManagerComponent()
        self.watcher = WatcherComponent()
        self.web_server = WebServerComponent(port=self.ws_port)
        self.log_collector = LogCollectorComponent()
        
        # Register them
        self.local_components['NetworkActions'] = self.network_actions
        self.local_components['ActionManager'] = self.action_manager
        self.local_components['Watcher'] = self.watcher
        self.local_components['WebServer'] = self.web_server
        self.local_components['LogCollector'] = self.log_collector
        
        # Set hub reference on each component
        for comp in self.local_components.values():
            comp.hub = self
        
        # Share nickname map between components
        # When ActionManager updates nicknames, Watcher should see them too
        self.watcher.set_nickname_map(self.action_manager._nickname_map)
        
        # Register Watcher to be notified when devices reconnect
        # This clears stale variable tracking so it gets fresh values immediately
        self.on_device_connected(self.watcher.on_device_reconnected)
        
        # Register LogCollector to subscribe to log params on connect/disconnect
        self.on_device_connected(self.log_collector.on_device_connected)
        self.on_device_disconnected(self.log_collector.on_device_disconnected)
        
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
    
    def on_device_connected(self, callback):
        """Register a callback to be called when a device connects/reconnects.
        
        Callback signature: callback(ip: str)
        """
        self._on_device_connected_callbacks.append(callback)
    
    def on_device_disconnected(self, callback):
        """Register a callback to be called when a device disconnects.
        
        Callback signature: callback(ip: str)
        """
        self._on_device_disconnected_callbacks.append(callback)
    
    async def _notify_device_connected(self, ip: str):
        """Notify all registered callbacks that a device connected."""
        for callback in self._on_device_connected_callbacks:
            try:
                if asyncio.iscoroutinefunction(callback):
                    await callback(ip)
                else:
                    callback(ip)
            except Exception as e:
                logger.error(f"Error in device connected callback: {e}")
    
    async def _notify_device_disconnected(self, ip: str):
        """Notify all registered callbacks that a device disconnected."""
        for callback in self._on_device_disconnected_callbacks:
            try:
                if asyncio.iscoroutinefunction(callback):
                    await callback(ip)
                else:
                    callback(ip)
            except Exception as e:
                logger.error(f"Error in device disconnected callback: {e}")
        
    async def start(self):
        """Start the central hub - connect to all devices."""
        logger.info(f"Starting Central Hub at {self.local_ip}")
        logger.info(f"Configured to connect to {len(self.esp32_ips)} ESP32 devices")
        self.running = True
        
        # Initialize all local components
        for comp in self.local_components.values():
            await comp.initialize()
        
        # Load saved state AFTER components are initialized
        # Also restores previously known device IPs
        persisted_devices = self.persistence.load()
        
        # Add any persisted devices that aren't already in the list
        for ip in persisted_devices:
            if ip not in self.esp32_ips and ip != self.local_ip:
                self.esp32_ips.append(ip)
                logger.info(f"📡 Restored persisted device: {ip}")
        
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
        
        # Start periodic mDNS rediscovery task (to find devices that come back online)
        tasks.append(self._periodic_mdns_discovery())
        
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
        await self.log_collector.stop()
        
        # Close all connections
        for device in self.devices.values():
            if device.websocket:
                await device.websocket.close()
    
    async def _manage_device(self, ip: str):
        """Manage connection to a single ESP32 device with auto-reconnect."""
        # Check if a task is already managing this device
        if ip in self._device_tasks:
            existing_task = self._device_tasks[ip]
            if not existing_task.done():
                logger.warning(f"[{ip}] Device already being managed, skipping duplicate task")
                return
        
        # Register this task
        self._device_tasks[ip] = asyncio.current_task()
        
        # Reuse existing device object if present, otherwise create new
        if ip not in self.devices:
            device = ESP32Device(ip=ip)
            self.devices[ip] = device
        else:
            device = self.devices[ip]
        
        try:
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
        finally:
            # Clean up task tracking
            if ip in self._device_tasks and self._device_tasks[ip] == asyncio.current_task():
                del self._device_tasks[ip]
    
    async def _connect_and_subscribe(self, device: ESP32Device):
        """Connect to device and discover components/params (no automatic subscriptions)."""
        ip = device.ip
        uri = f"ws://{ip}/ws"
        
        logger.info(f"[{ip}] Connecting to {uri}...")
        
        async with websockets.connect(uri, open_timeout=WS_CONNECT_TIMEOUT, ping_interval=WS_PING_INTERVAL, ping_timeout=WS_PING_TIMEOUT) as ws:
            device.websocket = ws
            device.connected = True
            device.active_subscriptions = set()  # Reset subscriptions on reconnect
            logger.info(f"[{ip}] Connected!")
            
            # Start listener task FIRST so we can receive responses
            listener_task = asyncio.create_task(self._listen_for_updates(device))
            
            try:
                # Query device identity (for IP change detection)
                await self._query_device_info(device)
                
                # Discover all components and parameters FIRST
                await self._discover_device(device)
                
                # NOW notify listeners that device connected/reconnected
                # (after discovery so components are available)
                await self._notify_device_connected(ip)
                
                # Subscribe to needed parameters (based on Watcher variables)
                await self._subscribe_needed(device)
                
                # Wait for listener to complete (will run until disconnect)
                await listener_task
            except Exception as e:
                listener_task.cancel()
                raise
            finally:
                # Notify listeners that device disconnected
                device.connected = False
                await self._notify_device_disconnected(ip)
    
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
    
    # ========================================================================
    # OTA Firmware Update
    # ========================================================================
    
    async def trigger_ota_update(self, device_ip: str, firmware_url: str) -> dict:
        """
        Trigger an OTA firmware update on a connected ESP32 device.
        
        Args:
            device_ip: IP address of the target ESP32
            firmware_url: HTTP URL where the firmware .bin file is hosted
                         e.g. "http://192.168.1.5:8080/firmware.bin"
        
        Returns:
            Response dict from the ESP32 (contains 'success' bool)
        
        Usage:
            # Host the firmware binary (e.g. from build/esp32_rtos_smart_home.bin):
            #   cd firmware/esp32_rtos_smart_home/build
            #   python -m http.server 8080
            #
            # Then trigger OTA:
            #   await hub.trigger_ota_update("192.168.1.100", "http://192.168.1.5:8080/esp32_rtos_smart_home.bin")
            #
            # The ESP32 will download, verify, and reboot into the new firmware.
            # Subscribe to the OTA component's 'status' and 'progress' parameters
            # for real-time update tracking.
        """
        device = self.devices.get(device_ip)
        if not device or not device.connected:
            raise ConnectionError(f"Device {device_ip} not connected")
        
        logger.info(f"[{device_ip}] Triggering OTA update from: {firmware_url}")
        
        response = await self._send_request(
            device, 
            {'type': 'start_ota', 'url': firmware_url},
            timeout=15.0
        )
        
        if response.get('success'):
            logger.info(f"[{device_ip}] OTA update started successfully")
            logger.info(f"[{device_ip}] Device will reboot when complete - expect temporary disconnect")
        else:
            logger.error(f"[{device_ip}] OTA update failed to start: {response.get('error', 'unknown')}")
        
        return response
    
    async def trigger_ota_update_all(self, firmware_url: str) -> dict:
        """
        Trigger OTA update on ALL connected ESP32 devices (sequentially).
        
        Args:
            firmware_url: HTTP URL where the firmware .bin file is hosted
            
        Returns:
            Dict of {ip: response} for each device
        """
        results = {}
        connected_devices = [
            (ip, dev) for ip, dev in self.devices.items() 
            if dev.connected
        ]
        
        if not connected_devices:
            logger.warning("No connected devices to update")
            return results
        
        logger.info(f"Starting OTA update for {len(connected_devices)} device(s)")
        
        for ip, device in connected_devices:
            try:
                results[ip] = await self.trigger_ota_update(ip, firmware_url)
            except Exception as e:
                logger.error(f"[{ip}] OTA trigger failed: {e}")
                results[ip] = {'success': False, 'error': str(e)}
        
        return results

    async def _query_device_info(self, device: ESP32Device):
        """Query device identity info for IP change detection."""
        ip = device.ip
        try:
            response = await self._send_request(device, {'type': 'get_device_info'})
            
            device_id = response.get('device_id', '')
            hostname = response.get('hostname', '')
            mac = response.get('mac', '')
            
            if device_id:
                old_device = self.devices_by_id.get(device_id)
                
                if old_device and old_device.ip != ip:
                    # This device was known at a different IP!
                    logger.info(f"🔄 Device {device_id} IP changed: {old_device.ip} -> {ip}")
                    
                    # Remove old IP entry
                    if old_device.ip in self.devices:
                        del self.devices[old_device.ip]
                    if old_device.ip in self.esp32_ips:
                        self.esp32_ips.remove(old_device.ip)
                    
                    # Update remote_state_cache keys from old IP to new IP
                    if old_device.ip in self.remote_state_cache:
                        self.remote_state_cache[ip] = self.remote_state_cache.pop(old_device.ip)
                    
                    # Notify components about the IP change
                    await self._notify_device_ip_changed(device_id, old_device.ip, ip)
                
                # Update device info
                device.device_id = device_id
                device.hostname = hostname
                device.mac = mac
                device.name = hostname or device_id
                
                # Register in device_id map
                self.devices_by_id[device_id] = device
                
                logger.info(f"[{ip}] Device identity: {device_id} (hostname: {hostname})")
            else:
                logger.warning(f"[{ip}] Device did not provide device_id (older firmware?)")
                
        except Exception as e:
            # Older firmware may not support get_device_info - that's OK
            logger.debug(f"[{ip}] Could not query device_info: {e}")
    
    async def _notify_device_ip_changed(self, device_id: str, old_ip: str, new_ip: str):
        """Notify components when a device's IP address changes."""
        # Update ActionManager nicknames that reference the old IP
        if hasattr(self, 'action_manager'):
            for nickname, mapped_ip in list(self.action_manager._nickname_map.items()):
                if mapped_ip == old_ip:
                    self.action_manager._nickname_map[nickname] = new_ip
                    logger.info(f"📝 Updated nickname '{nickname}': {old_ip} -> {new_ip}")
            # Persist the updated nicknames
            import json
            self.action_manager.device_nicknames.set_value(
                0, 0, json.dumps(self.action_manager._nickname_map), notify=False
            )
        
        # Update Watcher variable definitions that reference the old IP
        if hasattr(self, 'watcher'):
            import json
            updated = False
            for var_name, var_def in self.watcher._var_defs.items():
                if var_def.get('device') == old_ip:
                    var_def['device'] = new_ip
                    updated = True
                    logger.info(f"📝 Updated Watcher variable '{var_name}': {old_ip} -> {new_ip}")
            
            if updated:
                # Persist the updated variables
                self.watcher.variables.set_value(
                    0, 0, json.dumps(self.watcher._var_defs), notify=False
                )
    
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
            elif 'error' in response:
                # Device returned an error (e.g., "index out of bounds")
                error_msg = response.get('error', 'unknown error')
                logger.warning(f"[{ip}] Subscribe failed for {component_name}.{param_name}[{row}][{col}]: {error_msg}")
                # Return the error as a dict so callers can distinguish from None
                return {'_subscription_error': error_msg, 'response': response}
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
        
        # Route any component's 'log' parameter updates to LogCollector
        if param.name == 'log' and value:
            device_name = device.name or device.hostname or ip
            logger.info(f"[{ip}] 📝 Log from {component.name}: {str(value)[:120]}")
            self.log_collector.process_log_update(ip, device_name, str(value), component.name)
        
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
        
        # Check if a task is already managing this device
        if ip in self._device_tasks and not self._device_tasks[ip].done():
            if ip in self.devices and self.devices[ip].connected:
                return {'success': True, 'message': f'Device {ip} already connected'}
            else:
                return {'success': True, 'message': f'Device {ip} already being managed'}
        
        # Add to IP list and start managing it
        if ip not in self.esp32_ips:
            self.esp32_ips.append(ip)
        
        # Start managing the new device (this creates the connection task)
        asyncio.create_task(self._manage_device(ip))
        
        logger.info(f"Added new device: {ip}")
        return {'success': True, 'message': f'Connecting to {ip}...'}

    async def _periodic_mdns_discovery(self):
        """
        Periodically run mDNS discovery to find devices that came back online.
        This allows the hub to automatically reconnect to ESP32s that rebooted.
        """
        if not USE_MDNS_DISCOVERY:
            logger.info("Periodic mDNS discovery disabled (USE_MDNS_DISCOVERY=False)")
            return
        
        logger.info(f"🔍 Starting periodic mDNS discovery (every {MDNS_REDISCOVERY_INTERVAL}s)")
        
        while self.running:
            try:
                await asyncio.sleep(MDNS_REDISCOVERY_INTERVAL)
                
                if not self.running:
                    break
                
                # Run mDNS discovery in a thread to avoid blocking
                loop = asyncio.get_event_loop()
                discovered = await loop.run_in_executor(
                    None, 
                    lambda: discover_esp32_devices(
                        timeout=MDNS_DISCOVERY_TIMEOUT,
                        service_type=MDNS_SERVICE_TYPE
                    )
                )
                
                # Check for newly discovered devices
                for name, ip in discovered.items():
                    if ip == self.local_ip:
                        continue
                    
                    if ip not in self.devices:
                        # Brand new device - add it
                        logger.info(f"🔍 mDNS discovered new device: {name} ({ip})")
                        await self.add_device_dynamically(ip)
                    elif not self.devices[ip].connected:
                        # Known device that's offline - it will auto-reconnect via _manage_device
                        logger.debug(f"🔍 mDNS found returning device: {name} ({ip}) - will reconnect automatically")
                
            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error(f"Error in periodic mDNS discovery: {e}")

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
    import sys
    # On Windows, the default ProactorEventLoop has a bug where [WinError 64]
    # ("specified network name no longer available") from one socket can corrupt
    # other pending IOCP operations, killing unrelated connections.
    # The SelectorEventLoop doesn't have this issue.
    if sys.platform == 'win32':
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    asyncio.run(main())
