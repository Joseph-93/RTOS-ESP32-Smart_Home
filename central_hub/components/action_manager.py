"""
ActionManager Component - Queues and executes timed actions.

Manages a time-ordered queue of actions that can:
- Set parameter values on local or remote devices
- Wait specified delays between actions
- Target devices by IP address or nickname
"""

import asyncio
import heapq
import json
import logging
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, TYPE_CHECKING

from .base import Component, IntParameter, StringParameter, BoolParameter

if TYPE_CHECKING:
    from ..central_hub import CentralHub

logger = logging.getLogger('ActionManager')


@dataclass(order=True)
class QueuedAction:
    """An action waiting to be executed at a specific time."""
    execute_at: float  # Unix timestamp
    action: Dict[str, Any] = field(compare=False)
    

class ActionManagerComponent(Component):
    """
    Component for managing timed action execution.
    
    Parameters:
        - action_to_send (StringParameter[1x1]): JSON of action(s) to queue
        - queue_length (IntParameter[1x1]): Current queue size (read-only)
        - enabled (BoolParameter[1x1]): Whether processing is enabled
        - device_nicknames (StringParameter[1x1]): JSON mapping nicknames to IPs
    
    Action format (single action):
    {
        "device": "192.168.1.100",  # IP, nickname, or "self"
        "param_id": 123,  # OR use component + param names
        "component": "LightSensor",
        "param": "threshold",
        "row": 0,
        "col": 0,
        "value": 42,
        "wait_after_ms": 10000  # delay before next action
    }
    
    When setting action_to_send, use:
    {"actions": [action1, action2, ...]}
    
    The action_to_send cell is cleared after actions are queued.
    """
    
    def __init__(self):
        super().__init__("ActionManager")
        
        # Single cell for queueing new actions
        self.action_to_send = self.add_string_param(
            "action_to_send",
            rows=1, cols=1,
            default_val=""
        )
        
        # Current queue size (read-only)
        self.queue_length = self.add_int_param(
            "queue_length",
            rows=1, cols=1,
            min_val=0, max_val=999999,
            default_val=0,
            read_only=True
        )
        
        # Enable/disable processing
        self.enabled = self.add_bool_param(
            "enabled",
            rows=1, cols=1,
            default_val=True
        )
        
        # Device nicknames: JSON mapping "nickname" -> "ip_address"
        self.device_nicknames = self.add_string_param(
            "device_nicknames",
            rows=1, cols=1,
            default_val="{}"
        )
        
        # Shadow dict for O(1) nickname lookups
        self._nickname_map: Dict[str, str] = {}
        
        # Priority queue (min-heap by execute_at time)
        self._action_queue: List[QueuedAction] = []
        
        # Processing task
        self._processing_task: Optional[asyncio.Task] = None
        self._running = False
        
        # Register callbacks
        self.action_to_send.on_change(self._on_action_to_send_change)
        self.device_nicknames.on_change(self._on_nicknames_change)
    
    async def initialize(self):
        """Initialize the component."""
        self._parse_nicknames()
        logger.info("ActionManager component initialized")
    
    async def start(self):
        """Start the action processing loop."""
        self._running = True
        self._processing_task = asyncio.create_task(self._process_queue())
        logger.info("ActionManager processing started")
    
    async def stop(self):
        """Stop the action processing loop."""
        self._running = False
        if self._processing_task:
            self._processing_task.cancel()
            try:
                await self._processing_task
            except asyncio.CancelledError:
                pass
        logger.info("ActionManager processing stopped")
    
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
    
    def _on_action_to_send_change(self, param, row, col, new_value, old_value):
        """Process new actions when action_to_send is set."""
        if not new_value:
            return
        
        try:
            data = json.loads(new_value)
            actions = data.get('actions', [])
            
            if not actions:
                logger.warning("No actions in action_to_send")
                return
            
            self._queue_actions(actions)
            
        except json.JSONDecodeError as e:
            logger.error(f"Invalid JSON in action_to_send: {e}")
        finally:
            # Clear the cell after processing
            self.action_to_send.set_value(0, 0, "", notify=False)
    
    def _queue_actions(self, actions: List[Dict[str, Any]]):
        """Queue a list of actions with their delays."""
        current_time = time.time()
        cumulative_delay = 0.0
        
        for action in actions:
            # Calculate execution time
            execute_at = current_time + cumulative_delay
            
            # Add to priority queue
            queued = QueuedAction(execute_at=execute_at, action=action)
            heapq.heappush(self._action_queue, queued)
            
            # Accumulate delay for next action
            wait_ms = action.get('wait_after_ms', 0)
            cumulative_delay += wait_ms / 1000.0
        
        # Update queue length
        self.queue_length.set_value(0, 0, len(self._action_queue))
        logger.info(f"Queued {len(actions)} actions, queue size: {len(self._action_queue)}")
    
    async def _process_queue(self):
        """Main loop for processing queued actions."""
        while self._running:
            try:
                if not self.enabled.get_value(0, 0):
                    await asyncio.sleep(0.1)
                    continue
                
                if not self._action_queue:
                    await asyncio.sleep(0.05)  # 50ms idle check
                    continue
                
                # Peek at next action
                next_action = self._action_queue[0]
                now = time.time()
                
                if next_action.execute_at <= now:
                    # Time to execute
                    heapq.heappop(self._action_queue)
                    self.queue_length.set_value(0, 0, len(self._action_queue))
                    
                    await self._execute_action(next_action.action)
                else:
                    # Wait until next action time or 100ms, whichever is shorter
                    wait_time = min(next_action.execute_at - now, 0.1)
                    await asyncio.sleep(wait_time)
                    
            except asyncio.CancelledError:
                raise
            except Exception as e:
                logger.error(f"Error in action processing loop: {e}")
                await asyncio.sleep(0.1)
    
    async def _execute_action(self, action: Dict[str, Any]):
        """Execute a single action."""
        device = action.get('device', 'self')
        value = action.get('value')
        row = action.get('row', 0)
        col = action.get('col', 0)
        
        # Resolve device
        target_device = self._resolve_device(device)
        
        logger.debug(f"Executing action: device={target_device}, value={value}")
        
        if target_device == 'self':
            # Local parameter change
            await self._execute_local_action(action, row, col, value)
        else:
            # Remote device
            await self._execute_remote_action(target_device, action, row, col, value)
    
    def _resolve_device(self, device: str) -> str:
        """Resolve device identifier - could be IP, nickname, or 'self'."""
        if device.lower() == 'self':
            return 'self'
        
        # Check nickname map
        if device in self._nickname_map:
            return self._nickname_map[device]
        
        return device
    
    async def _execute_local_action(self, action: Dict[str, Any], 
                                     row: int, col: int, value: Any):
        """Execute action on a local component parameter."""
        if not self.hub:
            logger.error("No hub reference, cannot execute local action")
            return
        
        # Find the parameter
        param_id = action.get('param_id')
        component_name = action.get('component')
        param_name = action.get('param')
        
        param = None
        
        if param_id is not None:
            # Look up by ID across all local components
            for comp in self.hub.local_components.values():
                param = comp.get_param_by_id(param_id)
                if param:
                    break
        elif component_name and param_name:
            # Look up by component and param name
            comp = self.hub.local_components.get(component_name)
            if comp:
                param = comp.get_param(param_name)
        
        if param:
            if param.read_only:
                logger.warning(f"Cannot set read-only parameter: {param.name}")
            else:
                param.set_value(row, col, value)
                logger.debug(f"Set local {param.name}[{row},{col}] = {value}")
        else:
            logger.warning(f"Local parameter not found: {action}")
    
    async def _execute_remote_action(self, device_ip: str, action: Dict[str, Any],
                                      row: int, col: int, value: Any):
        """Execute action on a remote ESP32 device."""
        if not self.hub:
            logger.error("No hub reference, cannot execute remote action")
            return
        
        # Find the device connection
        device = self.hub.devices.get(device_ip)
        if not device:
            logger.warning(f"Device not found: {device_ip}")
            return
        
        # Determine param_id
        param_id = action.get('param_id')
        
        if param_id is None:
            # Need to look up by component/param name
            component_name = action.get('component')
            param_name = action.get('param')
            
            if component_name and param_name:
                # Look through discovered components
                comp = device.components.get(component_name)
                if comp:
                    param = comp.parameters.get(param_name)
                    if param:
                        param_id = param.param_id
        
        if param_id is None:
            logger.error(f"Cannot determine param_id for remote action: {action}")
            return
        
        # Send SET request via WebSocket
        ws = device.websocket
        if not ws:
            logger.error(f"No WebSocket connection to {device_ip}")
            return
        
        request = {
            'type': 'SET',
            'param_id': param_id,
            'row': row,
            'col': col,
            'value': value
        }
        
        try:
            await ws.send(json.dumps(request))
            logger.debug(f"Sent SET to {device_ip}: {request}")
        except Exception as e:
            logger.error(f"Failed to send action to {device_ip}: {e}")
    
    # Convenience methods for programmatic use
    
    def queue_action(self, device: str, param_id: int = None, 
                     component: str = None, param: str = None,
                     row: int = 0, col: int = 0, 
                     value: Any = None, wait_after_ms: int = 0):
        """Programmatically queue a single action."""
        action = {
            'device': device,
            'row': row,
            'col': col,
            'value': value,
            'wait_after_ms': wait_after_ms
        }
        
        if param_id is not None:
            action['param_id'] = param_id
        if component:
            action['component'] = component
        if param:
            action['param'] = param
        
        self._queue_actions([action])
    
    def add_nickname(self, nickname: str, ip_address: str):
        """Add or update a device nickname."""
        self._nickname_map[nickname] = ip_address
        self.device_nicknames.set_value(0, 0, json.dumps(self._nickname_map))
    
    def clear_queue(self):
        """Clear all pending actions."""
        self._action_queue.clear()
        self.queue_length.set_value(0, 0, 0)
        logger.info("Action queue cleared")
