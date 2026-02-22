from django.db import models
import json


class RgbPreset(models.Model):
    """
    Stores metadata about RGB LED presets.
    The actual frame data lives on the ESP32, but this stores the "recipe"
    so we can edit presets properly (effect type, colors, parameters, etc.)
    """
    
    # Device identification
    device_id = models.CharField(max_length=100, db_index=True)  # e.g., "esp32_living_room"
    component_name = models.CharField(max_length=100, default="RgbLed")
    
    # Preset identification (matches ESP32 preset index/name)
    preset_name = models.CharField(max_length=100)
    
    # Effect recipe
    EFFECT_CHOICES = [
        ('solid', 'Solid Color'),
        ('rainbow', 'Rainbow'),
        ('breathing', 'Breathing'),
        ('chase', 'Chase'),
        ('off', 'Off'),
        ('custom', 'Custom/Manual'),
    ]
    effect_type = models.CharField(max_length=20, choices=EFFECT_CHOICES, default='custom')
    
    # Effect parameters stored as JSON
    # Examples:
    #   solid: {"color": "#ff0000", "duration": 1000}
    #   rainbow: {"steps": 60, "step_ms": 50}
    #   breathing: {"color": "#ff0000", "steps": 60, "cycle_ms": 3000}
    #   chase: {"color": "#ff0000", "tail": 5, "step_ms": 50}
    effect_params = models.JSONField(default=dict, blank=True)
    
    # Playback settings
    loop = models.BooleanField(default=True)
    
    # Metadata
    frame_count = models.IntegerField(default=0)
    created_at = models.DateTimeField(auto_now_add=True)
    updated_at = models.DateTimeField(auto_now=True)
    
    class Meta:
        # Each device+component+name combination should be unique
        unique_together = ['device_id', 'component_name', 'preset_name']
        ordering = ['device_id', 'preset_name']
    
    def __str__(self):
        return f"{self.device_id}/{self.component_name}: {self.preset_name} ({self.effect_type})"
    
    def get_effect_params(self):
        """Return effect params as dict"""
        if isinstance(self.effect_params, str):
            return json.loads(self.effect_params)
        return self.effect_params or {}
    
    def set_effect_params(self, params):
        """Set effect params from dict"""
        self.effect_params = params
