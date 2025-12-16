# STICKER Calibration Mode

## Overview

Calibration mode is integrated into the standard firmware - no separate calibration build required.
This allows easy field recalibration without firmware changes.

## Activation

### Method 1: Config Setting (Primary) ✅ Implemented

```
config calibration true
```

Persistent NFC/shell configuration. Device enters calibration mode on boot.
Runs indefinitely until `config calibration false` and reboot.

### Method 2: Hall Sensor Activation (Field) ⏸️ Temporarily Disabled

**Status:** Code is implemented but commented out for later activation.

**Prerequisites:**
```
config cap-hall-left true
config cap-hall-right true
```
Both Hall sensors must be enabled in config. If not enabled, Hall activation is ignored.

**Condition:** Within `CALIBRATION_ACTIVATION_WINDOW_S` (30 min) after battery insertion, activate both magnetic contacts (Hall sensors).

**Hall activation rules:**
- Both Hall sensors must be enabled (`cap_hall_left` and `cap_hall_right`)
- Hall sensors are detected as **active-low** (magnet present = GPIO low)
- Both contacts must be activated within `CALIBRATION_HALL_TOLERANCE_S` (5s) of each other
- Each contact must be held active for at least `CALIBRATION_HALL_HOLD_S` (2s)
- Does not need to be simultaneous - sequential activation within tolerance window is OK
- Has 2 hour timeout (`CALIBRATION_MAX_DURATION_S`), then returns to normal mode

## Behavior in Calibration Mode

### Sensor Initialization

Uses `app_sensor_init()` - same initialization as normal mode. This ensures:
- SHT40 on-board sensor is always available
- DS18B20 probes work when `cap_1w_thermometer` is enabled
- Machine Probe works when `cap_1w_machine_probe` is enabled (even if `cap_1w_thermometer` is disabled)

### LoRaWAN Configuration

**For config-based activation (Method 1):**
- Uses existing LoRaWAN connection (OTAA or ABP as configured in device config)

**For Hall-activated mode (Method 2, when enabled):**
- Switches to ABP mode with fixed calibration credentials
- ADR disabled, fixed DR5

Fixed calibration credentials (constants in `app_calibration.h`):

| Parameter | Value |
|-----------|-------|
| DevEUI | `4b823d16dd16c11c` |
| JoinEUI | `0000000000000000` |
| DevAddr | `004f2a32` |
| NwkSKey | `83c3087ccae40cac26acd360b9e3aebc` |
| AppSKey | `721bcb16cab47b1d3c3f7459167fb898` |

```c
#define CALIBRATION_DEVADDR  0x004f2a32
#define CALIBRATION_NWKSKEY  { 0x83, 0xc3, 0x08, 0x7c, 0xca, 0xe4, 0x0c, 0xac, \
                               0x26, 0xac, 0xd3, 0x60, 0xb9, 0xe3, 0xae, 0xbc }
#define CALIBRATION_APPSKEY  { 0x72, 0x1b, 0xcb, 0x16, 0xca, 0xb4, 0x7b, 0x1d, \
                               0x3c, 0x3f, 0x74, 0x59, 0x16, 0x7f, 0xb8, 0x98 }
```

### Data Transmission

| Output | Interval | Description |
|--------|----------|-------------|
| RTT | 1 second (`CALIBRATION_RTT_INTERVAL_S`) | Debug output via J-Link |
| LoRaWAN | 30 seconds (`CALIBRATION_SEND_INTERVAL_S`) | Calibration payload |

### LED Indication

- Yellow LED blink (100ms) every second during calibration loop

### Duration

| Mode | Timeout |
|------|---------|
| Config-based (Method 1) | None - runs indefinitely |
| Hall-activated (Method 2) | 2 hours (`CALIBRATION_MAX_DURATION_S`) |

## RTT Communication

### Overview

RTT (Real-Time Transfer) je debug rozhraní od SEGGER pro komunikaci přes J-Link. Umožňuje vysokorychlostní obousměrný přenos dat bez dopadu na běh firmware.

### Technické parametry

| Parametr | Hodnota |
|----------|---------|
| Rozhraní | J-Link SWD |
| MCU | STM32WLE5CC |
| RTT Buffer Address | `0x20000800` |
| RTT Channel | 1 (Terminal) |
| Speed | 4000 kHz |

### Připojení

```
J-Link <--SWD--> STM32WLE5CC
  │
  └── USB --> PC (pylink-square)
```

### Firmware strana

RTT výstup je realizován pomocí `printk()` v Zephyr RTOS:

```c
printk("Sticker SN: %010u / T: %.2f C / H: %.1f %%\n",
       g_app_config.serial_number, temperature, humidity);
```

RTT buffer je automaticky konfigurován Zephyrem v `.bss` sekci.

## RTT Debug Output Format

Each sensor outputs its own line with serial number. Format parsed by `app/calibration/readout/readout.py`:

```
Sticker SN: 1234567890
SHT4x SN: 329967701 / T: 23.25 C / H: 35.0 %
DS18B20-1 SN: 281234567890123456 / T: 24.80 C
DS18B20-2 SN: 281234567890123457 / T: 24.90 C
MP-1 SN: 347864
MP-1-SHT SN: 593954345 / T: 22.47 C / H: 33.8 %
MP-2 SN: 347865
MP-2-SHT SN: 593954346 / T: 22.50 C / H: 34.0 %
```

| Line | Serial Number | Values | Description |
|------|---------------|--------|-------------|
| `Sticker SN` | Device config serial | - | Sticker device identifier (SN only) |
| `SHT4x SN` | SHT40 chip serial | T, H | On-board temperature/humidity sensor |
| `DS18B20-N SN` | DS18B20 ROM ID (64-bit) | T | External 1-Wire temperature probe |
| `MP-N SN` | DS28E17 ROM ID (64-bit) | - | Machine Probe bridge chip (SN only) |
| `MP-N-SHT SN` | SHT3x/4x chip serial | T, H | SHT sensor inside Machine Probe |

**Sensor sources:**
- **SHT40/SHT43** - On-board temperature/humidity sensor
- **DS18B20** - External 1-Wire temperature probes (requires `cap_1w_thermometer`)
- **Machine Probe** - External SHT sensor on 1-Wire bus via DS28E17 bridge (requires `cap_1w_machine_probe`)

## RTT Readout Script

### Location
`app/calibration/readout/readout.py`

### Setup
```bash
cd app/calibration/readout
python3 -m venv venv
source venv/bin/activate
pip install pylink-square
```

**Pozor:** Balíček `pylink-square` nesmí být instalován společně s `pylink` (jiná knihovna). Doporučen lokální venv.

### Usage
```bash
# Aktivace venv
source venv/bin/activate

# Základní použití - výstup na stdout + automaticky vytvoří sticker-{SN}.csv
python readout.py

# Vlastní název výstupního souboru
python readout.py -o calibration.csv

# Vlastní interval výstupu (default: 1 sekunda)
python readout.py -i 5

# Vlastní header delay pro detekci všech senzorů (default: 3 sekundy)
python readout.py -d 5
```

### Parametry

| Parametr | Default | Popis |
|----------|---------|-------|
| `-o`, `--output` | `sticker-{SN}.csv` | Výstupní CSV soubor |
| `-i`, `--interval` | `1` | Interval výstupu v sekundách |
| `-d`, `--header-delay` | `3` | Delay před zápisem headeru (čeká na detekci všech senzorů) |

### Výstup

Skript vypisuje data na **stdout** a zároveň ukládá do **CSV souboru**:

```
# Connected to J-Link: 822005110
# Writing to: sticker-1234567890.csv
# J-Link: 822005110, Sticker: 1234567890
Timestamp;Sticker SN:1234567890;SHT4x SN:329967701 T[C];SHT4x SN:329967701 H[%];MP-1 SN:347864;MP-1-SHT SN:593954345 T[C];MP-1-SHT SN:593954345 H[%]
2025-12-16 08:02:23;23.28;35.2;22.47;33.8
```

### CSV Output Format

Header obsahuje sériová čísla senzorů. Sticker a MP-N mají pouze SN (bez hodnot), ostatní senzory mají T a případně H:

```csv
# J-Link: 822005110, Sticker: 1234567890
Timestamp;Sticker SN:1234567890;SHT4x SN:329967701 T[C];SHT4x SN:329967701 H[%];DS18B20-1 SN:281234567890 T[C];MP-1 SN:347864;MP-1-SHT SN:593954345 T[C];MP-1-SHT SN:593954345 H[%]
2025-12-16 08:02:23;23.28;35.2;24.80;22.47;33.8
2025-12-16 08:02:24;23.25;35.3;24.81;22.49;33.9
```

### Podpora více J-Link

Skript automaticky detekuje všechny připojené J-Link emulátory a čte data ze všech současně. Každý sticker dostane vlastní CSV soubor.

## LoRaWAN Calibration Payload

### Payload Format (14 bytes)

```
[4B serial_number][2B temperature][1B humidity][2B t1][2B t2][2B mp1_t][1B mp1_h]
```

| Offset | Size | Type | Scale | Description |
|--------|------|------|-------|-------------|
| 0 | 4 | uint32 BE | - | Sticker serial number |
| 4 | 2 | int16 BE | /100 | SHT40 temperature (°C) |
| 6 | 1 | uint8 | /2 | SHT40 humidity (%) |
| 7 | 2 | int16 BE | /100 | DS18B20 #1 temperature (°C) |
| 9 | 2 | int16 BE | /100 | DS18B20 #2 temperature (°C) |
| 11 | 2 | int16 BE | /100 | Machine Probe #1 temperature (°C) |
| 13 | 1 | uint8 | /2 | Machine Probe #1 humidity (%) |

**Special values:**
- Temperature `0x7FFF` = sensor not available
- Humidity `0xFF` = sensor not available

### Decoder

Location: `decoder/tts_calibr.js`

Compatible with:
- The Things Stack (TTS/TTN) v3
- ChirpStack v3
- ChirpStack v4

```javascript
function decodeUplink(input) {
  // TTS/TTN v3, ChirpStack v4
}

function Decode(fPort, bytes, variables) {
  // ChirpStack v3
}
```

### Decoded Fields

| Field | JSON Key | Source |
|-------|----------|--------|
| Serial Number | `serial_number` | Device config |
| SHT40 Temperature | `temperature` | On-board sensor |
| SHT40 Humidity | `humidity` | On-board sensor |
| T1 Temperature | `ext_temperature_1` | DS18B20 #1 |
| T2 Temperature | `ext_temperature_2` | DS18B20 #2 |
| MP1 Temperature | `machine_probe_temperature_1` | Machine Probe SHT |
| MP1 Humidity | `machine_probe_humidity_1` | Machine Probe SHT |

## Configuration Parameters

Defined in `app/src/app_calibration.h`:

```c
/* Timing - Activation (Method 2) */
#define CALIBRATION_ACTIVATION_WINDOW_S  (30 * 60)     /* 30 minutes from boot */
#define CALIBRATION_HALL_TOLERANCE_S     5             /* 5 seconds between both halls */
#define CALIBRATION_HALL_HOLD_S          2             /* 2 seconds hold required */

/* Timing - Operation */
#define CALIBRATION_SEND_INTERVAL_S      30            /* LoRaWAN send interval */
#define CALIBRATION_RTT_INTERVAL_S       1             /* RTT output interval */
#define CALIBRATION_MAX_DURATION_S       (2 * 60 * 60) /* 2 hours max (Hall-activated only) */

/* LoRaWAN */
#define CALIBRATION_DATARATE             5             /* DR5 */
#define CALIBRATION_ADR_ENABLED          false
```

## Files

| File | Description |
|------|-------------|
| `app/src/app_calibration.h` | Constants, ABP credentials, API declarations |
| `app/src/app_calibration.c` | Main calibration logic |
| `app/calibration/readout/readout.py` | RTT readout script (CSV output) |
| `decoder/tts_calibr.js` | LoRaWAN payload decoder |

## Implementation Status

| Feature | Status |
|---------|--------|
| Config-based activation | ✅ Implemented |
| RTT output with sensor SNs | ✅ Implemented |
| LoRaWAN calibration payload | ✅ Implemented |
| Readout script (CSV) | ✅ Implemented |
| Payload decoder | ✅ Implemented |
| Hall sensor activation | ⏸️ Commented out |
| ABP mode for Hall activation | ✅ Implemented (unused) |

## Open Questions

1. ~~**Hall activation timing:** Must both contacts be activated simultaneously, or sequentially within a time window?~~ ✅ 5s tolerance, 2s hold
2. **Exit condition:** Can calibration mode be exited early (e.g., by activating both contacts again)?
3. ~~**LED pattern details:** Exact timing and color for calibration indication?~~ ✅ Yellow LED, 100ms blink every 1s
4. ~~**ADR behavior:** Disable ADR during calibration to maintain fast datarate?~~ ✅ ADR disabled, fixed DR5
5. ~~**ABP credentials:** Define actual calibration DevAddr, NwkSKey, AppSKey values~~ ✅ Defined in `app_calibration.h`
