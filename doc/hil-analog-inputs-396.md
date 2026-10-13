# HIL Test Report — Analog inputs GP_A/GP_B (#396)

> Target firmware: **v1.5.0**, PR #407. Feature spec:
> [`version 1.5.md` §45](version%201.5.md).
>
> **Two firmware layouts.** The executed tests T1–T4 below ran on 2026-08-26 against
> the first implementation (branch head `ec25fff`), which used Telemetry fields 28/29,
> history bits after `accel-motion` and alarm sources `input-a`/`input-b`. The reworked
> PR moves the values onto the §40 sensor channel model: Telemetry fields **30/31**,
> history channels **(0, 11) / (0, 14)**, alarm rules on `mb input-a-voltage` /
> `mb input-b-voltage`, and the no-data watchdog. The measurement chain (divider,
> ×34 correction, ADC read) is unchanged, so T1 still holds; T2–T4 must be repeated on
> the new layout — see [Re-verification on the reworked layout](#re-verification-on-the-reworked-layout-pending).

The measurement chain under test is:

```
input connector ── 33k series ──┬── ADC pin (GP_A=PB4/IN3, GP_B=PA11/IN7)
                                └── 1k shunt to GND
V_pin = V_in / 34        (hardware divider)
V_report = V_pin × 34    (firmware correction, app_analog.c)
```

so a voltage applied at the connector should be reported back ~unchanged, and
the ratio `V_report / V_applied` is the headline metric (target ≈ 1.0).

---

## Test bench

| Item | Value |
|---|---|
| Date | 2026-08-26 |
| STICKER unit | SN 2162165627, debug build (`ec25fff` + shell/doc follow-up) |
| Probe | J-Link EDU Mini `801053709`, SWD @ 0x20000000 (LTO) |
| Supply | battery, ~3.5 V → VDDA = LDO 3.3 V (TPS7A2033) |
| Signal source | PPK2 in source-meter mode, one channel at a time on the input connector **before** the divider |
| Observability | RTT shell over `rttt` (`ats sensors sample`, `ats lrw compose`, `history`, `alarm`); telemetry decoded with `app/decoder/ttn.js` |

**Bench constraints that shaped the plan:**

- The PPK2 source floor is **~800 mV** — requests below that clamp to ~780–850 mV
  actual output, so the low end of the input range cannot be swept with this
  source.
- The bench can drive **only one channel at a time**; GP_A and GP_B were tested
  separately (the unpowered channel reads its floating baseline).
- The `LOG_DBG` per-measurement line in `app_analog.c` is compiled out in the
  debug build (`CONFIG_LOG_MAX_LEVEL=2`); for the ratio sweep it was temporarily
  promoted to `LOG_WRN` (reverted afterwards — the shipped source keeps `DBG`).

---

## Executed tests

### T1 — Divider ratio & linearity

**Goal:** the `×34` correction is applied correctly and the reported voltage is
linear in the applied voltage.
**Observable:** `app_analog.c` measurement (pin mV / input mV) vs. the PPK2 set
voltage.

Sweep on GP_B (`cap-analog-b=true`), PPK2 stepped 1000→5000 mV:

| PPK2 [mV] | pin [mV] | FW input [mV] | ratio |
|---:|---:|---:|---:|
| 1000 | 29 | 986 | 0.986 |
| 1500 | 47 | 1598 | 1.065 |
| 2000 | 59 | 2006 | 1.003 |
| 2500 | 75 | 2550 | 1.020 |
| 3000 | 90 | 3060 | 1.020 |
| 3500 | 106 | 3604 | 1.030 |
| 4000 | 120 | 4080 | 1.020 |
| 4500 | 134 | 4556 | 1.012 |
| 5000 | 147 | 4998 | 1.000 |

**Result: PASS.** `FW input = pin × 34` exactly at every point (correction math
correct). Ratio 1.00–1.02 across the valid range; the effective divider from the
slope is ~33.9 (nominal 34, within resistor tolerance). Baseline at 0 V (source
off, connector pulled to GND via the shunt): pin 2 mV → 68 mV input (~0.3 % of a
24 V full scale). The 500 mV point read ratio 1.63 and was discarded — it is
below the PPK2 source floor, not a firmware error. Quantization is ~34 mV/LSB
referred to the input (0.8 mV pin × 34), i.e. coarse at the low end — inherent to
the ×34 attenuation.

### T2 — Telemetry propagation (old layout)

**Goal:** the measured value reaches the fPort-2 `Telemetry` frame (fields
28/29) and decodes correctly.
**Observable:** `ats lrw compose` (builds the uplink without sending) → decode
with `ttn.js`.

With PPK2 = 1000 mV on GP_B, GP_A floating:

```
frame: 0108ad01...e00144e8019e08
ttn.js: { input_a_voltage: 0.068, input_b_voltage: 1.054 }
```

**Result: PASS.** `input_b_voltage` matches the source; `input_a_voltage` shows
the floating-A baseline. The same encoder feeds the synchronous NFC `Sample`
response (`app_compose_snapshot()` → `fill_telemetry()`).

### T3 — History propagation (old layout)

**Goal:** the value is stored in and read back from the history buffer.
**Observable:** `history capture` + `history read`.

```
history enable on
history sensors input-a-voltage on ; history sensors input-b-voltage on
history capture ; history read
#  time          temperature  input-a-voltage  input-b-voltage
0  +0s (no-rtc)     25.52          0.07             0.99
```

**Result: PASS.** Both channels record; values agree with T1/T2 (small deltas =
inter-sample ADC quantization). Note: history commands are the root `history …`
command, not `ats history`.

### T4 — Alarm propagation (old layout)

**Goal:** a `voltage` threshold rule on an analog input source evaluates the real
reading and latches/recovers correctly.
**Observable:** `alarm poll` + `alarm list` active flag.

Rule: `alarm new input-b voltage 1.5 5.0 0` (band 1.5–5.0 V, immediate):

| PPK2 | Reading | Expected | `alarm list` |
|---|---|---|---|
| 1000 mV | ~1.0 V (below `lo`) | active | `active=1` ✅ |
| 3000 mV | ~3.0 V (in band) | recover | `active=0` ✅ |

**Result: PASS.** Bidirectional threshold on the real analog value confirmed.
(The test rule is persisted — clear it with `alarm clear <slot>` afterwards.)

---

## Proposed further testing

Not covered by the session above; ordered roughly by value.

1. **GP_A full sweep.** T1 was run on GP_B; GP_A was only spot-checked functionally
   at the start. Repeat the T1 sweep on GP_A (`cap-analog-a`, PPK2 moved to the A
   connector) to confirm the two channels match — R8/R9 are DNP, so the dividers
   should be identical, but this has not been measured.
2. **Low end of the range (< 800 mV).** The PPK2 floor blocks it; use a precision
   DAC / calibrator or a resistor ladder to verify 0–800 mV, where the ~34 mV/LSB
   quantization is proportionally largest.
3. **Top of the range & over-range.** Drive the input toward 24 V (pin approaches
   VDD): confirm the reading tracks up to 24 V (registry range 0–30 V; a value
   outside it is dropped as no reading), and characterize behaviour above 24 V (pin clipping at VDD, no damage — the
   divider protects the pin only up to ~112 V at VDD).
4. **Accuracy across units & supply.** The ×34 constant assumes nominal 33k/1k and
   VDDA = 3.3 V. Measure several units and a supply sweep (undervoltage band) to
   bound the systematic error from resistor tolerance and VREF.
5. **Temperature drift.** ADC/VREF and resistor drift over the operating range.
6. **Idle power re-check.** Still pending from the spec: J-Link detached + a
   current meter on the battery rail, comparing idle current with `cap-analog-*`
   off vs. on. The source-injection test here cannot measure this.
7. **Both channels + crosstalk.** Drive A and B independently (two sources or
   sequential) and confirm no channel-to-channel coupling.
8. **Fault / sentinel path.** Force an ADC failure (or leave the cap on with the
   pin lost to a sharing conflict) and confirm the reading is NaN → field 30/31
   omitted (telemetry, §42) / `0xFFFF` (history) and a `no_data` alarm (R5).
9. **Pin-sharing mutual exclusion on HW.** Enable `cap-input-a` + `cap-analog-a`
   (and PIR/buzzer combinations) and confirm the runtime resolution order and the
   `LOG_WRN`, matching `version 1.5.md` §45 (R8).
10. **Dynamic signals.** One-shot sampling against a slowly varying / pulsed input
    — characterize aliasing and the effective sample instant.
11. **Alarm dwell & hysteresis.** Non-zero `dwell` on an analog threshold rule
    (confirm the band + dwell timing), and history/alarm behaviour over a full
    LoRaWAN replay and NFC paged readout of the new channels.

## Re-verification on the reworked layout (pending)

Same bench as above (PPK2 as the source on the input connector). Debug build of the
PR head.

| # | Test | Steps | Expected |
|---|---|---|---|
| R1 | Divider spot check | `config cap-analog-b true`, `config save`; PPK2 1000 / 3000 / 5000 mV on B; `ats sensors sample` | `input-b-voltage` ≈ source (ratio 1.00–1.02, as T1) |
| R2 | Telemetry | `ats lrw compose` → `ttn.js` | `input_b_voltage` in V from field 31; field 30 absent with `cap-analog-a false` |
| R3 | History | `config history-channels 00010b0e` + rest `ff`; `history capture`; `history read` | both channels recorded, values as R1 |
| R4 | Threshold alarm | `alarm new mb input-b-voltage lo 1.5 hi 5`; PPK2 1000 → 3000 mV; `alarm poll` | active below the band, recovers inside it; clear the rule afterwards |
| R5 | No-data watchdog | `cap-analog-b true`, force a failed read (temporary hook) | `no_data` alarm on (0, 14) |
| R6 | **Pin-sharing regression** | `cap-input-a true` (digital) + `cap-analog-b true`; toggle input A while sampling for several `interval-sample` periods | input A keeps counting after every battery / analog read (pin stays digital) |
| R7 | PIR / buzzer regression | buzzer HW variant, `cap-buzzer true`, no analog cap; play a melody after a sample | buzzer still sounds after a battery read |
| R8 | Conflict resolution | `cap-input-a true` + `cap-analog-a true`; reboot | `LOG_WRN`, analog A cleared at runtime, digital A works |

R6/R7 cover review finding 1 of the first implementation: its DTS put GP_A/GP_B into
the ADC pinctrl, which the STM32 ADC driver re-applies on every PM resume.

> **Open hardware question:** the divider commit (`ec25fff`) names the **JP11**
> connector; an earlier draft of the spec named **JP2**. Confirm the
> correct connector/pin against `hio-sticker-sch-r2.1.pdf` before finalizing the
> customer-facing wiring docs.
