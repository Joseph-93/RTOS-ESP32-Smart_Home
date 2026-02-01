"""
Watcher Component - Monitors variables and triggers actions on expression changes.

Evaluates logic expressions every 100ms and triggers rising/falling edge actions
when expression results change from False->True or True->False.
"""

import asyncio
import json
import logging
import re
from typing import Any, Dict, List, Optional, Set, Tuple, TYPE_CHECKING

from .base import Component, StringParameter, IntParameter, BoolParameter

if TYPE_CHECKING:
    from ..central_hub import CentralHub

logger = logging.getLogger('Watcher')

# Number of watch expression slots
NUM_WATCH_SLOTS = 50

# Evaluation interval in seconds
EVAL_INTERVAL_SEC = 0.1  # 100ms


class WatcherComponent(Component):
    """
    Component that monitors variables and triggers actions on expression changes.
    
    Parameters:
        - variables (StringParameter[1x1]): JSON mapping variable names to sources
        - expressions (StringParameter[NUM_WATCH_SLOTS x 1]): Logic expressions
        - rising_actions (StringParameter[NUM_WATCH_SLOTS x 1]): Actions on True transition
        - falling_actions (StringParameter[NUM_WATCH_SLOTS x 1]): Actions on False transition  
        - enabled (BoolParameter[1x1]): Whether evaluation is enabled
        - eval_count (IntParameter[1x1]): Number of evaluations performed (read-only)
    
    Variables format (JSON object):
    {
        "main_light": {
            "device": "192.168.1.100",  # IP, nickname, or "self"
            "component": "LightSensor",
            "param": "current_light_level",
            "row": 0,
            "col": 0
        },
        "motion_detected": {
            "device": "ESP32_Motion",
            "component": "MotionSensor", 
            "param": "motion_state",
            "row": 0,
            "col": 0
        }
    }
    
    Expression format (string):
    "main_light > 50 and motion_detected == true"
    
    Supported operators: and, or, not, ==, !=, <, >, <=, >=, +, -, *, /, (, )
    
    Actions format (JSON):
    {
        "actions": [
            {"device": "self", "param_id": 5, "row": 0, "col": 0, "value": 1, "wait_after_ms": 0}
        ]
    }
    """
    
    def __init__(self):
        super().__init__("Watcher")
        
        # Variable definitions (single JSON object)
        self.variables = self.add_string_param(
            "variables",
            rows=1, cols=1,
            default_val="{}"
        )
        
        # Logic expressions (one per slot)
        self.expressions = self.add_string_param(
            "expressions",
            rows=NUM_WATCH_SLOTS, cols=1,
            default_val=""
        )
        
        # Rising edge actions (True transition)
        self.rising_actions = self.add_string_param(
            "rising_actions",
            rows=NUM_WATCH_SLOTS, cols=1,
            default_val=""
        )
        
        # Falling edge actions (False transition)
        self.falling_actions = self.add_string_param(
            "falling_actions",
            rows=NUM_WATCH_SLOTS, cols=1,
            default_val=""
        )
        
        # Enable/disable evaluation
        self.enabled = self.add_bool_param(
            "enabled",
            rows=1, cols=1,
            default_val=True
        )
        
        # Evaluation counter
        self.eval_count = self.add_int_param(
            "eval_count",
            rows=1, cols=1,
            min_val=0, max_val=999999999,
            default_val=0,
            read_only=True
        )
        
        # Parsed variable definitions
        self._var_defs: Dict[str, Dict[str, Any]] = {}
        
        # Current variable values cache
        self._var_values: Dict[str, Any] = {}
        
        # Previous expression results for edge detection
        self._prev_results: Dict[int, bool] = {}
        
        # Background task
        self._eval_task: Optional[asyncio.Task] = None
        self._running = False
        
        # Device nickname map (shared reference from ActionManager if available)
        self._nickname_map: Dict[str, str] = {}
        
        # Register callbacks
        self.variables.on_change(self._on_variables_change)
    
    async def initialize(self):
        """Initialize the component."""
        self._parse_variables()
        logger.info("Watcher component initialized")
    
    async def start(self):
        """Start the evaluation loop."""
        self._running = True
        self._eval_task = asyncio.create_task(self._evaluation_loop())
        logger.info("Watcher evaluation started")
    
    async def stop(self):
        """Stop the evaluation loop."""
        self._running = False
        if self._eval_task:
            self._eval_task.cancel()
            try:
                await self._eval_task
            except asyncio.CancelledError:
                pass
        logger.info("Watcher evaluation stopped")
    
    def set_nickname_map(self, nickname_map: Dict[str, str]):
        """Set the device nickname map (usually shared from ActionManager)."""
        self._nickname_map = nickname_map
    
    def _parse_variables(self):
        """Parse the variables JSON into the definitions dict."""
        try:
            raw = self.variables.get_value(0, 0)
            if raw:
                self._var_defs = json.loads(raw)
            else:
                self._var_defs = {}
        except json.JSONDecodeError as e:
            logger.error(f"Failed to parse variables: {e}")
            self._var_defs = {}
    
    def _on_variables_change(self, param, row, col, new_value, old_value):
        """Update variable definitions when changed."""
        self._parse_variables()
        logger.debug(f"Variables updated: {list(self._var_defs.keys())}")
    
    def _resolve_device(self, device: str) -> str:
        """Resolve device identifier."""
        if device.lower() == 'self':
            return 'self'
        if device in self._nickname_map:
            return self._nickname_map[device]
        return device
    
    async def _evaluation_loop(self):
        """Main loop for evaluating expressions."""
        while self._running:
            try:
                if not self.enabled.get_value(0, 0):
                    await asyncio.sleep(EVAL_INTERVAL_SEC)
                    continue
                
                # Refresh variable values
                await self._refresh_variables()
                
                # Evaluate all expressions
                for slot in range(NUM_WATCH_SLOTS):
                    expr = self.expressions.get_value(slot, 0)
                    if not expr:
                        continue
                    
                    try:
                        result = self._evaluate_expression(expr)
                        prev_result = self._prev_results.get(slot)
                        
                        # Check for edge transitions
                        if prev_result is not None:
                            if not prev_result and result:
                                # Rising edge: False -> True
                                await self._trigger_actions(slot, rising=True)
                            elif prev_result and not result:
                                # Falling edge: True -> False
                                await self._trigger_actions(slot, rising=False)
                        
                        self._prev_results[slot] = result
                        
                    except Exception as e:
                        logger.debug(f"Error evaluating expression slot {slot}: {e}")
                
                # Update eval count
                count = self.eval_count.get_value(0, 0)
                self.eval_count.set_value(0, 0, count + 1, notify=False)
                
                await asyncio.sleep(EVAL_INTERVAL_SEC)
                
            except asyncio.CancelledError:
                raise
            except Exception as e:
                logger.error(f"Error in evaluation loop: {e}")
                await asyncio.sleep(EVAL_INTERVAL_SEC)
    
    async def _refresh_variables(self):
        """Refresh all variable values from their sources."""
        if not self.hub:
            return
        
        for var_name, var_def in self._var_defs.items():
            try:
                device = self._resolve_device(var_def.get('device', 'self'))
                component_name = var_def.get('component')
                param_name = var_def.get('param')
                row = var_def.get('row', 0)
                col = var_def.get('col', 0)
                
                value = await self._get_param_value(device, component_name, param_name, row, col)
                
                if value is not None:
                    self._var_values[var_name] = value
                    
            except Exception as e:
                logger.debug(f"Error refreshing variable {var_name}: {e}")
    
    async def _get_param_value(self, device: str, component_name: str, 
                                param_name: str, row: int, col: int) -> Any:
        """Get a parameter value from a device."""
        if device == 'self':
            # Local component
            if self.hub and component_name in self.hub.local_components:
                comp = self.hub.local_components[component_name]
                param = comp.get_param(param_name)
                if param:
                    return param.get_value(row, col)
        else:
            # Remote device - check cached state
            if self.hub and device in self.hub.remote_state_cache:
                cached_state = self.hub.remote_state_cache[device]
                
                # Look for the parameter value in cached state
                key = f"{component_name}.{param_name}[{row},{col}]"
                if key in cached_state:
                    return cached_state[key]
        
        return None
    
    def _evaluate_expression(self, expr: str) -> bool:
        """
        Safely evaluate a logic expression with variable substitution.
        
        Only allows safe operators and variable references.
        """
        # Replace variable names with their values
        result_expr = expr
        
        for var_name, value in self._var_values.items():
            # Use word boundaries to avoid partial matches
            pattern = r'\b' + re.escape(var_name) + r'\b'
            
            # Format value appropriately
            if isinstance(value, bool):
                replacement = 'True' if value else 'False'
            elif isinstance(value, str):
                replacement = repr(value)
            else:
                replacement = str(value)
            
            result_expr = re.sub(pattern, replacement, result_expr)
        
        # Handle common keywords
        result_expr = re.sub(r'\btrue\b', 'True', result_expr, flags=re.IGNORECASE)
        result_expr = re.sub(r'\bfalse\b', 'False', result_expr, flags=re.IGNORECASE)
        result_expr = re.sub(r'\band\b', ' and ', result_expr, flags=re.IGNORECASE)
        result_expr = re.sub(r'\bor\b', ' or ', result_expr, flags=re.IGNORECASE)
        result_expr = re.sub(r'\bnot\b', ' not ', result_expr, flags=re.IGNORECASE)
        
        # Validate expression contains only safe characters
        allowed_chars = set('0123456789.+-*/()<=>&|! TrueFalsandor\t\n ')
        if not all(c in allowed_chars or c.isalpha() for c in result_expr):
            raise ValueError(f"Expression contains disallowed characters: {result_expr}")
        
        # Evaluate using safe eval with no builtins
        try:
            result = eval(result_expr, {"__builtins__": {}}, {})
            return bool(result)
        except Exception as e:
            raise ValueError(f"Failed to evaluate '{result_expr}': {e}")
    
    async def _trigger_actions(self, slot: int, rising: bool):
        """Trigger rising or falling edge actions for a slot."""
        if rising:
            action_json = self.rising_actions.get_value(slot, 0)
            edge_type = "rising"
        else:
            action_json = self.falling_actions.get_value(slot, 0)
            edge_type = "falling"
        
        if not action_json:
            return
        
        try:
            data = json.loads(action_json)
            actions = data.get('actions', [])
            
            if not actions:
                return
            
            logger.info(f"Triggering {edge_type} edge actions for slot {slot}: {len(actions)} actions")
            
            # Queue actions via ActionManager if available
            if self.hub:
                action_manager = self.hub.local_components.get('ActionManager')
                if action_manager:
                    action_manager._queue_actions(actions)
                else:
                    # Execute directly if no ActionManager
                    await self._execute_actions_directly(actions)
                    
        except json.JSONDecodeError as e:
            logger.error(f"Invalid action JSON for slot {slot}: {e}")
        except Exception as e:
            logger.error(f"Error triggering actions for slot {slot}: {e}")
    
    async def _execute_actions_directly(self, actions: List[Dict[str, Any]]):
        """Execute actions directly without ActionManager."""
        for action in actions:
            try:
                device = self._resolve_device(action.get('device', 'self'))
                
                if device == 'self':
                    await self._execute_local_action(action)
                else:
                    await self._execute_remote_action(device, action)
                
                # Handle wait
                wait_ms = action.get('wait_after_ms', 0)
                if wait_ms > 0:
                    await asyncio.sleep(wait_ms / 1000.0)
                    
            except Exception as e:
                logger.error(f"Error executing action directly: {e}")
    
    async def _execute_local_action(self, action: Dict[str, Any]):
        """Execute action on local component."""
        if not self.hub:
            return
        
        component_name = action.get('component')
        param_name = action.get('param')
        param_id = action.get('param_id')
        row = action.get('row', 0)
        col = action.get('col', 0)
        value = action.get('value')
        
        param = None
        
        if param_id is not None:
            for comp in self.hub.local_components.values():
                param = comp.get_param_by_id(param_id)
                if param:
                    break
        elif component_name and param_name:
            comp = self.hub.local_components.get(component_name)
            if comp:
                param = comp.get_param(param_name)
        
        if param and not param.read_only:
            param.set_value(row, col, value)
    
    async def _execute_remote_action(self, device_ip: str, action: Dict[str, Any]):
        """Execute action on remote device."""
        if not self.hub:
            return
        
        device = self.hub.devices.get(device_ip)
        if not device:
            return
        
        ws = device.websocket
        if not ws:
            return
        
        param_id = action.get('param_id')
        if param_id is None:
            return
        
        request = {
            'type': 'SET',
            'param_id': param_id,
            'row': action.get('row', 0),
            'col': action.get('col', 0),
            'value': action.get('value')
        }
        
        try:
            await ws.send(json.dumps(request))
        except Exception as e:
            logger.error(f"Failed to send action to {device_ip}: {e}")
    
    # Convenience methods
    
    def set_variable(self, name: str, device: str, component: str, 
                     param: str, row: int = 0, col: int = 0):
        """Add or update a variable definition."""
        self._var_defs[name] = {
            'device': device,
            'component': component,
            'param': param,
            'row': row,
            'col': col
        }
        self.variables.set_value(0, 0, json.dumps(self._var_defs))
    
    def set_watch(self, slot: int, expression: str, 
                  rising_actions: List[Dict] = None,
                  falling_actions: List[Dict] = None):
        """Set up a watch slot with expression and actions."""
        self.expressions.set_value(slot, 0, expression)
        
        if rising_actions:
            self.rising_actions.set_value(slot, 0, json.dumps({'actions': rising_actions}))
        
        if falling_actions:
            self.falling_actions.set_value(slot, 0, json.dumps({'actions': falling_actions}))
    
    def clear_watch(self, slot: int):
        """Clear a watch slot."""
        self.expressions.set_value(slot, 0, "")
        self.rising_actions.set_value(slot, 0, "")
        self.falling_actions.set_value(slot, 0, "")
        self._prev_results.pop(slot, None)
    
    def get_variable_value(self, name: str) -> Any:
        """Get the current cached value of a variable."""
        return self._var_values.get(name)
