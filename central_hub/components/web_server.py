"""
WebSocket Server Component for Central Hub

Exposes the same WebSocket API as ESP32 devices, allowing the web dashboard
to connect to and interact with the central hub's local components.

Parameters (read-only):
- port: The port the server is listening on
- local_ip: The IP address of this machine
- connected_clients: Number of currently connected WebSocket clients
- total_messages: Total number of messages handled

API Endpoints (same as ESP32):
- get_components: List all local components
- get_component_params: Get all parameters for a component
- get_param_info: Get parameter info by type and index
- get_param: Get parameter value
- set_param: Set parameter value
- subscribe: Subscribe to parameter updates
- unsubscribe: Unsubscribe from parameter updates
"""

import asyncio
import json
import logging
import socket
from pathlib import Path
from typing import Any, Dict, Optional, Set, Tuple, TYPE_CHECKING

# Directory where firmware files are staged for OTA updates
OTA_SERVE_DIR = Path(__file__).parent.parent / "ota"

import websockets
from websockets.server import WebSocketServerProtocol
from websockets.http11 import Response as WsResponse
from websockets.datastructures import Headers as WsHeaders

# mDNS advertisement
try:
    from zeroconf import Zeroconf, ServiceInfo
    ZEROCONF_AVAILABLE = True
except ImportError:
    ZEROCONF_AVAILABLE = False

from .base import Component, IntParameter, StringParameter

if TYPE_CHECKING:
    from ..central_hub import CentralHub

logger = logging.getLogger('WebServer')


class SubscriptionManager:
    """Manages WebSocket subscriptions to parameters."""
    
    def __init__(self):
        # Maps websocket -> set of (param_id, row, col) subscriptions
        self._subscriptions: Dict[WebSocketServerProtocol, Set[Tuple[int, int, int]]] = {}
        # Maps (param_id, row, col) -> set of websockets
        self._param_subscribers: Dict[Tuple[int, int, int], Set[WebSocketServerProtocol]] = {}
    
    def subscribe(self, ws: WebSocketServerProtocol, param_id: int, row: int, col: int):
        """Subscribe a websocket to a parameter."""
        key = (param_id, row, col)
        
        if ws not in self._subscriptions:
            self._subscriptions[ws] = set()
        self._subscriptions[ws].add(key)
        
        if key not in self._param_subscribers:
            self._param_subscribers[key] = set()
        self._param_subscribers[key].add(ws)
        
        logger.debug(f"Subscribed to param {param_id}[{row}][{col}]")
    
    def unsubscribe(self, ws: WebSocketServerProtocol, param_id: int, row: int, col: int):
        """Unsubscribe a websocket from a parameter."""
        key = (param_id, row, col)
        
        if ws in self._subscriptions:
            self._subscriptions[ws].discard(key)
        
        if key in self._param_subscribers:
            self._param_subscribers[key].discard(ws)
            if not self._param_subscribers[key]:
                del self._param_subscribers[key]
    
    def remove_client(self, ws: WebSocketServerProtocol):
        """Remove all subscriptions for a disconnected client."""
        if ws in self._subscriptions:
            for key in self._subscriptions[ws]:
                if key in self._param_subscribers:
                    self._param_subscribers[key].discard(ws)
                    if not self._param_subscribers[key]:
                        del self._param_subscribers[key]
            del self._subscriptions[ws]
    
    def get_subscribers(self, param_id: int, row: int, col: int) -> Set[WebSocketServerProtocol]:
        """Get all websockets subscribed to a parameter."""
        return self._param_subscribers.get((param_id, row, col), set())


class WebServerComponent(Component):
    """
    WebSocket server component that exposes local components with the same API as ESP32.
    
    Parameters (all read-only):
        - port (int): The port the server is listening on
        - local_ip (string): The IP address of this machine  
        - connected_clients (int): Number of currently connected clients
        - total_messages (int): Total number of messages handled
    """
    
    def __init__(self, port: int = 8080):
        super().__init__("WebServer")
        
        self._port = port
        self._local_ip = self._get_local_ip()
        
        # Parameters
        self.port_param = self.add_int_param(
            "port",
            rows=1, cols=1,
            min_val=1, max_val=65535,
            default_val=port,
            read_only=True
        )
        
        self.local_ip_param = self.add_string_param(
            "local_ip",
            rows=1, cols=1,
            default_val=self._local_ip,
            read_only=True
        )
        
        self.connected_clients_param = self.add_int_param(
            "connected_clients",
            rows=1, cols=1,
            min_val=0, max_val=1000,
            default_val=0,
            read_only=True
        )
        
        self.total_messages_param = self.add_int_param(
            "total_messages",
            rows=1, cols=1,
            min_val=0, max_val=999999999,
            default_val=0,
            read_only=True
        )
        
        # connected_devices: JSON string showing all connected ESP32 devices
        # Format: [{"ip": "10.0.0.46", "name": "esp32-lamp", "connected": true, "components": ["RgbLed", "WebServer"]}]
        self.connected_devices_param = self.add_string_param(
            "connected_devices",
            rows=1, cols=1,
            default_val="[]",
            read_only=True
        )
        
        # Internal state
        self.server = None
        self.subscriptions = SubscriptionManager()
        self._clients: Set[WebSocketServerProtocol] = set()
        self._running = False
        
        # mDNS advertisement
        self._zeroconf: Optional[Zeroconf] = None
        self._mdns_info: Optional[ServiceInfo] = None
    
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
    
    async def initialize(self):
        """Initialize the component."""
        # Update IP in case it changed
        self._local_ip = self._get_local_ip()
        self.local_ip_param.set_value(0, 0, self._local_ip, notify=False)
        
        # Start background task to periodically update connected_devices
        asyncio.create_task(self._update_devices_loop())
        
        logger.info("WebServer component initialized")
    
    async def _update_devices_loop(self):
        """Periodically update the connected_devices parameter."""
        while True:
            await asyncio.sleep(2)  # Update every 2 seconds
            self._update_connected_devices()
    
    def _update_connected_devices(self):
        """Update the connected_devices parameter with current device info."""
        if not self.hub:
            return
        
        devices = []
        for ip, device in self.hub.devices.items():
            comp_names = list(device.components.keys()) if device.components else []
            devices.append({
                'ip': ip,
                'name': device.nickname if hasattr(device, 'nickname') and device.nickname else ip,
                'connected': device.connected if hasattr(device, 'connected') else False,
                'components': comp_names
            })
        
        self.connected_devices_param.set_value(0, 0, json.dumps(devices), notify=True)
    
    async def _process_http_request(self, connection, request):
        """
        Intercept plain HTTP GET/HEAD requests before WebSocket upgrade.
        Serves files from the OTA staging directory at /ota/<filename>.
        All other paths proceed normally to WebSocket upgrade.
        """
        try:
            path = request.path
            if not path.startswith('/ota/'):
                return None  # proceed with WebSocket upgrade

            filename = path[5:]  # strip '/ota/'
            # Sanitize: no path traversal
            if '/' in filename or '..' in filename or not filename:
                return WsResponse(400, 'Bad Request', WsHeaders([]), b'Bad Request')

            filepath = OTA_SERVE_DIR / filename
            if not filepath.exists() or not filepath.is_file():
                logger.warning(f"OTA file not found: {filepath}")
                return WsResponse(404, 'Not Found', WsHeaders([]), b'Not Found')

            data = filepath.read_bytes()
            is_head = getattr(request, 'method', 'GET').upper() == 'HEAD'
            logger.info(f"OTA {'HEAD' if is_head else 'GET'}: {filename} ({len(data)} bytes)")
            headers = WsHeaders([
                ('Content-Type', 'application/octet-stream'),
                ('Content-Length', str(len(data))),
                ('Connection', 'close'),
            ])
            # For HEAD requests return headers only, no body
            return WsResponse(200, 'OK', headers, b'' if is_head else data)
        except Exception as e:
            logger.error(f"OTA process_request error: {e}")
            return WsResponse(500, 'Internal Server Error', WsHeaders([]), b'Error')

    async def start(self):
        """Start the WebSocket server."""
        self._running = True
        
        # Set up parameter change callbacks for broadcasting
        self._setup_broadcast_callbacks()
        
        self.server = await websockets.serve(
            self._handle_client,
            '0.0.0.0',
            self._port,
            ping_interval=30,
            ping_timeout=10,
            process_request=self._process_http_request
        )
        
        logger.info(f"WebSocket server started at ws://{self._local_ip}:{self._port}/ws")
        
        # Register with mDNS so Django webserver can discover us
        await self._register_mdns()
        
        # Print to console for user visibility
        print(f"\n{'='*60}")
        print(f"  CENTRAL HUB WebSocket Server")
        print(f"  URL: ws://{self._local_ip}:{self._port}/ws")
        print(f"  Web: http://{self._local_ip}:{self._port}")
        if self._zeroconf:
            print(f"  mDNS: central-hub._ws._tcp.local.")
        print(f"{'='*60}\n")
    
    async def stop(self):
        """Stop the WebSocket server."""
        self._running = False
        
        # Unregister from mDNS
        await self._unregister_mdns()
        
        if self.server:
            self.server.close()
            await self.server.wait_closed()
        logger.info("WebSocket server stopped")
    
    async def _register_mdns(self):
        """Register this server with mDNS for discovery."""
        if not ZEROCONF_AVAILABLE:
            logger.warning("zeroconf not installed - mDNS advertisement disabled")
            logger.warning("Install with: pip install zeroconf")
            return
        
        def _do_register():
            """Blocking registration - runs in executor."""
            try:
                # Get local IP as bytes for ServiceInfo
                ip_bytes = socket.inet_aton(self._local_ip)
                
                # Create service info
                # Service type must match what Django scans for: _ws._tcp.local.
                self._mdns_info = ServiceInfo(
                    type_="_ws._tcp.local.",
                    name="central-hub._ws._tcp.local.",
                    addresses=[ip_bytes],
                    port=self._port,
                    properties={
                        b'type': b'central-hub',
                        b'version': b'1.0',
                    },
                    server="central-hub.local."
                )
                
                # Create Zeroconf instance
                self._zeroconf = Zeroconf()
                self._zeroconf.register_service(self._mdns_info)
                
                logger.info(f"mDNS: Registered as central-hub._ws._tcp.local. on {self._local_ip}:{self._port}")
                return True
                
            except Exception as e:
                logger.error(f"Failed to register mDNS service: {type(e).__name__}: {e}")
                import traceback
                logger.debug(traceback.format_exc())
                self._zeroconf = None
                self._mdns_info = None
                return False
        
        # Run blocking zeroconf operations in executor
        loop = asyncio.get_event_loop()
        await loop.run_in_executor(None, _do_register)
    
    async def _unregister_mdns(self):
        """Unregister from mDNS."""
        if not self._zeroconf and not self._mdns_info:
            return
            
        def _do_unregister():
            """Blocking unregistration - runs in executor."""
            if self._zeroconf and self._mdns_info:
                try:
                    self._zeroconf.unregister_service(self._mdns_info)
                    logger.info("mDNS: Unregistered central-hub service")
                except Exception as e:
                    logger.error(f"Error unregistering mDNS service: {e}")
                finally:
                    try:
                        self._zeroconf.close()
                    except Exception:
                        pass
                    self._zeroconf = None
                    self._mdns_info = None
        
        # Run blocking zeroconf operations in executor
        loop = asyncio.get_event_loop()
        await loop.run_in_executor(None, _do_unregister)
    
    def _setup_broadcast_callbacks(self):
        """Set up onChange callbacks on all parameters to broadcast updates."""
        if not self.hub:
            return
            
        for comp in self.hub.local_components.values():
            for param in comp.parameters.values():
                param_id = param.param_id
                
                def make_callback(pid):
                    def callback(p, row, col, new_value, old_value):
                        asyncio.create_task(self._broadcast_update(pid, row, col, new_value))
                    return callback
                
                param.on_change(make_callback(param_id))
    
    async def _broadcast_update(self, param_id: int, row: int, col: int, value: Any):
        """Broadcast a parameter update to all subscribers."""
        subscribers = self.subscriptions.get_subscribers(param_id, row, col)
        if not subscribers:
            return
        
        message = json.dumps({
            'type': 'param_update',
            'param_id': param_id,
            'row': row,
            'col': col,
            'value': value
        })
        
        # Send to all subscribers
        dead_clients = []
        for ws in subscribers:
            try:
                await ws.send(message)
            except websockets.exceptions.ConnectionClosed:
                dead_clients.append(ws)
        
        # Clean up dead connections
        for ws in dead_clients:
            self._remove_client(ws)
    
    def _remove_client(self, ws):
        """Remove a client and update the count."""
        self.subscriptions.remove_client(ws)
        self._clients.discard(ws)
        self.connected_clients_param.set_value(0, 0, len(self._clients), notify=True)
    
    async def _handle_client(self, websocket):
        """Handle a WebSocket client connection."""
        self._clients.add(websocket)
        self.connected_clients_param.set_value(0, 0, len(self._clients), notify=True)
        
        client_addr = websocket.remote_address
        path = websocket.path if hasattr(websocket, 'path') else '/ws'
        logger.info(f"Client connected: {client_addr} (path: {path})")
        
        try:
            async for message in websocket:
                try:
                    # Increment message counter
                    total = self.total_messages_param.get_value(0, 0)
                    self.total_messages_param.set_value(0, 0, total + 1, notify=False)
                    
                    request = json.loads(message)
                    response = await self._handle_message(websocket, request)
                    
                    if response:
                        # Add request ID to response if present in request
                        if 'id' in request:
                            response['id'] = request['id']
                        await websocket.send(json.dumps(response))
                        
                except json.JSONDecodeError:
                    error_response = {'error': 'Invalid JSON'}
                    await websocket.send(json.dumps(error_response))
                except Exception as e:
                    logger.error(f"Error handling message: {e}")
                    error_response = {'error': str(e)}
                    await websocket.send(json.dumps(error_response))
                    
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            self._remove_client(websocket)
            logger.info(f"Client disconnected: {client_addr}")
    
    async def _handle_message(self, websocket: WebSocketServerProtocol, 
                               request: dict) -> Optional[dict]:
        """Handle a WebSocket message and return response."""
        msg_type = request.get('type')
        
        if not msg_type:
            return {'error': 'missing type field'}
        
        logger.debug(f"Handling message type: {msg_type}")
        
        # ====================================================================
        # get_components - List all local components
        # ====================================================================
        if msg_type == 'get_components':
            components = []
            for name, comp in self.hub.local_components.items():
                components.append({
                    'name': name,
                    'id': hash(name) & 0xFFFFFFFF  # Generate a pseudo-ID
                })
            return {'components': components}
        
        # ====================================================================
        # get_all_devices - List local hub + all remote ESP32 devices
        # ====================================================================
        elif msg_type == 'get_all_devices':
            devices = []
            
            # Add local hub as "self"
            local_components = []
            for name, comp in self.hub.local_components.items():
                local_components.append({
                    'name': name,
                    'id': hash(name) & 0xFFFFFFFF
                })
            devices.append({
                'device': 'self',
                'name': 'Central Hub (local)',
                'connected': True,
                'components': local_components
            })
            
            # Add remote ESP32 devices
            for ip, esp_device in self.hub.devices.items():
                remote_components = []
                for comp_name, comp in esp_device.components.items():
                    remote_components.append({
                        'name': comp_name,
                        'id': comp.component_id if hasattr(comp, 'component_id') else hash(comp_name) & 0xFFFFFFFF
                    })
                devices.append({
                    'device': ip,
                    'name': esp_device.nickname if hasattr(esp_device, 'nickname') and esp_device.nickname else ip,
                    'connected': esp_device.connected if hasattr(esp_device, 'connected') else True,
                    'components': remote_components
                })
            
            return {'devices': devices}
        
        # ====================================================================
        # add_device - Add a new ESP32 device by IP address
        # ====================================================================
        elif msg_type == 'add_device':
            ip = request.get('ip', '').strip()
            if not ip:
                return {'error': 'missing ip field'}
            
            # Validate IP format (basic check)
            import re
            if not re.match(r'^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$', ip):
                return {'error': f'invalid IP format: {ip}'}
            
            result = await self.hub.add_device_dynamically(ip)
            return result
        
        # ====================================================================
        # rescan_devices - Re-scan for devices via mDNS
        # ====================================================================
        elif msg_type == 'rescan_devices':
            # Import the discovery function - use absolute import
            import sys
            import os
            sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
            from central_hub import discover_esp32_devices
            from config import MDNS_DISCOVERY_TIMEOUT, MDNS_SERVICE_TYPE
            
            # Run discovery in executor to not block
            loop = asyncio.get_event_loop()
            discovered = await loop.run_in_executor(
                None,
                lambda: discover_esp32_devices(MDNS_DISCOVERY_TIMEOUT, MDNS_SERVICE_TYPE)
            )
            
            # Add any newly discovered devices
            added = []
            for name, ip in discovered.items():
                if ip not in self.hub.devices:
                    await self.hub.add_device_dynamically(ip)
                    added.append({'name': name, 'ip': ip})
            
            return {
                'discovered': [{'name': n, 'ip': i} for n, i in discovered.items()],
                'added': added
            }
        
        # ====================================================================
        # get_device_component_params - Get params for a component on any device
        # ====================================================================
        elif msg_type == 'get_device_component_params':
            device = request.get('device', 'self')
            comp_name = request.get('comp')
            
            if not comp_name:
                return {'error': 'missing comp field'}
            
            if device == 'self':
                # Local component
                comp = self.hub.local_components.get(comp_name)
                if not comp:
                    return {'error': 'component not found'}
                
                params_list = []
                for param in comp.parameters.values():
                    param_info = {
                        'name': param.name,
                        'id': param.param_id,
                        'type': param.param_type.value,
                        'rows': param.rows,
                        'cols': param.cols,
                        'readOnly': param.read_only
                    }
                    if hasattr(param, 'min_val'):
                        param_info['min'] = param.min_val
                    if hasattr(param, 'max_val'):
                        param_info['max'] = param.max_val
                    params_list.append(param_info)
                
                return {'device': device, 'component': comp_name, 'params': params_list}
            else:
                # Remote ESP32 device
                esp_device = self.hub.devices.get(device)
                if not esp_device:
                    return {'error': f'device not found: {device}'}
                
                comp = esp_device.components.get(comp_name)
                if not comp:
                    return {'error': f'component not found: {comp_name}'}
                
                params_list = []
                for param_name, param in comp.parameters.items():
                    param_info = {
                        'name': param_name,
                        'id': param.param_id if hasattr(param, 'param_id') else 0,
                        'type': param.param_type if hasattr(param, 'param_type') else 'unknown',
                        'rows': param.rows if hasattr(param, 'rows') else 1,
                        'cols': param.cols if hasattr(param, 'cols') else 1,
                        'readOnly': param.read_only if hasattr(param, 'read_only') else False
                    }
                    params_list.append(param_info)
                
                return {'device': device, 'component': comp_name, 'params': params_list}
        
        # ====================================================================
        # get_component_params - Get all parameters for a component
        # ====================================================================
        elif msg_type == 'get_component_params':
            comp_name = request.get('comp')
            comp_id = request.get('comp_id')
            
            comp = None
            if comp_name:
                comp = self.hub.local_components.get(comp_name)
            elif comp_id is not None:
                # Find by ID
                for name, c in self.hub.local_components.items():
                    if hash(name) & 0xFFFFFFFF == comp_id:
                        comp = c
                        comp_name = name
                        break
            
            if not comp:
                return {'error': 'component not found'}
            
            params_list = []
            for param in comp.parameters.values():
                param_info = {
                    'name': param.name,
                    'id': param.param_id,
                    'type': param.param_type.value,
                    'rows': param.rows,
                    'cols': param.cols,
                    'readOnly': param.read_only
                }
                
                # Add min/max for numeric types
                if hasattr(param, 'min_val'):
                    param_info['min'] = param.min_val
                if hasattr(param, 'max_val'):
                    param_info['max'] = param.max_val
                
                params_list.append(param_info)
            
            return {
                'component': comp_name,
                'component_id': hash(comp_name) & 0xFFFFFFFF,
                'params': params_list
            }
        
        # ====================================================================
        # get_param_info - Old API for one-at-a-time fetching
        # ====================================================================
        elif msg_type == 'get_param_info':
            comp_name = request.get('comp')
            param_type = request.get('param_type')
            idx = request.get('idx', -1)
            
            if not comp_name or not param_type:
                return {'error': 'missing comp or param_type'}
            
            comp = self.hub.local_components.get(comp_name)
            if not comp:
                return {'error': 'component not found'}
            
            # Filter parameters by type
            typed_params = [p for p in comp.parameters.values() 
                          if p.param_type.value == param_type]
            
            if idx == -1:
                # Return count
                return {'count': len(typed_params)}
            
            if idx < 0 or idx >= len(typed_params):
                return {'error': 'index out of range'}
            
            param = typed_params[idx]
            response = {
                'name': param.name,
                'param_id': param.param_id,
                'type': param.param_type.value,
                'rows': param.rows,
                'cols': param.cols,
                'readOnly': param.read_only
            }
            
            if hasattr(param, 'min_val'):
                response['min'] = param.min_val
            if hasattr(param, 'max_val'):
                response['max'] = param.max_val
            
            return response
        
        # ====================================================================
        # get_param - Get parameter value
        # ====================================================================
        elif msg_type == 'get_param':
            param_id = request.get('param_id')
            comp_name = request.get('comp')
            param_name = request.get('param')
            param_type = request.get('param_type')
            idx = request.get('idx')
            row = request.get('row', 0)
            col = request.get('col', 0)
            
            param = None
            
            # Lookup by param_id (preferred)
            if param_id is not None:
                for comp in self.hub.local_components.values():
                    param = comp.get_param_by_id(param_id)
                    if param:
                        break
            # Lookup by comp + param name
            elif comp_name and param_name:
                comp = self.hub.local_components.get(comp_name)
                if comp:
                    param = comp.get_param(param_name)
            # Lookup by comp + param_type + idx (ESP32 compatibility)
            elif comp_name and param_type is not None and idx is not None:
                comp = self.hub.local_components.get(comp_name)
                if comp:
                    param = comp.get_param_by_type_and_index(param_type, idx)
            
            if not param:
                return {'error': 'parameter not found'}
            
            return {
                'name': param.name,
                'id': param.param_id,
                'type': param.param_type.value,
                'value': param.get_value(row, col)
            }
        
        # ====================================================================
        # set_param / SET - Set parameter value (SET is alias used by ActionManager)
        # ====================================================================
        elif msg_type in ('set_param', 'SET'):
            param_id = request.get('param_id')
            comp_name = request.get('comp')
            param_name = request.get('param')
            param_type = request.get('param_type')
            idx = request.get('idx')
            row = request.get('row', 0)
            col = request.get('col', 0)
            value = request.get('value')
            
            if value is None:
                return {'success': False, 'error': 'missing value field'}
            
            param = None
            
            # Lookup by param_id (preferred)
            if param_id is not None:
                for comp in self.hub.local_components.values():
                    param = comp.get_param_by_id(param_id)
                    if param:
                        break
            # Lookup by comp + param name
            elif comp_name and param_name:
                comp = self.hub.local_components.get(comp_name)
                if comp:
                    param = comp.get_param(param_name)
            # Lookup by comp + param_type + idx (ESP32 compatibility)
            elif comp_name and param_type is not None and idx is not None:
                comp = self.hub.local_components.get(comp_name)
                if comp:
                    param = comp.get_param_by_type_and_index(param_type, idx)
            
            if not param:
                return {'success': False, 'error': 'parameter not found'}
            
            if param.read_only:
                return {'success': False, 'error': 'parameter is read-only'}
            
            try:
                param.set_value(row, col, value)
                return {'success': True}
            except Exception as e:
                return {'success': False, 'error': str(e)}
        
        # ====================================================================
        # subscribe - Subscribe to parameter updates
        # ====================================================================
        elif msg_type == 'subscribe':
            param_id = request.get('param_id')
            row = request.get('row', 0)
            col = request.get('col', 0)
            
            if param_id is None:
                return {'error': 'missing param_id'}
            
            # Find the parameter
            param = None
            for comp in self.hub.local_components.values():
                param = comp.get_param_by_id(param_id)
                if param:
                    break
            
            if not param:
                return {'error': 'parameter not found'}
            
            # Add subscription
            self.subscriptions.subscribe(websocket, param_id, row, col)
            
            # Return current value
            return {'value': param.get_value(row, col)}
        
        # ====================================================================
        # unsubscribe - Unsubscribe from parameter updates
        # ====================================================================
        elif msg_type == 'unsubscribe':
            param_id = request.get('param_id')
            row = request.get('row', 0)
            col = request.get('col', 0)
            
            if param_id is None:
                return {'error': 'missing param_id'}
            
            self.subscriptions.unsubscribe(websocket, param_id, row, col)
            return {'success': True}
        
        # ====================================================================
        # get_watcher_state - Get live state of a Watcher expression slot
        # ====================================================================
        elif msg_type == 'get_watcher_state':
            slot = request.get('slot', 0)
            
            # Get the Watcher component
            watcher = self.hub.local_components.get('Watcher')
            if not watcher:
                return {'error': 'Watcher component not found'}
            
            # Get expression for this slot
            expr = watcher.expressions.get_value(slot, 0)
            
            # Get timing parameters (in seconds)
            hold_high_sec = watcher.hold_high_sec.get_value(slot, 0)
            cooldown_sec = watcher.cooldown_sec.get_value(slot, 0)
            
            # Get timing state
            import time
            now = time.time()
            hold_until = watcher._hold_until.get(slot, 0)
            cooldown_until = watcher._cooldown_until.get(slot, 0)
            in_hold = now < hold_until
            in_cooldown = now < cooldown_until
            
            if not expr:
                return {
                    'slot': slot,
                    'expression': '',
                    'result': None,
                    'hold_high_sec': hold_high_sec,
                    'cooldown_sec': cooldown_sec,
                    'in_hold': False,
                    'in_cooldown': False,
                    'variable_values': {},
                    'variable_definitions': watcher._var_defs
                }
            
            # Get variable values, definitions, and errors
            var_values = dict(watcher._var_values)
            var_defs = dict(watcher._var_defs)
            var_errors = dict(getattr(watcher, '_var_errors', {}))
            
            # Try to evaluate the expression
            try:
                raw_result = watcher._evaluate_expression(expr)
                effective_result = watcher._apply_timing_logic(slot, raw_result, now)
                return {
                    'slot': slot,
                    'expression': expr,
                    'raw_result': raw_result,
                    'result': effective_result,
                    'hold_high_sec': hold_high_sec,
                    'cooldown_sec': cooldown_sec,
                    'in_hold': in_hold,
                    'in_cooldown': in_cooldown,
                    'hold_remaining_sec': round(max(0, hold_until - now), 1) if in_hold else 0,
                    'cooldown_remaining_sec': round(max(0, cooldown_until - now), 1) if in_cooldown else 0,
                    'variable_values': var_values,
                    'variable_definitions': var_defs,
                    'variable_errors': var_errors
                }
            except Exception as e:
                return {
                    'slot': slot,
                    'expression': expr,
                    'result': None,
                    'error': str(e),
                    'hold_high_sec': hold_high_sec,
                    'cooldown_sec': cooldown_sec,
                    'in_hold': in_hold,
                    'in_cooldown': in_cooldown,
                    'variable_values': var_values,
                    'variable_definitions': var_defs,
                    'variable_errors': var_errors
                }
        
        # ====================================================================
        # Unknown message type
        # ====================================================================
        else:
            return {'error': f'unknown message type: {msg_type}'}
