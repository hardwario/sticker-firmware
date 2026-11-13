# Compact telemetry (11 B tier, fPort 11)

At the 11 B LoRaWAN payload tier, protobuf telemetry with paging carries
almost nothing. A 1-Wire reading never fits, and two NA sentinels already
fill a frame. The 11 B tier is US915 DR0, and AU915 / AS923 DR2 with the
400 ms uplink dwell time.

Below a budget of `APP_COMPOSE_COMPACT_BUDGET` (20 B), LoRaWAN telemetry is
sent instead as a series of **self-contained, fixed-layout, bit-packed frames
on fPort 11**:

- Every report sends all of its frames: the base frame, plus one frame per
  present group.
- The format is chosen at the start of a report and stays the same for the
  whole report.
- P2P (Hub) keeps protobuf at any budget.

Not sent at this tier:

- Alarms (fPort 3) and the alarm status bits.
- `get_config`, `get_settings` and `get_info` answer with
  `BUDGET_TOO_SMALL`.
- Altitude. The decoder can derive it from pressure if needed.

## Frame

The frame is a bitstream with fields written LSB-first, at most 11 B long.

The header is 1 B:

| bits | field |
|---|---|
| 0-1 | version (0) |
| 2-5 | frame type |
| 6 | boot (first report after boot) |
| 7 | reserved |

A field without a valid sample holds its **NA code**:

- unsigned fields: all bits set;
- signed fields: the minimum value.

The decoder (`ttn.js`) maps NA to `null`. A group whose capability is off is
absent from the decoded object. The decoded keys are the same as on fPort 2.

### Encodings

| quantity | code | bits |
|---|---|---|
| battery | (V − 1.8) / 0.02 | 7 |
| temperature | (t + 55) × 10 (0.1 °C) | 11 |
| humidity | %RH × 2 | 8 |
| pressure | (hPa − 300) × 10 | 13 |
| illuminance | log: exponent and mantissa (see below) | 8 |
| orientation | 0..6 | 3 |
| event counters (accel, PIR) | modulo 4096 | 12 |
| pulse counters | modulo 65536 | 16 |
| input voltage | 10 mV | 12 |
| magnetic field | signed, 0.1 mT | 12 |
| acceleration | signed, 0.2 m/s² | 11 |
| tilt | 0 / 1, NA = 3 | 2 |

Illuminance: take v = lux / 2 + 1. The code is 4 bits of exponent
(floor(log2 v)) followed by 4 mantissa bits. The step is at most 6.25 %,
over a range of 0 to about 131 klx.

Counters are sent modulo 2^n, not as a delta. The host unwraps the overflow,
so a lost frame loses no pulses.

### Frame types

| type | name | contents after the header | bits |
|---|---|---|---|
| 0 | base (always) | battery 7, mask 5 (internal, barometer, light, accel, PIR), temperature 11, humidity 8, pressure 13, illuminance 8, orientation 3, accel count 12, PIR count 12 | 79 |
| 1 | counters | mask 4 (hall L, hall R, input A, input B), active 4, 4 × count 16 | 72 |
| 2 | analog | mask 2 (A, B), U_A 12, U_B 12 | 26 |
| 3 | dallas | slot mask 4, 4 × temperature 11 | 48 |
| 4 | machine probe A | slot − 1 (2 bits), temperature 11, humidity 8, temperature-aux 11, illuminance 8, tilt 2 | 42 |
| 5 | machine probe B | slot − 1 (2 bits), magnetic field 12, accel x / y / z 3 × 11 | 47 |

Types 1 to 3 are sent only when present in the report. Types 4 and 5 are
sent for each machine-probe slot. In the decoded object, frames 3 to 5
carry `w1_sensors` with the same layout as on fPort 2. A machine-probe
reading is split over its A and B frames.

## Airtime

One 11 B frame at SF10 / 125 kHz takes about 370 ms on air.

The TTS fair-use policy allows 30 s of uplink airtime per day, which is about
80 frames. A report of N frames therefore needs an interval of at least about
N × 18 min. For example, base + dallas every 40 min stays within the limit.
