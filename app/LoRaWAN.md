# LoRaWAN Configuration Notes

## RX Window Timing

### Problem
Device was unable to receive downlinks (ACK, LinkCheckAns) after successful JOIN. The RX1/RX2 windows were timing out with status `0xa6` (RX timeout).

### Root Cause
1. **ChirpStack RX1 Delay** was set to system default (0), which caused timing mismatch
2. **SYSTEM_MAX_RX_ERROR** default value (20ms) was too small for reliable RX window synchronization

### Solution

#### ChirpStack Configuration
- **Device Profile → Class A → RX1 delay**: Set to `1000` ms (explicit, not system default)
- After changing this setting, device must do fresh JOIN (`tester lrw leave` + `tester lrw join`) to receive new timing parameters

#### Firmware Configuration (prj.conf)
```
CONFIG_LORAWAN_SYSTEM_MAX_RX_ERROR=100
```

| Value | Description |
|-------|-------------|
| 20ms  | Default - may be too tight |
| 50ms  | Aggressive - test after fresh join |
| 100ms | Recommended - good balance |
| 500ms | Conservative - for debugging |

### Timing Analysis

#### JOIN vs Regular Uplink
| Operation | RX1 Delay | RX2 Delay |
|-----------|-----------|-----------|
| JOIN      | 5000ms    | 6000ms    |
| Regular   | 1000ms    | 2000ms    |

JOIN has 5x longer delay, making it more tolerant to timing errors.

#### RX Window Calculation
```
RX1_open = TX_end + RX1_DELAY - SYSTEM_MAX_RX_ERROR
RX1_close = RX1_open + symbol_timeout
```

With `SYSTEM_MAX_RX_ERROR=100`:
- RX1 opens ~100ms before expected downlink
- Symbol timeout extends window to catch late arrivals

### Debug Commands

```shell
tester lrw join      # OTAA join
tester lrw leave     # Leave network (clears session)
tester lrw check     # Send LinkCheckReq + confirmed uplink
tester lrw status    # Show link check result (margin, gateways)
tester lrw keys      # Show current session keys
```

### Debug Configuration (debug.conf)
```
CONFIG_LORAWAN_LOG_LEVEL_DBG=y
CONFIG_LORA_LOG_LEVEL_DBG=y
```

### Important Notes

1. **NVM Persistence**: RX timing parameters are stored in NVM after JOIN. If ChirpStack settings change, device must rejoin to get new parameters.

2. **Power Consumption**: Lower `SYSTEM_MAX_RX_ERROR` = shorter RX window = lower power consumption. Find the minimum value that works reliably.

3. **Status Codes**:
   - `0xa4` - RX done (data received)
   - `0xa6` - RX timeout (no preamble detected)
   - `0xac` - TX done

### Tested Working Configuration

- ChirpStack RX1 delay: 1000ms
- `CONFIG_LORAWAN_SYSTEM_MAX_RX_ERROR=100`
- Link check result: margin=17 dB, gateways=1, RSSI=-59 dB, SNR=12 dB
