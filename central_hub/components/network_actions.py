"""
NetworkActions Component - Handles sending network messages to remote devices.

Supports 100 configurable network message slots with protocols:
UDP, TCP, HTTP, HTTPS, WS, WSS

Each message can be configured to fire-and-forget or await response.
"""

import asyncio
import json
import logging
import socket
import ssl
from typing import Any, Dict, Optional

import aiohttp
import websockets

from .base import Component, IntParameter, StringParameter

logger = logging.getLogger('NetworkActions')

# Number of network action slots
NUM_NETWORK_ACTIONS = 100


class NetworkActionsComponent(Component):
    """
    Component for sending configurable network messages.
    
    Parameters:
        - network_messages (StringParameter[100]): JSON config for each network message slot
        - trigger (IntParameter): Set to 0-99 to trigger that message, -1 = idle
        - last_response (StringParameter): Stores the last received response
        - device_nicknames (StringParameter): JSON mapping nicknames to IPs
    
    Message config format:
    {
        "protocol": "HTTP",  # UDP, TCP, HTTP, HTTPS, WS, WSS
        "host": "192.168.1.100",
        "port": 80,
        "path": "/api",  # for HTTP/WS
        "method": "POST",  # for HTTP: GET, POST, PUT, DELETE
        "headers": {},  # for HTTP
        "body": "{}",  # message body
        "await_response": true,  # whether to wait for response
        "timeout_ms": 5000
    }
    """
    
    def __init__(self):
        super().__init__("NetworkActions")
        
        # Network message configurations (100 slots)
        self.network_messages = self.add_string_param(
            "network_messages", 
            rows=NUM_NETWORK_ACTIONS, 
            cols=1,
            default_val=""
        )
        
        # Trigger parameter: -1 = idle, 0-99 = trigger that message
        self.trigger = self.add_int_param(
            "trigger",
            rows=1, cols=1,
            min_val=-1, max_val=NUM_NETWORK_ACTIONS - 1,
            default_val=-1
        )
        
        # Last received response
        self.last_response = self.add_string_param(
            "last_response",
            rows=1, cols=1,
            default_val="",
            read_only=True
        )
        
        # Device nicknames: JSON mapping "nickname" -> "ip_address"
        self.device_nicknames = self.add_string_param(
            "device_nicknames",
            rows=1, cols=1,
            default_val="{}"
        )
        
        # Shadow dict for O(1) nickname lookups
        self._nickname_map: Dict[str, str] = {}
        
        # Register callbacks
        self.trigger.on_change(self._on_trigger_change)
        self.device_nicknames.on_change(self._on_nicknames_change)
    
    async def initialize(self):
        """Initialize the component."""
        # Load initial nickname map
        self._parse_nicknames()
        logger.info("NetworkActions component initialized")
    
    def _parse_nicknames(self):
        """Parse the device_nicknames JSON into the shadow dict."""
        try:
            raw = self.device_nicknames.get_value(0, 0)
            if raw:
                self._nickname_map = json.loads(raw)
            else:
                self._nickname_map = {}
        except json.JSONDecodeError as e:
            logger.error(f"Failed to parse device nicknames: {e}")
            self._nickname_map = {}
    
    def _on_nicknames_change(self, param, row, col, new_value, old_value):
        """Update shadow dict when nicknames change."""
        self._parse_nicknames()
        logger.debug(f"Device nicknames updated: {self._nickname_map}")
    
    def _on_trigger_change(self, param, row, col, new_value, old_value):
        """Handle trigger changes - fire off network action."""
        if new_value >= 0:
            # Schedule the network action
            asyncio.create_task(self._execute_action(new_value))
            # Reset trigger to -1
            self.trigger.set_value(0, 0, -1, notify=False)
    
    def resolve_host(self, host: str) -> str:
        """Resolve a host - could be IP, hostname, or nickname."""
        # Check nickname map first
        if host in self._nickname_map:
            return self._nickname_map[host]
        return host
    
    async def _execute_action(self, index: int):
        """Execute the network action at the given index."""
        config_str = self.network_messages.get_value(index, 0)
        if not config_str:
            logger.warning(f"No config for network action {index}")
            return
        
        try:
            config = json.loads(config_str)
        except json.JSONDecodeError as e:
            logger.error(f"Invalid config JSON for action {index}: {e}")
            return
        
        protocol = config.get('protocol', 'HTTP').upper()
        host = self.resolve_host(config.get('host', 'localhost'))
        port = config.get('port', 80)
        await_response = config.get('await_response', False)
        timeout_ms = config.get('timeout_ms', 5000)
        timeout_sec = timeout_ms / 1000.0
        
        logger.info(f"Executing network action {index}: {protocol} to {host}:{port}")
        
        try:
            response = None
            
            if protocol == 'UDP':
                response = await self._send_udp(host, port, config, timeout_sec, await_response)
            elif protocol == 'TCP':
                response = await self._send_tcp(host, port, config, timeout_sec, await_response)
            elif protocol in ('HTTP', 'HTTPS'):
                response = await self._send_http(protocol, host, port, config, timeout_sec)
            elif protocol in ('WS', 'WSS'):
                response = await self._send_websocket(protocol, host, port, config, timeout_sec, await_response)
            else:
                logger.error(f"Unknown protocol: {protocol}")
                return
            
            if response is not None:
                self.last_response.set_value(0, 0, str(response))
                logger.debug(f"Response from action {index}: {response[:100]}...")
                
        except asyncio.TimeoutError:
            logger.warning(f"Timeout on network action {index}")
            self.last_response.set_value(0, 0, f"ERROR: Timeout after {timeout_ms}ms")
        except Exception as e:
            logger.error(f"Error executing network action {index}: {e}")
            self.last_response.set_value(0, 0, f"ERROR: {str(e)}")
    
    async def _send_udp(self, host: str, port: int, config: dict, 
                        timeout: float, await_response: bool) -> Optional[str]:
        """Send UDP message."""
        body = config.get('body', '')
        if isinstance(body, dict):
            body = json.dumps(body)
        
        loop = asyncio.get_event_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setblocking(False)
        
        try:
            await loop.sock_sendto(sock, body.encode(), (host, port))
            
            if await_response:
                sock.settimeout(timeout)
                data, addr = await asyncio.wait_for(
                    loop.sock_recvfrom(sock, 4096),
                    timeout=timeout
                )
                return data.decode()
        finally:
            sock.close()
        
        return None
    
    async def _send_tcp(self, host: str, port: int, config: dict,
                        timeout: float, await_response: bool) -> Optional[str]:
        """Send TCP message."""
        body = config.get('body', '')
        if isinstance(body, dict):
            body = json.dumps(body)
        
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port),
            timeout=timeout
        )
        
        try:
            writer.write(body.encode())
            await writer.drain()
            
            if await_response:
                data = await asyncio.wait_for(
                    reader.read(4096),
                    timeout=timeout
                )
                return data.decode()
        finally:
            writer.close()
            await writer.wait_closed()
        
        return None
    
    async def _send_http(self, protocol: str, host: str, port: int,
                         config: dict, timeout: float) -> Optional[str]:
        """Send HTTP/HTTPS request."""
        method = config.get('method', 'GET').upper()
        path = config.get('path', '/')
        headers = config.get('headers', {})
        body = config.get('body', '')
        
        if isinstance(body, dict):
            body = json.dumps(body)
            if 'Content-Type' not in headers:
                headers['Content-Type'] = 'application/json'
        
        scheme = 'https' if protocol == 'HTTPS' else 'http'
        url = f"{scheme}://{host}:{port}{path}"
        
        timeout_obj = aiohttp.ClientTimeout(total=timeout)
        
        async with aiohttp.ClientSession(timeout=timeout_obj) as session:
            async with session.request(method, url, headers=headers, data=body) as resp:
                return await resp.text()
    
    async def _send_websocket(self, protocol: str, host: str, port: int,
                              config: dict, timeout: float, 
                              await_response: bool) -> Optional[str]:
        """Send WebSocket message."""
        path = config.get('path', '/')
        body = config.get('body', '')
        
        if isinstance(body, dict):
            body = json.dumps(body)
        
        scheme = 'wss' if protocol == 'WSS' else 'ws'
        uri = f"{scheme}://{host}:{port}{path}"
        
        ssl_context = ssl.create_default_context() if protocol == 'WSS' else None
        
        async with websockets.connect(uri, ssl=ssl_context, close_timeout=timeout) as ws:
            await ws.send(body)
            
            if await_response:
                response = await asyncio.wait_for(
                    ws.recv(),
                    timeout=timeout
                )
                return response
        
        return None
    
    # Convenience methods for programmatic triggering
    
    async def send_message(self, index: int):
        """Programmatically trigger a network action."""
        if 0 <= index < NUM_NETWORK_ACTIONS:
            await self._execute_action(index)
    
    def set_message_config(self, index: int, config: dict):
        """Set the configuration for a message slot."""
        if 0 <= index < NUM_NETWORK_ACTIONS:
            self.network_messages.set_value(index, 0, json.dumps(config))
    
    def add_nickname(self, nickname: str, ip_address: str):
        """Add or update a device nickname."""
        self._nickname_map[nickname] = ip_address
        self.device_nicknames.set_value(0, 0, json.dumps(self._nickname_map))
    
    def remove_nickname(self, nickname: str):
        """Remove a device nickname."""
        if nickname in self._nickname_map:
            del self._nickname_map[nickname]
            self.device_nicknames.set_value(0, 0, json.dumps(self._nickname_map))
