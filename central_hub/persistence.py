"""
Simple JSON-based persistence for Central Hub parameters.

Saves all local component parameter values to a JSON file.
Loads them on startup, saves periodically in the background.
"""

import asyncio
import json
import logging
from pathlib import Path
from typing import TYPE_CHECKING, Dict, Any

if TYPE_CHECKING:
    from .central_hub import CentralHub

logger = logging.getLogger('Persistence')

# Default persistence file location
DEFAULT_DB_FILE = Path(__file__).parent / "hub_state.json"

# How often to check for changes and save (seconds)
SAVE_INTERVAL = 60


class PersistenceManager:
    """Manages saving and loading of component parameter state."""
    
    def __init__(self, hub: 'CentralHub', db_file: Path = DEFAULT_DB_FILE):
        self.hub = hub
        self.db_file = db_file
        self._last_state: Dict[str, Any] = {}
        self._save_task: asyncio.Task = None
        self._running = False
    
    def load(self):
        """Load saved state from disk and apply to components."""
        if not self.db_file.exists():
            logger.info(f"No saved state found at {self.db_file}")
            return
        
        try:
            with open(self.db_file, 'r') as f:
                data = json.load(f)
            
            loaded_count = 0
            
            # Apply saved values to local components
            for comp_name, params in data.get('components', {}).items():
                comp = self.hub.local_components.get(comp_name)
                if not comp:
                    logger.debug(f"Component '{comp_name}' not found, skipping")
                    continue
                
                for param_name, values in params.items():
                    param = comp.get_param(param_name)
                    if not param:
                        logger.debug(f"Parameter '{comp_name}.{param_name}' not found, skipping")
                        continue
                    
                    if param.read_only:
                        continue
                    
                    # values is a dict of "row,col" -> value
                    for key, value in values.items():
                        try:
                            row, col = map(int, key.split(','))
                            if row < param.rows and col < param.cols:
                                param.set_value(row, col, value, notify=False)
                                loaded_count += 1
                        except (ValueError, IndexError) as e:
                            logger.debug(f"Error loading {comp_name}.{param_name}[{key}]: {e}")
            
            logger.info(f"📂 Loaded {loaded_count} parameter values from {self.db_file.name}")
            self._last_state = data
            
        except json.JSONDecodeError as e:
            logger.error(f"Invalid JSON in state file: {e}")
        except Exception as e:
            logger.error(f"Failed to load state: {e}")
    
    def _get_current_state(self) -> Dict[str, Any]:
        """Get current state of all local component parameters."""
        state = {'components': {}}
        
        for comp_name, comp in self.hub.local_components.items():
            comp_state = {}
            
            for param_name, param in comp.parameters.items():
                if param.read_only:
                    continue
                
                param_values = {}
                for row in range(param.rows):
                    for col in range(param.cols):
                        value = param.get_value(row, col)
                        # Only save non-default values to keep file small
                        if value is not None and value != '':
                            param_values[f"{row},{col}"] = value
                
                if param_values:
                    comp_state[param_name] = param_values
            
            if comp_state:
                state['components'][comp_name] = comp_state
        
        return state
    
    def save(self, force: bool = False):
        """Save current state to disk if changed."""
        current_state = self._get_current_state()
        
        # Check if state changed (or force save)
        if not force and current_state == self._last_state:
            return False
        
        try:
            # Write to temp file first, then rename (atomic on most systems)
            temp_file = self.db_file.with_suffix('.tmp')
            with open(temp_file, 'w') as f:
                json.dump(current_state, f, indent=2)
            temp_file.replace(self.db_file)
            
            self._last_state = current_state
            logger.info(f"💾 Saved state to {self.db_file.name}")
            return True
            
        except Exception as e:
            logger.error(f"Failed to save state: {e}")
            return False
    
    async def start(self):
        """Start the periodic save task."""
        self._running = True
        self._save_task = asyncio.create_task(self._periodic_save())
        logger.info(f"Persistence manager started (saves every {SAVE_INTERVAL}s)")
    
    async def stop(self):
        """Stop the periodic save task and do final save."""
        self._running = False
        if self._save_task:
            self._save_task.cancel()
            try:
                await self._save_task
            except asyncio.CancelledError:
                pass
        
        # Final save on shutdown
        self.save(force=True)
        logger.info("Persistence manager stopped")
    
    async def _periodic_save(self):
        """Periodically check for changes and save."""
        while self._running:
            try:
                await asyncio.sleep(SAVE_INTERVAL)
                self.save()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                logger.error(f"Error in periodic save: {e}")
