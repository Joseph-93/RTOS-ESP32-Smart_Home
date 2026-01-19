"""
ESP32 Communication Manager
Handles HTTP REST API communication with ESP32 web server
"""
import requests
import json
from typing import Dict, Any, Optional, List


class ESP32Device:
    """Represents a single ESP32 device connection via HTTP REST API"""
    
    def __init__(self, host: str, port: int = 80):
        self.host = host
        self.port = port
        self.base_url = f"http://{host}:{port}"
        self.session = requests.Session()
        self.session.headers.update({'Content-Type': 'application/json'})
        
    def get_components(self) -> List[str]:
        """Get list of all components via GET /api/components"""
        try:
            response = self.session.get(f"{self.base_url}/api/components", timeout=5)
            response.raise_for_status()
            data = response.json()
            print(f"[ESP32 {self.host}] Components: {data}")
            return data.get("components", [])
        except Exception as e:
            print(f"[ESP32 {self.host}] Error getting components: {e}")
            return []
    
    def get_param_info(self, component: str, param_type: str = None) -> Optional[Dict[str, Any]]:
        """
        Get parameter info for a component
        GET /api/param_info?comp=ComponentName&type=int|float|bool|str|actions
        """
        try:
            params = {'comp': component}
            if param_type:
                params['type'] = param_type
            
            response = self.session.get(f"{self.base_url}/api/param_info", params=params, timeout=5)
            response.raise_for_status()
            data = response.json()
            print(f"[ESP32 {self.host}] Param info for {component}/{param_type}: {data}")
            return data
        except Exception as e:
            print(f"[ESP32 {self.host}] Error getting param info: {e}")
            return None
    
    def get_param_value(self, comp: str, param_type: str, idx: int, row: int, col: int) -> Any:
        """
        Get specific parameter value
        GET /api/get_param?comp=X&type=Y&idx=0&row=0&col=0
        """
        try:
            params = {
                'comp': comp,
                'type': param_type,
                'idx': idx,
                'row': row,
                'col': col
            }
            response = self.session.get(f"{self.base_url}/api/get_param", params=params, timeout=5)
            response.raise_for_status()
            data = response.json()
            print(f"[ESP32 {self.host}] Get param {comp}/{param_type}[{idx}][{row}][{col}]: {data}")
            return data.get("value")
        except Exception as e:
            print(f"[ESP32 {self.host}] Error getting param value: {e}")
            return None
    
    def set_param_value(self, comp: str, param_type: str, idx: int, row: int, col: int, value: Any) -> bool:
        """
        Set specific parameter value
        POST /api/set_param
        Body: {comp, type, idx, row, col, value}
        """
        try:
            payload = {
                'comp': comp,
                'type': param_type,
                'idx': idx,
                'row': row,
                'col': col,
                'value': value
            }
            response = self.session.post(f"{self.base_url}/api/set_param", json=payload, timeout=5)
            response.raise_for_status()
            data = response.json()
            print(f"[ESP32 {self.host}] Set param {comp}/{param_type}[{idx}][{row}][{col}]={value}: {data}")
            return data.get("success", False)
        except Exception as e:
            print(f"[ESP32 {self.host}] Error setting param value: {e}")
            return False
    
    def invoke_action(self, comp: str, action: str) -> bool:
        """
        Invoke component action
        POST /api/invoke_action
        Body: {comp, action}
        """
        try:
            payload = {
                'comp': comp,
                'action': action
            }
            response = self.session.post(f"{self.base_url}/api/invoke_action", json=payload, timeout=5)
            response.raise_for_status()
            data = response.json()
            print(f"[ESP32 {self.host}] Invoke action {comp}/{action}: {data}")
            return data.get("success", False)
        except Exception as e:
            print(f"[ESP32 {self.host}] Error invoking action: {e}")
            return False


class ESP32Manager:
    """Manages multiple ESP32 devices"""
    
    def __init__(self):
        self.devices: Dict[str, ESP32Device] = {}
        
    def add_device(self, name: str, host: str, port: int = 80) -> ESP32Device:
        """Add a new ESP32 device (HTTP server on port 80 by default)"""
        device = ESP32Device(host, port)
        self.devices[name] = device
        return device
    
    def get_device(self, name: str) -> Optional[ESP32Device]:
        """Get device by name"""
        return self.devices.get(name)
    
    def get_all_devices(self) -> Dict[str, ESP32Device]:
        """Get all registered devices"""
        return self.devices


# Global instance
esp32_manager = ESP32Manager()
