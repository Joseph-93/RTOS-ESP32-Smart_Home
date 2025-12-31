# ESP32 FreeRTOS Development Setup

ESP32 development environment with ESP-IDF and FreeRTOS for Windows 11.

---

## 🚀 One-Time Setup

### 1. Install ESP-IDF Extension
✅ Already installed: `espressif.esp-idf-extension`

### 2. Configure ESP-IDF Toolchain
✅ Already done at: `C:\Espressif\frameworks\v5.5\esp-idf`

### 3. Set COM Port
✅ Already configured: **COM5** (in `.vscode/settings.json`)

To change: Edit `.vscode/settings.json` and update `"idf.portWin": "COM5"`

### 4. Initial Project Setup
✅ Already completed: Target set to `esp32`, project built successfully

---

## 🎯 Quick Start

1. Open **ESP-IDF Terminal**: `Ctrl+Shift+P` → **`ESP-IDF: Open ESP-IDF Terminal`**
2. Run:
```powershell
cd firmware\esp32_rtos_smart_home
.\build-flash.ps1
```
3. Watch your ESP32 blink and log! 🎉

---

## 🛠️ Build, Flash, Monitor

### **EASY MODE - Simple Scripts** ✨

Open **ESP-IDF Terminal** (`Ctrl+Shift+P` → `ESP-IDF: Open ESP-IDF Terminal`), then:

```powershell
cd firmware\esp32_rtos_smart_home

# Build + Flash + Monitor (all-in-one)
.\build-flash.ps1

# Or individual steps:
.\build-only.ps1          # Just build
.\flash-monitor.ps1       # Just flash and monitor
```

**Exit monitor:** Press `Ctrl+]`

---

### Using ESP-IDF Commands Directly

In the **ESP-IDF Terminal**:

```powershell
cd firmware\esp32_rtos_smart_home

# Build
idf.py build

# Flash
idf.py -p COM5 flash

# Monitor
idf.py -p COM5 monitor

# Flash + Monitor
idf.py -p COM5 flash monitor
```

---

## 📋 Project Structure

```
RTOS-ESP32/
├── firmware/
│   └── esp32_rtos_smart_home/          # Main ESP-IDF project
│       ├── main/
│       │   ├── main.c             # FreeRTOS app (2 tasks + queue)
│       │   └── CMakeLists.txt
│       ├── CMakeLists.txt
│       └── sdkconfig.defaults
├── .vscode/
│   ├── tasks.json                 # Build/flash/monitor tasks
│   ├── launch.json                # Debug configuration
│   └── settings.json              # ESP-IDF settings (COM port)
└── README.md                      # This file
```

---

## 📱 Application Description

**FreeRTOS Blink + Log Demo**

Two concurrent tasks demonstrating FreeRTOS fundamentals:

- **Task A (BlinkTask)**: Toggles LED on GPIO 2 every 500ms, sends blink count to Task B via queue
- **Task B (HeartbeatTask)**: Logs system uptime every 2 seconds, receives blink count from Task A

**Features:**
- `vTaskDelayUntil()` for precise periodic timing
- Queue-based inter-task communication
- Proper FreeRTOS task creation
- ESP32 logging (ESP_LOGI)

**Modify LED GPIO**: Edit `BLINK_GPIO` in `main/main.c` (default: GPIO 2)

---

## 🔧 Troubleshooting

### COM Port Not Found
```powershell
# List all serial devices
Get-CimInstance -ClassName Win32_SerialPort | Format-Table -AutoSize

# Check Device Manager
devmgmt.msc
# Look under "Ports (COM & LPT)"
```

**Solutions:**
- Install CP210x or CH340 USB-to-serial drivers
- Try different USB cable (data cable, not charge-only)
- Reconnect ESP32, check Device Manager for errors
- Try different USB port

### Permission Denied on Flash
- Close any serial monitor/terminal using the COM port
- Disable antivirus temporarily
- Run VS Code as Administrator (if needed)

### Build Fails
```powershell
# Clean and rebuild
cd firmware\esp32_rtos_smart_home
idf.py fullclean
idf.py build
```

### ESP-IDF Not Found
- Ensure ESP-IDF extension setup completed successfully
- Restart VS Code
- Check `C:\Espressif` directory exists
- Re-run: `ESP-IDF: Configure ESP-IDF Extension`

### Monitor Shows Garbage
- Wrong baud rate: Use `115200` (default)
- Wrong COM port selected
- Press EN/RST button on ESP32 to reboot

---

## 🔍 Verification Commands

```powershell
# Check ESP-IDF installation
idf.py --version

# Verify toolchain
xtensa-esp32-elf-gcc --version

# List Python packages
python -m pip list | Select-String esp

# Check COM ports
mode
```

---

## 📚 Quick Reference

### Common idf.py Commands
```powershell
idf.py build                    # Build project
idf.py -p COM3 flash           # Flash firmware
idf.py -p COM3 monitor         # Serial monitor
idf.py -p COM3 flash monitor   # Flash and monitor
idf.py menuconfig              # Configuration menu
idf.py fullclean               # Complete clean
idf.py size                    # Show binary size
```

### Monitor Controls
- `Ctrl+]` - Exit monitor
- `Ctrl+T` `Ctrl+H` - Show help
- `Ctrl+T` `Ctrl+R` - Reset ESP32
- `Ctrl+T` `Ctrl+A` - Toggle timestamp

### ESP32 Buttons
- **EN** (Reset) - Restart the program
- **BOOT** - Hold during reset to enter flash mode (usually automatic)

---

## 📦 What's Installed

- **VS Code Extension**: ESP-IDF by Espressif Systems
- **ESP-IDF**: Framework with FreeRTOS (~2GB at `C:\Espressif`)
- **Toolchain**: GCC cross-compiler (xtensa-esp32-elf)
- **Python**: ESP-IDF Python environment
- **Tools**: esptool.py, idf.py, OpenOCD, etc.

---

## 🎯 Next Steps

1. Complete ESP-IDF installation (see section 2 above)
2. Identify your COM port (section 3)
3. Build the project: `Ctrl+Shift+P` → `Tasks: Run Task` → `ESP-IDF: Build`
4. Flash to ESP32: Task → `ESP-IDF: Flash and Monitor`
5. Watch the logs - you should see Task A blinking and Task B heartbeats!

---

**Project Ready for Development** ✨  
ESP32 + FreeRTOS + VS Code on Windows 11
