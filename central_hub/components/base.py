"""
Base Component and Parameter classes for the Central Hub.
Mirrors the ESP32 component/parameter system architecture.
"""

import asyncio
import logging
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
from typing import Any, Callable, Dict, List, Optional, Tuple, Union

logger = logging.getLogger('Components')


class ParameterType(Enum):
    INT = 'int'
    FLOAT = 'float'
    BOOL = 'bool'
    STRING = 'str'


@dataclass
class BaseParameter(ABC):
    """Base class for all parameters."""
    name: str
    param_id: int
    rows: int
    cols: int
    read_only: bool = False
    _values: Dict[Tuple[int, int], Any] = field(default_factory=dict, repr=False)
    _on_change_callbacks: List[Callable] = field(default_factory=list, repr=False)
    last_updated: Optional[datetime] = None
    
    @property
    @abstractmethod
    def param_type(self) -> ParameterType:
        pass
    
    def get_value(self, row: int = 0, col: int = 0) -> Any:
        return self._values.get((row, col))
    
    def set_value(self, row: int, col: int, value: Any, notify: bool = True):
        """Set value and optionally trigger callbacks."""
        old_value = self._values.get((row, col))
        self._values[(row, col)] = value
        self.last_updated = datetime.now()
        
        if notify and old_value != value:
            for callback in self._on_change_callbacks:
                try:
                    callback(self, row, col, value, old_value)
                except Exception as e:
                    logger.error(f"Error in onChange callback for {self.name}: {e}")
    
    def on_change(self, callback: Callable):
        """Register a callback for value changes."""
        self._on_change_callbacks.append(callback)
    
    def to_dict(self) -> dict:
        """Serialize parameter metadata."""
        return {
            'name': self.name,
            'param_id': self.param_id,
            'type': self.param_type.value,
            'rows': self.rows,
            'cols': self.cols,
            'readOnly': self.read_only,
        }


@dataclass
class IntParameter(BaseParameter):
    min_val: int = 0
    max_val: int = 100
    default_val: int = 0
    
    @property
    def param_type(self) -> ParameterType:
        return ParameterType.INT
    
    def __post_init__(self):
        # Initialize all cells with default value
        for r in range(self.rows):
            for c in range(self.cols):
                if (r, c) not in self._values:
                    self._values[(r, c)] = self.default_val
    
    def set_value(self, row: int, col: int, value: Any, notify: bool = True):
        # Clamp to min/max
        value = int(value)
        value = max(self.min_val, min(self.max_val, value))
        super().set_value(row, col, value, notify)
    
    def to_dict(self) -> dict:
        d = super().to_dict()
        d['min'] = self.min_val
        d['max'] = self.max_val
        return d


@dataclass
class FloatParameter(BaseParameter):
    min_val: float = 0.0
    max_val: float = 100.0
    default_val: float = 0.0
    
    @property
    def param_type(self) -> ParameterType:
        return ParameterType.FLOAT
    
    def __post_init__(self):
        for r in range(self.rows):
            for c in range(self.cols):
                if (r, c) not in self._values:
                    self._values[(r, c)] = self.default_val
    
    def set_value(self, row: int, col: int, value: Any, notify: bool = True):
        value = float(value)
        value = max(self.min_val, min(self.max_val, value))
        super().set_value(row, col, value, notify)
    
    def to_dict(self) -> dict:
        d = super().to_dict()
        d['min'] = self.min_val
        d['max'] = self.max_val
        return d


@dataclass
class BoolParameter(BaseParameter):
    default_val: bool = False
    
    @property
    def param_type(self) -> ParameterType:
        return ParameterType.BOOL
    
    def __post_init__(self):
        for r in range(self.rows):
            for c in range(self.cols):
                if (r, c) not in self._values:
                    self._values[(r, c)] = self.default_val
    
    def set_value(self, row: int, col: int, value: Any, notify: bool = True):
        if isinstance(value, str):
            value = value.lower() in ('true', '1', 'yes')
        else:
            value = bool(value)
        super().set_value(row, col, value, notify)


@dataclass
class StringParameter(BaseParameter):
    default_val: str = ""
    
    @property
    def param_type(self) -> ParameterType:
        return ParameterType.STRING
    
    def __post_init__(self):
        for r in range(self.rows):
            for c in range(self.cols):
                if (r, c) not in self._values:
                    self._values[(r, c)] = self.default_val
    
    def set_value(self, row: int, col: int, value: Any, notify: bool = True):
        value = str(value) if value is not None else ""
        super().set_value(row, col, value, notify)


class Component(ABC):
    """Base class for all components in the Central Hub."""
    
    _next_param_id = 1  # Class-level ID counter
    
    def __init__(self, name: str):
        self.name = name
        self.parameters: Dict[str, BaseParameter] = {}
        self.params_by_id: Dict[int, BaseParameter] = {}
        self._initialized = False
        self.hub = None  # Reference to CentralHub, set during registration
    
    def _get_next_param_id(self) -> int:
        pid = Component._next_param_id
        Component._next_param_id += 1
        return pid
    
    def add_int_param(self, name: str, rows: int = 1, cols: int = 1,
                      min_val: int = 0, max_val: int = 100, 
                      default_val: int = 0, read_only: bool = False) -> IntParameter:
        param = IntParameter(
            name=name,
            param_id=self._get_next_param_id(),
            rows=rows,
            cols=cols,
            min_val=min_val,
            max_val=max_val,
            default_val=default_val,
            read_only=read_only,
        )
        self.parameters[name] = param
        self.params_by_id[param.param_id] = param
        return param
    
    def add_float_param(self, name: str, rows: int = 1, cols: int = 1,
                        min_val: float = 0.0, max_val: float = 100.0,
                        default_val: float = 0.0, read_only: bool = False) -> FloatParameter:
        param = FloatParameter(
            name=name,
            param_id=self._get_next_param_id(),
            rows=rows,
            cols=cols,
            min_val=min_val,
            max_val=max_val,
            default_val=default_val,
            read_only=read_only,
        )
        self.parameters[name] = param
        self.params_by_id[param.param_id] = param
        return param
    
    def add_bool_param(self, name: str, rows: int = 1, cols: int = 1,
                       default_val: bool = False, read_only: bool = False) -> BoolParameter:
        param = BoolParameter(
            name=name,
            param_id=self._get_next_param_id(),
            rows=rows,
            cols=cols,
            default_val=default_val,
            read_only=read_only,
        )
        self.parameters[name] = param
        self.params_by_id[param.param_id] = param
        return param
    
    def add_string_param(self, name: str, rows: int = 1, cols: int = 1,
                         default_val: str = "", read_only: bool = False) -> StringParameter:
        param = StringParameter(
            name=name,
            param_id=self._get_next_param_id(),
            rows=rows,
            cols=cols,
            default_val=default_val,
            read_only=read_only,
        )
        self.parameters[name] = param
        self.params_by_id[param.param_id] = param
        return param
    
    def get_param(self, name: str) -> Optional[BaseParameter]:
        return self.parameters.get(name)
    
    def get_param_by_id(self, param_id: int) -> Optional[BaseParameter]:
        return self.params_by_id.get(param_id)
    
    def get_param_by_type_and_index(self, param_type: str, idx: int) -> Optional[BaseParameter]:
        """Get parameter by type and index (ESP32 compatibility)."""
        # Map type string to ParameterType
        type_map = {
            'int': ParameterType.INT,
            'float': ParameterType.FLOAT,
            'bool': ParameterType.BOOL,
            'str': ParameterType.STRING,
            'string': ParameterType.STRING,
        }
        target_type = type_map.get(param_type.lower())
        if not target_type:
            return None
        
        # Find the idx-th parameter of this type
        count = 0
        for param in self.parameters.values():
            if param.param_type == target_type:
                if count == idx:
                    return param
                count += 1
        return None
    
    @abstractmethod
    async def initialize(self):
        """Initialize the component. Override in subclasses."""
        pass
    
    async def start(self):
        """Start any background tasks. Override if needed."""
        pass
    
    async def stop(self):
        """Stop any background tasks. Override if needed."""
        pass
    
    def to_dict(self) -> dict:
        """Serialize component metadata."""
        return {
            'name': self.name,
            'parameters': {name: p.to_dict() for name, p in self.parameters.items()}
        }
