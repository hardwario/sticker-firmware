# STICKER Calibration Mode

## Overview

Calibration mode is integrated into the standard firmware - no separate calibration build required.
This allows easy field recalibration without firmware changes.

## Activation

### Method 1: Config Setting (Primary)

```
config calibration true
```

Persistent NFC/shell configuration. Device enters calibration mode on boot.
Existing implementation in `app/src/app_calibration.c`.

### Method 2: Hall Sensor Activation (Field)

**Condition:** Within `CALIBRATION_ACTIVATION_WINDOW_S` after battery insertion, activate both magnetic contacts (Hall sensors).

**Hall activation rules:**
- Both contacts must be activated within `CALIBRATION_HALL_TOLERANCE_S` of each other
- Each contact must be held active for at least `CALIBRATION_HALL_HOLD_S`
- Does not need to be simultaneous - sequential activation within tolerance window is OK

```
Battery inserted
     │
     ▼
┌─────────────────────────────┐
│  CALIBRATION_ACTIVATION_    │
│  WINDOW_S for activation    │
└─────────────────────────────┘
     │
     ▼
Hall LEFT active ≥HALL_HOLD_S ──┬── within HALL_TOLERANCE_S ──┬── Hall RIGHT active
                                │                             │
                                └─────────────────────────────┘
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
- **LoRaWAN Interval:** `CALIBRATION_SEND_INTERVAL_S`
- **RTT Interval:** `CALIBRATION_RTT_INTERVAL_S`
- **Datarate:** `CALIBRATION_DATARATE` (fixed, ADR disabled)
- **Payload:** Standard sensor data

### Build Variants

| Build | RTT Output | LoRaWAN | Use Case |
|-------|------------|---------|----------|
| **Debug** | ✅ Active | ✅ Active | Lab calibration with J-Link |
| **Release** | ❌ Disabled | ✅ Active | Field calibration via network |

### RTT Debug Output (Debug build only)

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
- **Maximum:** `CALIBRATION_MAX_DURATION_S`
- **Auto-exit:** After max duration, returns to normal operation

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
                    ┌─────────────────────────┐
                    │  STARTUP WINDOW         │◄──── ACTIVATION_WINDOW_S
                    │  (Normal + Watch)       │
                    └────────┬────────────────┘
                             │
            ┌────────────────┴────────────────┐
            │                                 │
            ▼                                 ▼
   Both Hall activated?              Window elapsed?
            │                                 │
            ▼                                 ▼
   ┌─────────────────────┐          ┌────────────────┐
   │ CALIBRATION MODE    │          │ NORMAL MODE    │
   │ - SEND_INTERVAL_S   │          │ - Config       │
   │ - DATARATE          │          │   interval     │
   │ - MAX_DURATION_S    │          │                │
   └────────┬────────────┘          └────────────────┘
            │
            │ MAX_DURATION_S elapsed
            ▼
   ┌────────────────┐
   │ NORMAL MODE    │
   └────────────────┘
```

## Implementation Notes

### Existing Implementation

`app/src/app_calibration.c` already implements:
- Config-based activation (`g_app_config.calibration`)
- RTT output in format for `readout.py`
- 1-Wire device initialization (DS18B20, Machine Probe)
- Infinite loop with 1s sensor reading interval
- Yellow LED blink indication

### Required Extensions

**Extend `app_calibration.c`:**
- Add LoRaWAN ABP mode with fixed credentials
- LoRaWAN send at `CALIBRATION_SEND_INTERVAL_S`, RTT at `CALIBRATION_RTT_INTERVAL_S`
- Add `CALIBRATION_MAX_DURATION_S` timeout (for Hall-activated mode)
- Add Hall sensor activation detection

**Integration with existing modules:**
- `app_hall.c` - Detect both contacts activation (`HALL_TOLERANCE_S`, `HALL_HOLD_S`)
- `app_lrw.c` - Switch to ABP mode with fixed credentials, override send interval
- `app_led.c` - Different blink pattern for calibration indication

### Configuration Parameters
```c
/* Timing - Activation */
#define CALIBRATION_ACTIVATION_WINDOW_S  (30 * 60)     /* 30 minutes from boot */
#define CALIBRATION_HALL_TOLERANCE_S     5             /* 5 seconds between both halls */
#define CALIBRATION_HALL_HOLD_S          2             /* 2 seconds hold required */

/* Timing - Operation */
#define CALIBRATION_SEND_INTERVAL_S      30            /* LoRaWAN send interval */
#define CALIBRATION_RTT_INTERVAL_S       1             /* RTT output interval */
#define CALIBRATION_MAX_DURATION_S       (2 * 60 * 60) /* 2 hours max */

/* LoRaWAN */
#define CALIBRATION_DATARATE             LORAWAN_DR_5
#define CALIBRATION_ADR_ENABLED          false
```

### Persistent State
- Calibration mode state should NOT persist across reboot
- Each power cycle starts fresh with `CALIBRATION_ACTIVATION_WINDOW_S` window

## LoRaWAN Payload Decoder

Create `app/calibration/tts_calibr.js` - decoder compatible with:
- The Things Stack (TTS/TTN) v3
- ChirpStack v3
- ChirpStack v4

### Decoder Structure

```javascript
/* Common decode function */
function decodePayload(bytes) {
  // ... parses standard STICKER payload
  // Returns object with calibration-relevant fields:
  // - temperature (SHT40)
  // - humidity (SHT40)
  // - ext_temperature_1 (T1 - DS18B20)
  // - ext_temperature_2 (T2 - DS18B20)
  // - machine_probe_temperature_1 (MP1-T)
  // - machine_probe_humidity_1 (MP1-H)
}

/* TTS/TTN v3 entry point */
function decodeUplink(input) {
  return { data: decodePayload(input.bytes), warnings: [], errors: [] };
}

/* ChirpStack v3 entry point */
function Decode(fPort, bytes, variables) {
  return decodePayload(bytes);
}

/* ChirpStack v4 uses decodeUplink (same as TTS) */
```

### Calibration-Relevant Fields

| Field | JSON Key | Source |
|-------|----------|--------|
| SHT40 Temperature | `temperature` | On-board sensor |
| SHT40 Humidity | `humidity` | On-board sensor |
| T1 Temperature | `ext_temperature_1` | DS18B20 #1 |
| T2 Temperature | `ext_temperature_2` | DS18B20 #2 |
| MP1 Temperature | `machine_probe_temperature_1` | Machine Probe SHT |
| MP1 Humidity | `machine_probe_humidity_1` | Machine Probe SHT |

## Open Questions

1. ~~**Hall activation timing:** Must both contacts be activated simultaneously, or sequentially within a time window?~~ ✅ 5s tolerance, 2s hold
2. **Exit condition:** Can calibration mode be exited early (e.g., by activating both contacts again)?
3. **LED pattern details:** Exact timing and color for calibration indication?
4. ~~**ADR behavior:** Disable ADR during calibration to maintain fast datarate?~~ ✅ ADR disabled, fixed DR5
5. **ABP credentials:** Define actual calibration DevAddr, NwkSKey, AppSKey values
