# STICKER Calibration Mode

## Overview

Calibration mode is integrated into the standard firmware - no separate calibration build required.
This allows easy field recalibration without firmware changes.

## Activation

**Condition:** Within 30 minutes after battery insertion, activate both magnetic contacts (Hall sensors).

**Hall activation rules:**
- Both contacts must be activated within **5 second tolerance** of each other
- Each contact must be held active for at least **2 seconds**
- Does not need to be simultaneous - sequential activation within 5s window is OK

```
Battery inserted
     │
     ▼
┌─────────────────────┐
│  30-minute window   │
│  for activation     │
└─────────────────────┘
     │
     ▼
Hall LEFT active ≥2s ──┬── within 5s ──┬── Hall RIGHT active ≥2s
                       │               │
                       └───────────────┘
                               │
                               ▼
                      CALIBRATION MODE
```

## Behavior in Calibration Mode

### LoRaWAN Configuration
- **Activation:** ABP (fixed credentials, no join required)
- **ADR:** Disabled (maintain fast datarate)

Fixed calibration credentials (constants in `app_calibration.h`):
```c
#define CALIBRATION_DEVEUI   { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }
#define CALIBRATION_DEVADDR  0x00000001
#define CALIBRATION_NWKSKEY  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }
#define CALIBRATION_APPSKEY  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }
```

### Data Transmission
- **Interval:** Every 30 seconds
- **Datarate:** DR5 (fixed, ADR disabled)
- **Payload:** Standard sensor data

### RTT Debug Output (for calibration readout)

Format parsed by `app/calibration/readout/readout.py`:
```
Serial number: 1234567890 / Temperature: 25.50 C / Humidity: 45.30 % / T1: 24.80 C / T2: 24.90 C / MP1-T: 25.20 C / MP1-H: 44.50 %
```

| Field | Format | Description |
|-------|--------|-------------|
| Serial number | `\d{10}` | 10-digit device serial number (SHT40) |
| Temperature | `-?\d+\.\d+` | SHT40 temperature in °C |
| Humidity | `\d+\.\d+` | SHT40 relative humidity in % |
| T1 | `-?\d+\.\d+` | DS18B20 #1 (1-Wire) temperature in °C |
| T2 | `-?\d+\.\d+` | DS18B20 #2 (1-Wire) temperature in °C |
| MP1-T | `-?\d+\.\d+` | Machine Probe SHT temperature in °C |
| MP1-H | `\d+\.\d+` | Machine Probe SHT humidity in % |

**Sensor sources:**
- **SHT40** - On-board temperature/humidity sensor (serial number used as device ID)
- **T1, T2 (DS18B20)** - External 1-Wire temperature probes
- **MP1 (Machine Probe)** - External SHT sensor on 1-Wire bus via DS28E17 bridge

The readout script connects via J-Link RTT and aggregates readings from multiple devices.

### Duration
- **Maximum:** 2 hours
- **Auto-exit:** After 2 hours, returns to normal operation

### LED Indication
- **Different blink pattern** to distinguish from normal operation
- Suggested: Fast double-blink every 5 seconds (vs. single blink in normal mode)

## State Machine

```
                    ┌──────────────────┐
                    │   POWER ON       │
                    └────────┬─────────┘
                             │
                             ▼
                    ┌──────────────────┐
                    │  STARTUP WINDOW  │◄──── 30 min timer
                    │  (Normal + Watch)│
                    └────────┬─────────┘
                             │
            ┌────────────────┴────────────────┐
            │                                 │
            ▼                                 ▼
   Both Hall activated?              30 min elapsed?
            │                                 │
            ▼                                 ▼
   ┌────────────────┐               ┌────────────────┐
   │ CALIBRATION    │               │ NORMAL MODE    │
   │ MODE           │               │                │
   │ - 30s interval │               │ - Config       │
   │ - Fast DR      │               │   interval     │
   │ - 2h max       │               │                │
   └────────┬───────┘               └────────────────┘
            │
            │ 2 hours elapsed
            ▼
   ┌────────────────┐
   │ NORMAL MODE    │
   └────────────────┘
```

## Implementation Notes

### Required Components

**New files:**
- `app/src/app_calibration.c` - Calibration mode state machine
- `app/src/app_calibration.h` - Header with constants and API

**Integration with existing modules:**
- `app_hall.c` - Detect both contacts activation (5s window, 2s hold)
- `app_lrw.c` - Switch to ABP mode with fixed credentials, override send interval
- `app_led.c` - Different blink pattern for calibration indication
- `CMakeLists.txt` - Add app_calibration.c to build

### Configuration Parameters
```c
/* Timing */
#define CALIBRATION_ACTIVATION_WINDOW_S  (30 * 60)   /* 30 minutes from boot */
#define CALIBRATION_HALL_TOLERANCE_S     5           /* 5 seconds between both halls */
#define CALIBRATION_HALL_HOLD_S          2           /* 2 seconds hold required */
#define CALIBRATION_SEND_INTERVAL_S      30          /* 30 seconds */
#define CALIBRATION_MAX_DURATION_S       (2 * 60 * 60) /* 2 hours max */

/* LoRaWAN */
#define CALIBRATION_DATARATE             LORAWAN_DR_5
#define CALIBRATION_ADR_ENABLED          false
```

### Persistent State
- Calibration mode state should NOT persist across reboot
- Each power cycle starts fresh with 30-minute activation window

## Open Questions

1. ~~**Hall activation timing:** Must both contacts be activated simultaneously, or sequentially within a time window?~~ ✅ 5s tolerance, 2s hold
2. **Exit condition:** Can calibration mode be exited early (e.g., by activating both contacts again)?
3. **LED pattern details:** Exact timing and color for calibration indication?
4. ~~**ADR behavior:** Disable ADR during calibration to maintain fast datarate?~~ ✅ ADR disabled, fixed DR5
5. **ABP credentials:** Define actual calibration DevAddr, NwkSKey, AppSKey values
