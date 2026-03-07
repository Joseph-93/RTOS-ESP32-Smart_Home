"""
Watcher Component - Monitors variables and triggers actions on expression changes.

Evaluates logic expressions every 100ms and triggers rising/falling edge actions
when expression results change from False->True or True->False.
"""

import asyncio
import json
import logging
import re
import time
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
            "comp": "LightSensor",
            "param": "current_light_level",
            "row": 0,
            "col": 0
        },
        "motion_detected": {
            "device": "ESP32_Motion",
            "comp": "MotionSensor", 
            "param": "motion_state",
            "row": 0,
            "col": 0
        }
    }
    
    Expression format (string):
    "main_light > 50 and motion_detected == true"
    
    Supported operators: and, or, not, ==, !=, <, >, <=, >=, +, -, *, /, (, )
    
    Actions format - Simple mode (JSON):
    {
        "actions": [
            {"device": "self", "param_id": 5, "row": 0, "col": 0, "value": 1, "wait_after_ms": 0}
        ]
    }
    
    Actions format - Cycle mode (JSON):
    {
        "cycle": [
            {"actions": [...]},  # Executed on 1st trigger
            {"actions": [...]},  # Executed on 2nd trigger
            {"actions": [...]}   # Executed on 3rd trigger, then wraps to 1st
        ]
    }
    
    Cycle mode enables toggle (2 sets) or multi-state (N sets) behavior with a single button.
    The rising_cycle_index/falling_cycle_index params track which set fires next.
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
        
        # Hold-high time per slot (seconds) - expression result stays TRUE for at least this long
        self.hold_high_sec = self.add_float_param(
            "hold_high_sec",
            rows=NUM_WATCH_SLOTS, cols=1,
            min_val=0.0, max_val=86400.0,  # Up to 24 hours
            default_val=0.0
        )
        
        # Cooldown time per slot (seconds) - after going FALSE, can't go TRUE again for this long
        self.cooldown_sec = self.add_float_param(
            "cooldown_sec",
            rows=NUM_WATCH_SLOTS, cols=1,
            min_val=0.0, max_val=86400.0,  # Up to 24 hours
            default_val=0.0
        )
        
        # Cycle index for rising actions (tracks which action set to run next)
        # Read-only: managed internally, but visible for debugging
        self.rising_cycle_index = self.add_int_param(
            "rising_cycle_index",
            rows=NUM_WATCH_SLOTS, cols=1,
            min_val=0, max_val=999,
            default_val=0,
            read_only=True
        )
        
        # Cycle index for falling actions
        self.falling_cycle_index = self.add_int_param(
            "falling_cycle_index",
            rows=NUM_WATCH_SLOTS, cols=1,
            min_val=0, max_val=999,
            default_val=0,
            read_only=True
        )
        
        # Parsed variable definitions
        self._var_defs: Dict[str, Dict[str, Any]] = {}
        
        # Current variable values cache
        self._var_values: Dict[str, Any] = {}
        
        # Previous expression results for edge detection
        self._prev_results: Dict[int, bool] = {}
        
        # Timing state per slot
        self._hold_until: Dict[int, float] = {}  # slot -> timestamp when hold-high ends
        self._cooldown_until: Dict[int, float] = {}  # slot -> timestamp when cooldown ends
        
        # Background task
        self._eval_task: Optional[asyncio.Task] = None
        self._running = False
        
        # Device nickname map (shared reference from ActionManager if available)
        self._nickname_map: Dict[str, str] = {}
        
        # Track stale variables for recovery
        # var_name -> {'last_seen': timestamp, 'recovery_attempts': count, 'last_recovery': timestamp}
        self._var_health: Dict[str, Dict[str, Any]] = {}
        
        # Track subscription errors (e.g., "index out of bounds")
        # var_name -> error message string
        self._var_errors: Dict[str, str] = {}
        
        # Track logged slot errors to avoid spam (slot -> last_error_message)
        self._logged_slot_errors: Dict[int, str] = {}
        
        # Recovery settings
        self._STALE_THRESHOLD_SEC = 10.0  # Consider variable stale after 10 seconds without value
        self._RECOVERY_COOLDOWN_SEC = 30.0  # Don't spam recovery attempts
        self._MAX_RECOVERY_ATTEMPTS = 3  # After this many failures, try reconnect
        
        # Register callbacks
        self.variables.on_change(self._on_variables_change)
    
    async def initialize(self):
        """Initialize the component."""
        logger.debug("🚀 WATCHER INITIALIZING...")
        self._parse_variables()
        
        # Log current expressions
        active_count = 0
        for slot in range(NUM_WATCH_SLOTS):
            expr = self.expressions.get_value(slot, 0)
            if expr:
                active_count += 1
                rising = self.rising_actions.get_value(slot, 0)
                falling = self.falling_actions.get_value(slot, 0)
                logger.debug(f"   📝 Slot {slot}: expr='{expr}' rising={bool(rising)} falling={bool(falling)}")
        
        logger.debug(f"🚀 WATCHER INITIALIZED: {len(self._var_defs)} variables, {active_count} expressions")
    
    async def start(self):
        """Start the evaluation loop."""
        # Re-parse variables in case persistence loaded data after initialize()
        self._parse_variables()
        
        self._running = True
        self._eval_task = asyncio.create_task(self._evaluation_loop())
        logger.debug(f"✅ WATCHER STARTED - evaluating every {EVAL_INTERVAL_SEC*1000:.0f}ms")
    
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
    
    def on_device_reconnected(self, device_ip: str):
        """Called when a remote device reconnects - clears stale tracking for its variables.
        
        This allows the Watcher to immediately start receiving fresh values
        instead of waiting for recovery timeouts.
        """
        cleared_count = 0
        for var_name, var_def in self._var_defs.items():
            device = self._resolve_device(var_def.get('device', 'self'))
            if device == device_ip:
                # Clear health tracking - variable is fresh again
                if var_name in self._var_health:
                    del self._var_health[var_name]
                    cleared_count += 1
                # Clear any subscription errors - give it a fresh chance
                self._var_errors.pop(var_name, None)
        
        # Clear logged slot errors so new issues get logged fresh
        self._logged_slot_errors.clear()
        
        if cleared_count > 0:
            logger.info(f"🔄 Device {device_ip} reconnected - cleared stale tracking for {cleared_count} variables")
    
    def _parse_variables(self):
        """Parse the variables JSON into the definitions dict."""
        try:
            raw = self.variables.get_value(0, 0)
            if raw:
                self._var_defs = json.loads(raw)
                logger.debug(f"📋 WATCHER PARSED VARIABLES: {len(self._var_defs)} variables defined")
                for var_name, var_def in self._var_defs.items():
                    logger.debug(f"   📌 {var_name} => {var_def.get('device')}/{var_def.get('component')}.{var_def.get('param')}[{var_def.get('row',0)}][{var_def.get('col',0)}]")
            else:
                self._var_defs = {}
                logger.debug("📋 WATCHER PARSED VARIABLES: empty (no variables defined)")
        except json.JSONDecodeError as e:
            logger.error(f"❌ WATCHER FAILED TO PARSE VARIABLES: {e}")
            self._var_defs = {}
    
    def _on_variables_change(self, param, row, col, new_value, old_value):
        """Update variable definitions when changed and trigger subscriptions for new variables."""
        logger.debug(f"🔄 WATCHER VARIABLES CHANGED - re-parsing...")
        old_vars = set(self._var_defs.keys())
        self._parse_variables()
        new_vars = set(self._var_defs.keys())
        
        # Find newly added variables and trigger subscriptions
        added_vars = new_vars - old_vars
        if added_vars and self.hub:
            logger.debug(f"📡 New variables added, requesting subscriptions: {added_vars}")
            asyncio.create_task(self._subscribe_to_new_variables(added_vars))
    
    async def _subscribe_to_new_variables(self, var_names: set):
        """Subscribe to remote parameters for newly added variables."""
        for var_name in var_names:
            var_def = self._var_defs.get(var_name)
            if not var_def:
                continue
            
            device = self._resolve_device(var_def.get('device', 'self'))
            if device == 'self':
                continue  # Local variables don't need subscriptions
            
            comp = var_def.get('component')
            param = var_def.get('param')
            row = var_def.get('row', 0)
            col = var_def.get('col', 0)
            
            if comp and param:
                logger.debug(f"📡 Subscribing to {device}/{comp}.{param}[{row}][{col}] for variable '{var_name}'")
                result = await self.hub.subscribe_to_param(device, comp, param, row, col)
                
                # Check if subscription returned an error dict
                if isinstance(result, dict) and '_subscription_error' in result:
                    error_msg = result['_subscription_error']
                    self._var_errors[var_name] = error_msg
                    logger.warning(f"❌ {var_name}: subscription error - {error_msg}")
                elif result is not None:
                    self._var_values[var_name] = result
                    # Clear any previous error
                    self._var_errors.pop(var_name, None)
                    logger.debug(f"✅ {var_name} = {result}")
    
    def _resolve_device(self, device: str) -> str:
        """Resolve device identifier - could be IP, nickname, device_id, or 'self'.
        
        Resolution order:
        1. 'self' -> 'self'
        2. nickname -> IP (from nickname map, shared with ActionManager)
        3. device_id -> IP (from hub's devices_by_id)
        4. IP -> IP (passthrough)
        """
        if device.lower() == 'self':
            return 'self'
        if device in self._nickname_map:
            return self._nickname_map[device]
        # Check if it's a device_id (hub tracks these)
        if self.hub and hasattr(self.hub, 'devices_by_id'):
            if device in self.hub.devices_by_id:
                return self.hub.devices_by_id[device].ip
        return device
    
    def _apply_timing_logic(self, slot: int, raw_result: bool, now: float) -> bool:
        """
        Apply hold-high and cooldown timing logic to a raw expression result.
        
        Hold-high: When result goes TRUE, it stays TRUE for at least hold_high_sec,
                   even if the raw expression goes FALSE.
        
        Cooldown: After result goes FALSE (including after hold-high ends),
                  it cannot go TRUE again for cooldown_sec.
        
        Returns the effective (modified) result.
        """
        hold_sec = self.hold_high_sec.get_value(slot, 0)
        cool_sec = self.cooldown_sec.get_value(slot, 0)
        
        # Get previous effective result
        prev_result = self._prev_results.get(slot, False)
        
        # Check if we're in hold-high period
        hold_until = self._hold_until.get(slot, 0)
        in_hold = now < hold_until
        
        # Check if we're in cooldown period
        cooldown_until = self._cooldown_until.get(slot, 0)
        in_cooldown = now < cooldown_until
        
        # Determine effective result
        if in_hold:
            # Still in hold-high period - force TRUE
            return True
        
        if in_cooldown:
            # In cooldown period - force FALSE (can't go high yet)
            return False
        
        # Not in any timing period - use raw result
        if raw_result and not prev_result:
            # Rising edge - start hold-high timer if configured
            if hold_sec > 0:
                self._hold_until[slot] = now + hold_sec
        
        elif not raw_result and prev_result:
            # Falling edge (or hold-high just ended) - start cooldown if configured
            if cool_sec > 0:
                self._cooldown_until[slot] = now + cool_sec
        
        return raw_result

    async def _evaluation_loop(self):
        """Main loop for evaluating expressions."""
        loop_count = 0
        while self._running:
            try:
                if not self.enabled.get_value(0, 0):
                    await asyncio.sleep(EVAL_INTERVAL_SEC)
                    continue
                
                # Refresh variable values
                await self._refresh_variables()
                
                # Log variable values periodically (every 50 iterations = 5 seconds)
                loop_count += 1
                if loop_count % 50 == 1:
                    if self._var_defs:
                        logger.debug(f"👁️ WATCHER MONITORING {len(self._var_defs)} variables:")
                        for var_name, var_def in self._var_defs.items():
                            if var_name in self._var_values:
                                logger.debug(f"   📊 {var_name} = {self._var_values[var_name]}")
                            else:
                                device = var_def.get('device', '?')
                                comp = var_def.get('component', '?')
                                param = var_def.get('param', '?')
                                logger.debug(f"   ❌ {var_name} = NO VALUE (source: {device}/{comp}.{param})")
                    else:
                        logger.debug(f"👁️ WATCHER MONITORING: no variables defined")
                
                # Evaluate all expressions
                active_expressions = 0
                now = time.time()
                
                for slot in range(NUM_WATCH_SLOTS):
                    expr = self.expressions.get_value(slot, 0)
                    if not expr:
                        continue
                    
                    active_expressions += 1
                    
                    try:
                        # Get raw expression result
                        raw_result = self._evaluate_expression(expr)
                        
                        # Apply hold-high and cooldown logic
                        result = self._apply_timing_logic(slot, raw_result, now)
                        
                        prev_result = self._prev_results.get(slot)
                        
                        # Log expression evaluation periodically
                        if loop_count % 50 == 1:
                            hold_sec = self.hold_high_sec.get_value(slot, 0)
                            cool_sec = self.cooldown_sec.get_value(slot, 0)
                            timing_info = ""
                            if hold_sec > 0 or cool_sec > 0:
                                timing_info = f" [hold={hold_sec}s, cool={cool_sec}s]"
                            if raw_result != result:
                                logger.debug(f"   🧮 Slot {slot}: '{expr}' => raw={raw_result}, effective={result} (prev={prev_result}){timing_info}")
                            else:
                                logger.debug(f"   🧮 Slot {slot}: '{expr}' => {result} (prev={prev_result}){timing_info}")
                        
                        # Check for edge transitions
                        if prev_result is not None:
                            if not prev_result and result:
                                # Rising edge: False -> True
                                logger.debug(f"🔺 RISING EDGE slot {slot}: '{expr}'")
                                await self._trigger_actions(slot, rising=True)
                            elif prev_result and not result:
                                # Falling edge: True -> False
                                logger.debug(f"🔻 FALLING EDGE slot {slot}: '{expr}'")
                                await self._trigger_actions(slot, rising=False)
                        
                        self._prev_results[slot] = result
                        
                        # Clear any logged error since this slot is working now
                        self._logged_slot_errors.pop(slot, None)
                        
                    except Exception as e:
                        # Only log if this is a NEW error or different from last logged
                        error_msg = str(e)
                        if self._logged_slot_errors.get(slot) != error_msg:
                            logger.warning(f"   ⚠️ Slot {slot}: '{expr}' => ERROR: {e}")
                            self._logged_slot_errors[slot] = error_msg
                
                if loop_count % 50 == 1 and active_expressions == 0:
                    logger.debug(f"   ⚠️ No active expressions configured")
                
                # Update eval count
                count = self.eval_count.get_value(0, 0)
                self.eval_count.set_value(0, 0, count + 1, notify=False)
                
                await asyncio.sleep(EVAL_INTERVAL_SEC)
                
            except asyncio.CancelledError:
                raise
            except Exception as e:
                logger.error(f"❌ Error in evaluation loop: {e}")
                await asyncio.sleep(EVAL_INTERVAL_SEC)
    
    async def _refresh_variables(self):
        """Refresh all variable values from their sources, with active recovery for stale variables."""
        if not self.hub:
            return
        
        now = time.time()
        
        for var_name, var_def in self._var_defs.items():
            try:
                device = self._resolve_device(var_def.get('device', 'self'))
                component_name = var_def.get('component')
                param_name = var_def.get('param')
                row = var_def.get('row', 0)
                col = var_def.get('col', 0)
                
                value = await self._get_param_value(device, component_name, param_name, row, col)
                
                if value is not None:
                    old_value = self._var_values.get(var_name)
                    if old_value != value:
                        logger.debug(f"📝 {var_name} changed: {old_value} -> {value}")
                    self._var_values[var_name] = value
                    
                    # Mark variable as healthy
                    self._var_health[var_name] = {'last_seen': now, 'recovery_attempts': 0, 'last_recovery': 0}
                else:
                    # Variable has no value - track and attempt recovery
                    await self._handle_missing_variable(var_name, var_def, device, component_name, param_name, row, col, now)
                    
            except Exception as e:
                logger.debug(f"Error refreshing variable {var_name}: {e}")
    
    async def _handle_missing_variable(self, var_name: str, var_def: dict, device: str, 
                                        component_name: str, param_name: str, row: int, col: int, now: float):
        """Handle a variable that has no value - track staleness and attempt recovery."""
        if device == 'self':
            # Local variables should always have values - log error but don't try to recover
            logger.warning(f"⚠️ Local variable {var_name} has no value: {component_name}.{param_name}")
            return
        
        # If this variable has a known config error (like "out of bounds"), don't retry
        # The user needs to fix the variable configuration
        if var_name in self._var_errors:
            error = self._var_errors[var_name]
            if 'out of bounds' in error.lower() or 'not found' in error.lower():
                # Config error - don't keep spamming retry attempts
                return
        
        # Initialize health tracking if needed
        if var_name not in self._var_health:
            self._var_health[var_name] = {'last_seen': 0, 'recovery_attempts': 0, 'last_recovery': 0}
        
        health = self._var_health[var_name]
        time_since_seen = now - health['last_seen'] if health['last_seen'] > 0 else float('inf')
        time_since_recovery = now - health['last_recovery']
        
        # Skip if variable was seen recently (just a momentary glitch)
        if time_since_seen < self._STALE_THRESHOLD_SEC:
            return
        
        # Skip if we tried recovery recently
        if time_since_recovery < self._RECOVERY_COOLDOWN_SEC:
            return
        
        # Check if device is connected
        if not self.hub.is_device_connected(device):
            # Device disconnected - force reconnect
            if health['recovery_attempts'] >= self._MAX_RECOVERY_ATTEMPTS:
                logger.warning(f"🔌 {var_name}: device {device} offline, forcing reconnect (attempt {health['recovery_attempts'] + 1})")
                await self.hub.request_reconnect(device)
                health['recovery_attempts'] = 0  # Reset after reconnect attempt
            else:
                logger.debug(f"⚠️ {var_name}: device {device} not connected, waiting for auto-reconnect")
            health['last_recovery'] = now
            return
        
        # Device is connected but param has no value - try to re-subscribe
        logger.debug(f"🔄 {var_name}: attempting to re-subscribe to {device}/{component_name}.{param_name}[{row}][{col}]")
        
        result = await self.hub.subscribe_param_by_name(device, component_name, param_name, row, col)
        health['last_recovery'] = now
        health['recovery_attempts'] += 1
        
        # Check if subscription returned an error dict
        if isinstance(result, dict) and '_subscription_error' in result:
            error_msg = result['_subscription_error']
            self._var_errors[var_name] = error_msg
            logger.warning(f"❌ {var_name}: re-subscribe error - {error_msg}")
            # Don't keep retrying if it's a config error like "index out of bounds"
            if 'out of bounds' in error_msg.lower():
                health['recovery_attempts'] = 0  # Stop retrying - it's a config issue
        elif result is not None:
            logger.debug(f"✅ {var_name}: recovered! value = {result}")
            self._var_values[var_name] = result
            self._var_errors.pop(var_name, None)  # Clear error
            health['recovery_attempts'] = 0
            health['last_seen'] = now
        else:
            logger.warning(f"❌ {var_name}: re-subscribe failed (attempt {health['recovery_attempts']})")
            
            # Too many failures - try full reconnect
            if health['recovery_attempts'] >= self._MAX_RECOVERY_ATTEMPTS:
                logger.warning(f"🔌 {var_name}: too many failures, forcing device reconnect")
                await self.hub.request_reconnect(device)
                health['recovery_attempts'] = 0
    
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
                    logger.debug(f"⚠️ Local param not found: {component_name}.{param_name}")
            else:
                logger.debug(f"⚠️ Local component not found: {component_name}")
        else:
            # Remote device - check cached state
            if self.hub and device in self.hub.remote_state_cache:
                cached_state = self.hub.remote_state_cache[device]
                
                # Look for the parameter value in cached state
                key = f"{component_name}.{param_name}[{row},{col}]"
                if key in cached_state:
                    return cached_state[key]
                else:
                    logger.debug(f"⚠️ Remote param not in cache: {device}/{key} (cache has {len(cached_state)} keys)")
            else:
                if self.hub:
                    logger.debug(f"⚠️ Device not in remote_state_cache: {device} (available: {list(self.hub.remote_state_cache.keys())})")
                else:
                    logger.debug(f"⚠️ No hub reference")
        
        return None
    
    def _evaluate_expression(self, expr: str) -> bool:
        """
        Safely evaluate a logic expression with variable substitution.
        
        Supports:
        - Boolean variables used directly: `Button1` means `Button1 == True`
        - Comparison operators: ==, !=, <, >, <=, >=
        - Logic operators: and, or, not
        - Parentheses for grouping: (Button1 or Button2) and ActionsOn
        - Numeric comparisons: sensor_value > 50
        
        Order of operations (PEMDAS-like):
        1. Parentheses (innermost first)
        2. not
        3. Comparisons (==, !=, <, >, <=, >=)
        4. and
        5. or
        """
        # FIRST: Find all variable names used in the expression and check they have values
        missing_vars = []
        used_vars = set()
        
        for var_name, var_def in self._var_defs.items():
            # Check if this variable is used in the expression
            pattern = r'\b' + re.escape(var_name) + r'\b'
            if re.search(pattern, expr):
                used_vars.add(var_name)
                # Variable is used - check if we have a value for it
                if var_name not in self._var_values:
                    device = var_def.get('device', 'unknown')
                    comp = var_def.get('component', 'unknown')
                    param = var_def.get('param', 'unknown')
                    row = var_def.get('row', 0)
                    col = var_def.get('col', 0)
                    
                    # Check if there's a specific subscription error
                    sub_error = self._var_errors.get(var_name)
                    if sub_error:
                        missing_vars.append(f"'{var_name}' ({device}/{comp}.{param}[{row}][{col}] - {sub_error})")
                    elif device == 'self':
                        missing_vars.append(f"'{var_name}' (local: {comp}.{param} - component may not exist)")
                    else:
                        missing_vars.append(f"'{var_name}' (remote: {device}/{comp}.{param}[{row}][{col}] - device may be disconnected)")
        
        if missing_vars:
            raise ValueError(f"Variables have no value: {', '.join(missing_vars)}")
        
        # Pre-process expression to handle bare boolean variables
        # A variable that's not followed by an operator should be treated as `var == True`
        result_expr = self._preprocess_bare_booleans(expr, used_vars)
        
        # Replace variable names with their values
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
        
        # Handle common keywords (true/false/and/or/not)
        result_expr = re.sub(r'\btrue\b', 'True', result_expr, flags=re.IGNORECASE)
        result_expr = re.sub(r'\bfalse\b', 'False', result_expr, flags=re.IGNORECASE)
        result_expr = re.sub(r'\band\b', ' and ', result_expr, flags=re.IGNORECASE)
        result_expr = re.sub(r'\bor\b', ' or ', result_expr, flags=re.IGNORECASE)
        result_expr = re.sub(r'\bnot\b', ' not ', result_expr, flags=re.IGNORECASE)
        
        # Clean up multiple spaces
        result_expr = re.sub(r'\s+', ' ', result_expr).strip()
        
        # Validate expression contains only safe characters
        allowed_chars = set('0123456789.+-*/()<=>&|! TrueFalsandor\t\n "\'')
        if not all(c in allowed_chars or c.isalpha() for c in result_expr):
            raise ValueError(f"Expression contains disallowed characters: {result_expr}")
        
        # Evaluate using safe eval with no builtins
        try:
            result = eval(result_expr, {"__builtins__": {}}, {})
            return bool(result)
        except NameError as e:
            # This should never happen now, but catch it just in case
            raise ValueError(f"Undefined variable in expression: {e}")
        except SyntaxError as e:
            raise ValueError(f"Syntax error in expression '{result_expr}': {e}")
        except Exception as e:
            raise ValueError(f"Failed to evaluate '{result_expr}': {e}")
    
    def _preprocess_bare_booleans(self, expr: str, var_names: set) -> str:
        """
        Pre-process expression to handle bare boolean variables.
        
        Converts:
          - "Button1 and ActionsOn" -> "(Button1 == True) and (ActionsOn == True)"
          - "not Button1" -> "not (Button1 == True)"
          - "Button1 == false" -> stays as is (already has comparison)
          
        Variables followed by a comparison operator are left alone.
        """
        if not var_names:
            return expr
        
        result = expr
        
        # Sort by length descending to handle longer names first (avoid partial matches)
        sorted_vars = sorted(var_names, key=len, reverse=True)
        
        for var_name in sorted_vars:
            # Pattern: variable name NOT followed by a comparison operator
            # We want to match: VarName that is NOT followed by ==, !=, <, >, <=, >=
            # But IS followed by: end of string, whitespace, ), 'and', 'or', etc.
            
            # First, find all occurrences of the variable
            pattern = r'\b' + re.escape(var_name) + r'\b'
            
            # Check each match to see if it needs wrapping
            def replace_bare(match):
                start = match.start()
                end = match.end()
                
                # Check what comes after (skip whitespace)
                after = result[end:].lstrip()
                
                # If followed by a comparison operator, leave it alone
                if after.startswith(('==', '!=', '<=', '>=', '<', '>')):
                    return match.group(0)
                
                # Otherwise, wrap it as a boolean check
                return f'({var_name} == True)'
            
            result = re.sub(pattern, replace_bare, result)
        
        return result
    
    async def _trigger_actions(self, slot: int, rising: bool):
        """Trigger rising or falling edge actions for a slot.
        
        Supports two action formats:
        1. Simple (original): {"actions": [...]}
           - Executes all actions every trigger
        
        2. Cycle mode: {"cycle": [{"actions": [...]}, {"actions": [...]}]}
           - Each trigger executes the NEXT set in the cycle
           - Wraps around after the last set
           - Enables toggle (2 sets) or multi-state (N sets) behavior
        """
        if rising:
            action_json = self.rising_actions.get_value(slot, 0)
            cycle_param = self.rising_cycle_index
            edge_type = "RISING"
        else:
            action_json = self.falling_actions.get_value(slot, 0)
            cycle_param = self.falling_cycle_index
            edge_type = "FALLING"
        
        if not action_json:
            logger.debug(f"⚠️ {edge_type} edge slot {slot}: no actions configured")
            return
        
        try:
            data = json.loads(action_json)
            
            # Check for cycle mode vs simple mode
            if 'cycle' in data:
                # Cycle mode: get current index and select that action set
                cycle_list = data['cycle']
                if not cycle_list:
                    logger.debug(f"⚠️ {edge_type} edge slot {slot}: cycle list is empty")
                    return
                
                current_index = cycle_param.get_value(slot, 0) if cycle_param else 0
                
                # Clamp index in case cycle was shortened
                if current_index >= len(cycle_list):
                    current_index = 0
                
                # Get the action set for this cycle position
                action_set = cycle_list[current_index]
                actions = action_set.get('actions', [])
                
                # Advance to next index (wrap around)
                next_index = (current_index + 1) % len(cycle_list)
                if cycle_param:
                    cycle_param.set_value(slot, 0, next_index)
                
                logger.debug(f"🔄 {edge_type} edge slot {slot}: CYCLE mode, index {current_index}/{len(cycle_list)-1} → {next_index}")
            else:
                # Simple mode: just get the actions list
                actions = data.get('actions', [])
            
            if not actions:
                logger.debug(f"⚠️ {edge_type} edge slot {slot}: actions list is empty")
                return
            
            logger.debug(f"🎯 Triggering {edge_type} edge slot {slot}: {len(actions)} actions")
            for i, action in enumerate(actions):
                logger.debug(f"   📤 Action {i}: {json.dumps(action)}")
            
            # Queue actions via ActionManager if available
            if self.hub:
                action_manager = self.hub.local_components.get('ActionManager')
                if action_manager:
                    logger.debug(f"   ➡️ Queuing to ActionManager")
                    action_manager._queue_actions(actions)
                else:
                    logger.debug(f"   ➡️ No ActionManager, executing directly")
                    # Execute directly if no ActionManager
                    await self._execute_actions_directly(actions)
                    
        except json.JSONDecodeError as e:
            logger.error(f"❌ Invalid action JSON for slot {slot}: {e}")
        except Exception as e:
            logger.error(f"❌ Error triggering actions for slot {slot}: {e}")
    
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
            'type': 'set_param',
            'param_id': param_id,
            'row': action.get('row', 0),
            'col': action.get('col', 0),
            'value': action.get('value')
        }
        
        try:
            request_json = json.dumps(request)
            logger.debug(f"📤 WATCHER SENDING TO {device_ip}: {request_json}")
            await ws.send(request_json)
        except Exception as e:
            logger.error(f"❌ Failed to send action to {device_ip}: {e}")
    
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
