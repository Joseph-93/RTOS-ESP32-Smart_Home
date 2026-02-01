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
from typing import Any, Dict, Optional, Set, Tuple, TYPE_CHECKING

import websockets
from websockets.server import WebSocketServerProtocol

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
        
        # Internal state
        self.server = None
        self.subscriptions = SubscriptionManager()
        self._clients: Set[WebSocketServerProtocol] = set()
        self._running = False
    
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
        logger.info("WebServer component initialized")
    
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
            ping_timeout=10
        )
        
        logger.info(f"WebSocket server started at ws://{self._local_ip}:{self._port}/ws")
        
        # Print to console for user visibility
        print(f"\n{'='*60}")
        print(f"  CENTRAL HUB WebSocket Server")
        print(f"  URL: ws://{self._local_ip}:{self._port}/ws")
        print(f"  Web: http://{self._local_ip}:{self._port}")
        print(f"{'='*60}\n")
    
    async def stop(self):
        """Stop the WebSocket server."""
        self._running = False
        if self.server:
            self.server.close()
            await self.server.wait_closed()
        logger.info("WebSocket server stopped")
    
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
        # Unknown message type
        # ====================================================================
        else:
            return {'error': f'unknown message type: {msg_type}'}
