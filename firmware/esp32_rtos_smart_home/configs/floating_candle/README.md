# Floating Candle Build — Hardware Setup

## New Board Setup (one-time)

GPIO12 is used for A4988 MS2 (microstepping). It is also an ESP32 boot strapping
pin that controls flash voltage. To prevent boot failures, burn the VDD_SDIO eFuse
to lock flash voltage at 3.3V:

```powershell
# From ESP-IDF Terminal — run ONCE per new ESP32 board
espefuse.py --port COM<X> set_flash_voltage 3.3V
```

> **This is permanent and irreversible.** Correct for all ESP-WROOM-32 modules.

## Wiring

### Shared across all 4 A4988 driver boards

| ESP32 GPIO | A4988 Pin | Notes |
|------------|-----------|-------|
| GPIO27 | ENABLE | Add **10K pull-up to 3.3V** for boot safety |
| GPIO14 | MS1 | |
| GPIO12 | MS2 | Boot strapping — burn eFuse (see above) |
| GPIO13 | MS3 | |
| 3V3 | VDD | Logic supply |
| GND | GND | |

### SLEEP and RESET (all 4 boards)

Tie SLEEP to RESET on each board, all pulled to 3.3V:

```
3.3V ---+--- MD0_SLEEP --- MD0_RESET
        +--- MD1_SLEEP --- MD1_RESET
        +--- MD2_SLEEP --- MD2_RESET
        +--- MD3_SLEEP --- MD3_RESET
```

No ESP32 GPIOs needed for SLEEP/RESET.

### Per-motor STEP and DIR

| ESP32 GPIO | A4988 Pin | Motor |
|------------|-----------|-------|
| GPIO15 | STEP | Motor 0 |
| GPIO2 | DIR | Motor 0 |
| GPIO4 | STEP | Motor 1 |
| GPIO16 | DIR | Motor 1 |
| GPIO17 | STEP | Motor 2 |
| GPIO5 | DIR | Motor 2 |
| GPIO18 | STEP | Motor 3 |
| GPIO19 | DIR | Motor 3 |

### Limit switches

| ESP32 GPIO | Function | Pull-up |
|------------|----------|---------|
| GPIO26 | M0 MIN (retract) | Internal |
| GPIO25 | M0 MAX (pay-out) | Internal |
| GPIO33 | M1 MIN (retract) | Internal |
| GPIO32 | M1 MAX (pay-out) | Internal |
| GPIO35 | M2 MIN (retract) | **External 10K to 3.3V** |
| GPIO34 | M2 MAX (pay-out) | **External 10K to 3.3V** |
| GPIO39/VN | M3 MIN (retract) | **External 10K to 3.3V** |
| GPIO36/VP | M3 MAX (pay-out) | **External 10K to 3.3V** |

All limit switches are active LOW (switch closes to GND when triggered).

### Boot strapping notes

- **GPIO15** (MD0 STEP): Outputs PWM at boot. Safe because ENABLE pull-up keeps drivers disabled.
- **GPIO2** (MD0 DIR): Must be LOW/floating at boot. A4988 DIR is high-Z, no conflict.
- **GPIO5** (MD2 DIR): Internal pull-up, reads HIGH at boot. Harmless for DIR.
- **GPIO12** (MS2): Burn eFuse to neutralize (see above).

### Spare GPIOs

GPIO 21, 22, 23 are unused and available.
