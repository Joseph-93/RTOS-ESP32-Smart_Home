#!/usr/bin/env python3
"""
Animation Helper - Simple CLI for testing RGB LED animations

Quick test tool for uploading animations to ESP32 RGB LED component.
Discovers devices via mDNS or manual IP entry.

Usage:
    python animation_helper.py              # Interactive mode
    python animation_helper.py --discover   # Just list discovered devices
"""

import asyncio
import base64
import struct
import sys
import argparse
import math
from typing import List, Tuple, Optional
from dataclasses import dataclass

try:
    import websockets
    from zeroconf import Zeroconf, ServiceBrowser, ServiceListener
except ImportError:
    print("Missing dependencies. Install with:")
    print("  pip install websockets zeroconf")
    sys.exit(1)


# ============================================================================
# Configuration
# ============================================================================

DEFAULT_LED_COUNT = 30
WS_PORT = 81
CHUNK_SIZE = 768  # Bytes per chunk (must be < 1024, leave room for base64 overhead)


# ============================================================================
# Animation Frame Generation
# ============================================================================

@dataclass
class Frame:
    """Single animation frame."""
    colors: List[Tuple[int, int, int]]  # List of (R, G, B) for each LED
    duration_ms: int


def generate_solid_color(led_count: int, r: int, g: int, b: int, duration_ms: int = 1000) -> List[Frame]:
    """Generate a single-frame solid color animation."""
    colors = [(r, g, b)] * led_count
    return [Frame(colors=colors, duration_ms=duration_ms)]


def generate_off(led_count: int) -> List[Frame]:
    """Generate all-off animation."""
    return generate_solid_color(led_count, 0, 0, 0, 1000)


def generate_rainbow(led_count: int, steps: int = 60, step_duration_ms: int = 50) -> List[Frame]:
    """Generate rainbow cycle animation."""
    frames = []
    for step in range(steps):
        offset = (step * 360) // steps
        colors = []
        for i in range(led_count):
            hue = (offset + (i * 360) // led_count) % 360
            r, g, b = hsv_to_rgb(hue, 255, 255)
            colors.append((r, g, b))
        frames.append(Frame(colors=colors, duration_ms=step_duration_ms))
    return frames


def generate_breathing(led_count: int, r: int, g: int, b: int, 
                       steps: int = 60, cycle_ms: int = 3000) -> List[Frame]:
    """Generate breathing/pulsing animation."""
    frames = []
    step_duration = cycle_ms // steps
    for step in range(steps):
        # Sine wave for smooth breathing
        brightness = (math.sin(step * 2 * math.pi / steps) + 1) / 2
        br = int(r * brightness)
        bg = int(g * brightness)
        bb = int(b * brightness)
        colors = [(br, bg, bb)] * led_count
        frames.append(Frame(colors=colors, duration_ms=step_duration))
    return frames


def generate_chase(led_count: int, r: int, g: int, b: int,
                   tail_length: int = 5, step_duration_ms: int = 50) -> List[Frame]:
    """Generate chase/running light animation."""
    frames = []
    for pos in range(led_count):
        colors = [(0, 0, 0)] * led_count
        for tail in range(tail_length):
            idx = (pos - tail) % led_count
            fade = 1.0 - (tail / tail_length)
            colors[idx] = (int(r * fade), int(g * fade), int(b * fade))
        frames.append(Frame(colors=colors, duration_ms=step_duration_ms))
    return frames


def hsv_to_rgb(h: int, s: int, v: int) -> Tuple[int, int, int]:
    """Convert HSV to RGB. h: 0-359, s: 0-255, v: 0-255."""
    if s == 0:
        return (v, v, v)
    
    region = h // 60
    remainder = (h - (region * 60)) * 255 // 60
    
    p = (v * (255 - s)) >> 8
    q = (v * (255 - ((s * remainder) >> 8))) >> 8
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8
    
    if region == 0:
        return (v, t, p)
    elif region == 1:
        return (q, v, p)
    elif region == 2:
        return (p, v, t)
    elif region == 3:
        return (p, q, v)
    elif region == 4:
        return (t, p, v)
    else:
        return (v, p, q)


# ============================================================================
# Frame Serialization
# ============================================================================

def frames_to_bytes(frames: List[Frame], led_count: int) -> bytes:
    """Convert frames to raw byte data for upload."""
    data = bytearray()
    for frame in frames:
        # RGB data for each LED
        for i in range(led_count):
            if i < len(frame.colors):
                r, g, b = frame.colors[i]
            else:
                r, g, b = 0, 0, 0
            data.extend([r, g, b])
        # Duration as 16-bit little-endian
        data.extend(struct.pack('<H', frame.duration_ms))
    return bytes(data)


# ============================================================================
# mDNS Discovery
# ============================================================================

class ESP32Listener(ServiceListener):
    """Listener for mDNS service discovery."""
    
    def __init__(self):
        self.devices = {}
    
    def add_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        info = zc.get_service_info(type_, name)
        if info:
            addresses = [addr for addr in info.parsed_addresses()]
            if addresses:
                self.devices[name] = {
                    'ip': addresses[0],
                    'port': info.port,
                    'name': info.server
                }
    
    def remove_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        self.devices.pop(name, None)
    
    def update_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        self.add_service(zc, type_, name)


def discover_devices(timeout: float = 3.0) -> dict:
    """Discover ESP32 devices via mDNS."""
    print(f"Searching for ESP32 devices ({timeout}s)...")
    
    zc = Zeroconf()
    listener = ESP32Listener()
    browser = ServiceBrowser(zc, "_ws._tcp.local.", listener)
    
    import time
    time.sleep(timeout)
    
    browser.cancel()
    zc.close()
    
    return listener.devices


# ============================================================================
# WebSocket Upload
# ============================================================================

async def upload_animation(ip: str, port: int, frames: List[Frame], 
                           led_count: int, loop: bool = True) -> bool:
    """Upload animation to ESP32 via WebSocket."""
    uri = f"ws://{ip}:{port}"
    
    try:
        async with websockets.connect(uri, ping_interval=20, ping_timeout=10) as ws:
            print(f"Connected to {uri}")
            
            # Serialize frames
            frame_data = frames_to_bytes(frames, led_count)
            frame_count = len(frames)
            frame_size = (led_count * 3) + 2
            
            print(f"Uploading {frame_count} frames, {len(frame_data)} bytes...")
            
            # 1. Set total frames to start upload
            await send_param(ws, "RgbLed", "anim_total_frames", frame_count)
            await asyncio.sleep(0.1)
            
            # 2. Upload chunks
            chunk_idx = 0
            offset = 0
            while offset < len(frame_data):
                chunk = frame_data[offset:offset + CHUNK_SIZE]
                chunk_b64 = base64.b64encode(chunk).decode('ascii')
                
                await send_param(ws, "RgbLed", "anim_chunk_index", chunk_idx)
                await send_param(ws, "RgbLed", "anim_chunk_data", chunk_b64)
                
                offset += len(chunk)
                chunk_idx += 1
                print(f"  Chunk {chunk_idx}: {len(chunk)} bytes")
                await asyncio.sleep(0.05)  # Small delay between chunks
            
            # 3. Set loop mode
            await send_param(ws, "RgbLed", "loop", loop)
            
            # 4. Commit
            await send_param(ws, "RgbLed", "anim_commit", True)
            await asyncio.sleep(0.1)
            
            # 5. Start playback
            await send_param(ws, "RgbLed", "playing", True)
            
            print("Animation uploaded and playing!")
            return True
            
    except Exception as e:
        print(f"Error: {e}")
        return False


async def stop_playback(ip: str, port: int) -> bool:
    """Stop animation playback (turn LEDs off)."""
    uri = f"ws://{ip}:{port}"
    
    try:
        async with websockets.connect(uri, ping_interval=20, ping_timeout=10) as ws:
            await send_param(ws, "RgbLed", "playing", False)
            print("Playback stopped.")
            return True
    except Exception as e:
        print(f"Error: {e}")
        return False


async def send_param(ws, component: str, param: str, value) -> None:
    """Send a parameter update via WebSocket."""
    if isinstance(value, bool):
        val_str = "true" if value else "false"
    else:
        val_str = str(value)
    
    msg = {
        "type": "set",
        "component": component,
        "param": param,
        "row": 0,
        "col": 0,
        "value": val_str
    }
    await ws.send(__import__('json').dumps(msg))
    
    # Wait for response
    try:
        response = await asyncio.wait_for(ws.recv(), timeout=2.0)
    except asyncio.TimeoutError:
        pass


# ============================================================================
# Interactive CLI
# ============================================================================

def parse_color(color_str: str) -> Tuple[int, int, int]:
    """Parse color from string. Accepts: 'red', 'green', 'blue', 'white', '#RRGGBB', 'R,G,B'."""
    color_str = color_str.lower().strip()
    
    presets = {
        'red': (255, 0, 0),
        'green': (0, 255, 0),
        'blue': (0, 0, 255),
        'white': (255, 255, 255),
        'yellow': (255, 255, 0),
        'cyan': (0, 255, 255),
        'magenta': (255, 0, 255),
        'orange': (255, 128, 0),
        'purple': (128, 0, 255),
        'pink': (255, 128, 128),
        'warm': (255, 180, 100),
        'cool': (200, 220, 255),
    }
    
    if color_str in presets:
        return presets[color_str]
    
    if color_str.startswith('#') and len(color_str) == 7:
        r = int(color_str[1:3], 16)
        g = int(color_str[3:5], 16)
        b = int(color_str[5:7], 16)
        return (r, g, b)
    
    if ',' in color_str:
        parts = color_str.split(',')
        if len(parts) == 3:
            return (int(parts[0]), int(parts[1]), int(parts[2]))
    
    return (255, 255, 255)  # Default white


def interactive_mode():
    """Run interactive CLI."""
    print("\n" + "=" * 60)
    print("  ESP32 RGB LED Animation Helper")
    print("=" * 60)
    
    # Discover devices
    devices = discover_devices(timeout=3.0)
    
    if not devices:
        print("\nNo devices found via mDNS.")
        ip = input("Enter ESP32 IP address (or 'esp32-lamp.local'): ").strip()
        if ip.endswith('.local'):
            # Try to resolve
            import socket
            try:
                ip = socket.gethostbyname(ip)
            except socket.gaierror:
                print(f"Could not resolve {ip}")
                return
        port = WS_PORT
    else:
        print(f"\nFound {len(devices)} device(s):")
        device_list = list(devices.values())
        for i, dev in enumerate(device_list):
            print(f"  [{i+1}] {dev['name']} - {dev['ip']}:{dev['port']}")
        
        if len(device_list) == 1:
            ip = device_list[0]['ip']
            port = device_list[0]['port']
            print(f"\nUsing: {ip}:{port}")
        else:
            choice = input("\nSelect device [1]: ").strip() or "1"
            idx = int(choice) - 1
            ip = device_list[idx]['ip']
            port = device_list[idx]['port']
    
    led_count = DEFAULT_LED_COUNT
    
    while True:
        print("\n" + "-" * 40)
        print("Commands:")
        print("  1. Solid color (on)")
        print("  2. Off")
        print("  3. Rainbow")
        print("  4. Breathing")
        print("  5. Chase")
        print("  6. Stop playback")
        print("  7. Change LED count (current: {})".format(led_count))
        print("  q. Quit")
        print("-" * 40)
        
        choice = input("Select [1-7, q]: ").strip().lower()
        
        if choice == 'q':
            break
        elif choice == '1':
            color = input("Color (red/green/blue/white/#RRGGBB/R,G,B) [white]: ").strip() or "white"
            r, g, b = parse_color(color)
            frames = generate_solid_color(led_count, r, g, b, 60000)  # 1 minute frame
            asyncio.run(upload_animation(ip, port, frames, led_count, loop=True))
        elif choice == '2':
            frames = generate_off(led_count)
            asyncio.run(upload_animation(ip, port, frames, led_count, loop=True))
        elif choice == '3':
            frames = generate_rainbow(led_count, steps=60, step_duration_ms=50)
            asyncio.run(upload_animation(ip, port, frames, led_count, loop=True))
        elif choice == '4':
            color = input("Color [white]: ").strip() or "white"
            r, g, b = parse_color(color)
            frames = generate_breathing(led_count, r, g, b, steps=60, cycle_ms=3000)
            asyncio.run(upload_animation(ip, port, frames, led_count, loop=True))
        elif choice == '5':
            color = input("Color [white]: ").strip() or "white"
            r, g, b = parse_color(color)
            frames = generate_chase(led_count, r, g, b, tail_length=5, step_duration_ms=50)
            asyncio.run(upload_animation(ip, port, frames, led_count, loop=True))
        elif choice == '6':
            asyncio.run(stop_playback(ip, port))
        elif choice == '7':
            new_count = input(f"LED count [{led_count}]: ").strip()
            if new_count:
                led_count = int(new_count)
                print(f"LED count set to {led_count}")
        else:
            print("Invalid choice.")


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description='ESP32 RGB LED Animation Helper')
    parser.add_argument('--discover', action='store_true', help='Just discover and list devices')
    parser.add_argument('--ip', type=str, help='ESP32 IP address')
    parser.add_argument('--port', type=int, default=WS_PORT, help='WebSocket port')
    args = parser.parse_args()
    
    if args.discover:
        devices = discover_devices(timeout=5.0)
        if devices:
            print(f"\nFound {len(devices)} device(s):")
            for name, dev in devices.items():
                print(f"  {dev['name']} - {dev['ip']}:{dev['port']}")
        else:
            print("\nNo devices found.")
        return
    
    interactive_mode()


if __name__ == '__main__':
    main()
