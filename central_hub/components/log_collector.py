"""
LogCollector Component - Collects log output from all connected ESP32 devices.

Subscribes to the built-in 'log' string parameter on every component of every
ESP32, appends each received log line to a per-device .log file, and exposes
recent log content as a string parameter for the Django web frontend to read.

Log files are stored in central_hub/logs/<device_name>.log
"""

import asyncio
import json
import logging
import os
from collections import deque
from datetime import datetime
from pathlib import Path
from typing import Dict, Optional, TYPE_CHECKING

from .base import Component, StringParameter, IntParameter

if TYPE_CHECKING:
    from ..central_hub import CentralHub

logger = logging.getLogger('LogCollector')

# Directory for log files (relative to central_hub/)
LOG_DIR = Path(__file__).parent.parent / "logs"

# Max lines to keep in memory for the Django viewer
MAX_TAIL_LINES = 500

# Max log file size before rotation (5 MB)
MAX_LOG_FILE_SIZE = 5 * 1024 * 1024


class LogCollectorComponent(Component):
    """
    Collects log output from all connected ESP32 devices over WebSocket.
    
    Parameters:
        - device_filter (StringParameter): Which device's logs to view (IP or "all")
        - tail_lines (IntParameter): How many recent lines to return
        - log_content (StringParameter, read-only): Recent log lines for the selected device
        - log_devices (StringParameter, read-only): JSON list of devices with logs
    """
    
    def __init__(self):
        super().__init__("LogCollector")
        
        # Which device to show logs for (IP address or "all")
        self.device_filter = self.add_string_param(
            "device_filter",
            rows=1, cols=1,
            default_val="all"
        )
        
        # How many lines to tail
        self.tail_lines = self.add_int_param(
            "tail_lines",
            rows=1, cols=1,
            min_val=10, max_val=MAX_TAIL_LINES,
            default_val=100
        )
        
        # Recent log content (read-only, updated when device_filter changes)
        self.log_content = self.add_string_param(
            "log_content",
            rows=1, cols=1,
            default_val="",
            read_only=True
        )
        
        # List of devices that have log files (read-only, JSON array)
        self.log_devices = self.add_string_param(
            "log_devices",
            rows=1, cols=1,
            default_val="[]",
            read_only=True
        )
        
        # In-memory log buffers per device (deques for efficient tail)
        self._log_buffers: Dict[str, deque] = {}
        
        # File handles (opened lazily)
        self._log_files: Dict[str, object] = {}
        
        # Track which devices we've subscribed to
        self._subscribed_devices: set = set()
        
        # Register callbacks
        self.device_filter.on_change(self._on_filter_change)
        self.tail_lines.on_change(self._on_filter_change)
    
    async def initialize(self):
        """Create the logs directory."""
        LOG_DIR.mkdir(exist_ok=True)
        logger.info(f"Log directory: {LOG_DIR}")
        
        # Load existing log files into the device list
        self._update_device_list()
    
    async def stop(self):
        """Flush and close all log files."""
        for ip, f in self._log_files.items():
            try:
                f.close()
            except Exception:
                pass
        self._log_files.clear()
        logger.info("Log files flushed and closed")
    
    # ========================================================================
    # Device connection hooks
    # ========================================================================
    
    async def on_device_connected(self, ip: str):
        """Called when an ESP32 connects - subscribe to the 'log' param on every component."""
        hub = self.hub
        if not hub:
            logger.warning(f"[{ip}] LogCollector: no hub reference, cannot subscribe")
            return
        
        device = hub.devices.get(ip)
        if not device or not device.connected:
            logger.warning(f"[{ip}] LogCollector: device not found or not connected")
            return
        
        # Initialize buffer for this device
        if ip not in self._log_buffers:
            self._log_buffers[ip] = deque(maxlen=MAX_TAIL_LINES)
            self._load_existing_log(ip)
        
        logger.info(f"[{ip}] LogCollector: device has {len(device.components)} components: {list(device.components.keys())}")
        
        # Subscribe to the 'log' parameter on every component
        subscribed_any = False
        for comp_name, comp in device.components.items():
            log_param = comp.parameters.get('log')
            if not log_param:
                logger.debug(f"[{ip}] LogCollector: {comp_name} has no 'log' param (params: {list(comp.parameters.keys())})")
                continue
            
            try:
                logger.info(f"[{ip}] LogCollector: subscribing to {comp_name}.log (param_id={log_param.param_id})")
                result = await hub.subscribe_to_param(ip, comp_name, 'log', 0, 0)
                if result is not None and not (isinstance(result, dict) and '_subscription_error' in result):
                    subscribed_any = True
                    logger.info(f"[{ip}] ✅ Subscribed to {comp_name}.log")
                else:
                    logger.warning(f"[{ip}] ❌ Failed to subscribe to {comp_name}.log: {result}")
            except Exception as e:
                logger.error(f"[{ip}] Error subscribing to {comp_name}.log: {e}")
        
        if subscribed_any:
            self._subscribed_devices.add(ip)
            self._update_device_list()
    
    def on_device_disconnected(self, ip: str):
        """Called when an ESP32 disconnects - flush its log file."""
        self._subscribed_devices.discard(ip)
        if ip in self._log_files:
            try:
                self._log_files[ip].flush()
            except Exception:
                pass
    
    # ========================================================================
    # Log line processing
    # ========================================================================
    
    def process_log_update(self, ip: str, device_name: str, log_line: str, component_name: str = ''):
        """
        Called when a log parameter update is received from an ESP32.
        Appends to file and in-memory buffer.
        """
        if not log_line or log_line.strip() == '':
            return
        
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
        comp_tag = f" {component_name}:" if component_name else ""
        formatted_line = f"[{timestamp}]{comp_tag} {log_line}"
        
        # Append to in-memory buffer
        if ip not in self._log_buffers:
            self._log_buffers[ip] = deque(maxlen=MAX_TAIL_LINES)
        self._log_buffers[ip].append(formatted_line)
        
        # Append to log file
        self._write_to_file(ip, device_name, formatted_line)
        
        # Update log_content if this device is currently being viewed
        current_filter = self.device_filter.get_value(0, 0)
        if current_filter == ip or current_filter == 'all':
            self._refresh_log_content()
    
    def _write_to_file(self, ip: str, device_name: str, line: str):
        """Append a line to the device's log file."""
        # Use device_name for filename if available, otherwise IP
        filename = device_name if device_name else ip.replace('.', '_')
        filepath = LOG_DIR / f"{filename}.log"
        
        try:
            # Check file size for rotation
            if filepath.exists() and filepath.stat().st_size > MAX_LOG_FILE_SIZE:
                # Rotate: rename current to .log.old, start fresh
                old_path = filepath.with_suffix('.log.old')
                if old_path.exists():
                    old_path.unlink()
                filepath.rename(old_path)
                logger.info(f"Rotated log file for {filename}")
            
            # Open file if not already open (or re-open after rotation)
            if ip not in self._log_files or self._log_files[ip].closed:
                self._log_files[ip] = open(filepath, 'a', encoding='utf-8')
            
            self._log_files[ip].write(line + '\n')
            self._log_files[ip].flush()
            
        except Exception as e:
            logger.error(f"Failed to write log for {ip}: {e}")
    
    def _load_existing_log(self, ip: str):
        """Load the tail of an existing log file into the in-memory buffer."""
        # Find any log file matching this IP or device name
        for filepath in LOG_DIR.glob("*.log"):
            # Try to match by IP (converted to filename format)
            ip_filename = ip.replace('.', '_')
            if filepath.stem == ip_filename:
                try:
                    with open(filepath, 'r', encoding='utf-8') as f:
                        lines = f.readlines()
                        # Load last MAX_TAIL_LINES
                        for line in lines[-MAX_TAIL_LINES:]:
                            self._log_buffers[ip].append(line.rstrip('\n'))
                    logger.info(f"Loaded {len(self._log_buffers[ip])} existing log lines for {ip}")
                except Exception as e:
                    logger.error(f"Failed to load existing log for {ip}: {e}")
                break
    
    # ========================================================================
    # Log content for Django frontend
    # ========================================================================
    
    def _on_filter_change(self, param, row, col, value, old_value):
        """Update log_content when the filter or line count changes."""
        # Handle clear command: "clear:all" or "clear:10.0.0.46"
        if isinstance(value, str) and value.startswith('clear:'):
            target = value[len('clear:'):]
            device_ip = None if target == 'all' else target
            self.clear_logs(device_ip)
            # Reset filter to the target (or "all")
            self.device_filter.set_value(0, 0, target if target else 'all', notify=False)
            return
        self._refresh_log_content()
    
    def _refresh_log_content(self):
        """Rebuild the log_content parameter based on current filter."""
        current_filter = self.device_filter.get_value(0, 0)
        num_lines = self.tail_lines.get_value(0, 0) or 100
        
        lines = []
        
        if current_filter == 'all':
            # Merge all device logs (interleaved by timestamp)
            all_lines = []
            for ip, buf in self._log_buffers.items():
                device = self._get_device_name(ip)
                for line in buf:
                    all_lines.append(f"[{device}] {line}")
            # Sort by timestamp (which is the first part of each line)
            all_lines.sort()
            lines = all_lines[-num_lines:]
        else:
            # Single device
            buf = self._log_buffers.get(current_filter, deque())
            lines = list(buf)[-num_lines:]
        
        content = '\n'.join(lines)
        self.log_content.set_value(0, 0, content, notify=True)
    
    def _update_device_list(self):
        """Update the log_devices parameter with available devices."""
        devices = []
        
        # From in-memory buffers (currently connected)
        for ip in self._log_buffers:
            name = self._get_device_name(ip)
            devices.append({'ip': ip, 'name': name, 'connected': ip in self._subscribed_devices})
        
        # From log files on disk (may include disconnected devices)
        for filepath in LOG_DIR.glob("*.log"):
            ip_from_file = filepath.stem.replace('_', '.')
            if not any(d['ip'] == ip_from_file for d in devices):
                devices.append({'ip': ip_from_file, 'name': filepath.stem, 'connected': False})
        
        self.log_devices.set_value(0, 0, json.dumps(devices), notify=True)
    
    def _get_device_name(self, ip: str) -> str:
        """Get the friendly name for a device."""
        if self.hub and ip in self.hub.devices:
            device = self.hub.devices[ip]
            return device.name or device.hostname or ip
        return ip
    
    # ========================================================================
    # Public API for Django
    # ========================================================================
    
    def get_log_content(self, device_ip: str = 'all', num_lines: int = 100) -> str:
        """Get log content for a specific device (called directly by Django views)."""
        if device_ip == 'all':
            all_lines = []
            for ip, buf in self._log_buffers.items():
                name = self._get_device_name(ip)
                for line in buf:
                    all_lines.append(f"[{name}] {line}")
            all_lines.sort()
            return '\n'.join(all_lines[-num_lines:])
        else:
            buf = self._log_buffers.get(device_ip, deque())
            return '\n'.join(list(buf)[-num_lines:])
    
    def get_device_list(self) -> list:
        """Get list of devices with logs (called directly by Django views)."""
        try:
            return json.loads(self.log_devices.get_value(0, 0) or '[]')
        except (json.JSONDecodeError, TypeError):
            return []
    
    def clear_logs(self, device_ip: str = None):
        """Clear log buffer and file for a device (or all devices)."""
        if device_ip:
            if device_ip in self._log_buffers:
                self._log_buffers[device_ip].clear()
            # Delete log file
            for filepath in LOG_DIR.glob("*.log"):
                ip_from_file = filepath.stem.replace('_', '.')
                if ip_from_file == device_ip:
                    filepath.unlink()
                    break
        else:
            self._log_buffers.clear()
            for filepath in LOG_DIR.glob("*.log"):
                filepath.unlink()
        
        self._refresh_log_content()
        self._update_device_list()
