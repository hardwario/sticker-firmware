# HARDWARIO STICKER — Power Consumption

Idle / sleep current analysis of the STICKER firmware on the STM32WLE5CC, measured
with a Nordic PPK2 source/ampere meter at 3.3 V. This documents where the energy goes,
what the firmware already optimises, and what is left to hardware / upstream.

> Investigation record: issues #90 (idle current) and #160 (power campaign).
> Accelerometer fix shipped in #168. Follow-up: #175.

---

## 1. Summary

In the steady idle state (release firmware, LoRaWAN radio-silent, J-Link **physically
detached**), the board reaches a true deep-Stop2 floor of **~3.7 µA**, but spends most
of its time in a **~55–58 µA mid-band** state plus the always-on hall sensors. Typical
measured idle:

| Configuration | avg | floor median | min (deep Stop2) |
|---|---:|---:|---:|
| no hall, radio off | ~74 µA | ~37 µA | 3.7 µA |
| 2× hall fitted, radio off | ~119 µA | ~65 µA | 3.7 µA |

`avg` includes the periodic wake bursts; `floor median` is the DC sleep current; `min`
is the deep-Stop2 floor reached between wakes.

> A physically attached J-Link adds **~60–100 µA** (debug power domain held over SWD,
> independent of firmware — USB de-authorize does **not** release it). All floor figures
> require the SWD probe to be physically disconnected.

---

## 2. Idle current decomposition

| Component | Contribution | Firmware-addressable? |
|---|---|---|
| Deep Stop2 floor (SoC minimum reached) | ~3.7 µA | — |
| Board / SoC / Zephyr-STM32WL mid-band residual | ~55–58 µA | **No** (upstream / HW) |
| 2× A1266 hall switches | ~20–28 µA | **No** (no enable pin) |
| Accelerometer (LIS2DH continuous ODR) | +30 µA | **Yes — fixed (#168)** |
| Periodic wake bursts (sample / report cadence) | ~12–35 µA (avg) | partially (config) |

---

## 3. Per-element measurements

Measured as the delta in the DC floor region (radio excluded, ±2 µA resolution),
toggling one capability at a time:

| Element | Idle contribution | Notes |
|---|---|---|
| Barometer (LPS22 / MPL3115) | ≈ 0 µA | read on-demand |
| Light (OPT3001) | ≈ 0 µA | read on-demand |
| PIR (PYQ1648) | ≈ 0 µA | event-driven |
| Inputs A / B | ≈ 0 µA | polled (see §4) |
| 1-Wire bus + DS18B20 | ≈ 0 µA | DS2484 suspended via SLPZ between transactions; DS18B20 standby negligible |
| **Hall L + R (2× A1266)** | **~20–28 µA** | always-on, **not** firmware-gateable |
| **Accelerometer (LIS2DH)** | **+30 µA → 0** | continuous ODR; **fixed** by runtime-PM suspend (#168) |

### Accelerometer (the one firmware win)

The LIS2DH ran at a fixed ODR continuously, drawing ~30 µA even when motion/free-fall
detection was off. Fixed by deferring init unless `cap-accelerometer` is set and wrapping
each read in `pm_device_runtime_get/put`, so the sensor is powered down between on-demand
reads (and only armed continuously when `accel-motion-sensitivity` is enabled). This is the
same on-demand pattern as Zephyr PR #110325 (TMP112) and the CHESTER `tmp112` shutdown +
one-shot driver. Shipped in **#168**.

### Hall (A1266) — hardware-bound

The two A1266 hall switches are autonomous self-oscillating devices wired straight to VDD
with **no enable / bus pin** — the firmware only reads their GPIO outputs, it cannot power
them down. They draw a combined ~20–28 µA continuously regardless of configuration. A
controlled two-unit A/B (with vs without the hall parts) confirmed the delta. Reducing this
requires a board-level load-switch on the hall VDD rail; it is not a firmware change.

### 1-Wire master (DS2484) — correctly slept

The DS2484 has an SLPZ pin (PB8). `app_w1.c` resumes it (`PM_DEVICE_ACTION_RESUME`) only for
the duration of a 1-Wire transaction and suspends it afterwards, so it sits at its ~0.5 µA
sleep current at idle. With `cap-w1-sensors` off it is not initialised and the 100 k pull-down
holds it asleep.

---

## 4. Wake sources

| Source | Period | Cost | Condition |
|---|---|---|---|
| Hall poll timer | 100 ms | short GPIO read | `cap-hall-*` enabled |
| Input poll timer | 100 ms | short GPIO read | `cap-input-*` enabled |
| Kernel LPTIM (deep-Stop2 rollover) | ~2 s | ~27 mA / 0.2 ms MSI-restart inrush spike | always |
| Sensor sample | `interval_sample` (e.g. 30 s) | ~45 ms / 27 mA burst (sensor read) | `interval_sample != 0` |
| Report cadence | `interval_report` | compose / uplink | LoRaWAN active |
| Clock resync | long | short | RTC synced |

The 100 ms hall + input poll timers are the dominant wake rate (they keep the CPU in the
~57 µA mid-band rather than deep Stop2). The ~27 mA spikes are **not** the radio and **not**
LED — they are the regulator/oscillator (MSI) restart inrush when the SoC exits a deep Stop2,
plus the periodic sensor-sample burst. The fine ~108 ms / 10 Hz mid-band ripple does **not**
pass through `sys_clock_set_timeout`; it is sub-policy hardware behaviour.

---

## 5. Sleep clocking

On entering Stop0/1/2 the STM32WL **stops the main oscillator (MSI 48 MHz) entirely** — the
core clock is gated, not slowed. Only the **LSE 32.768 kHz** keeps running (RTC + LPTIM1
wake timer). On wake the SoC reselects MSI and Zephyr does a full clock-tree re-init
(`stm32_clock_control_init`), which is the ~9 ms wake cost and the inrush spike. There is no
"switch to a slow clock for sleep" — sleep already means the core oscillator is off.

The kernel correctly chooses Stop2 (verified: `LL_PWR` LPMS = Stop2, `PWR_CR1` read back over
a `__noinit` RAM trace with the J-Link detached). The residual ~57 µA is therefore *inside*
a confirmed Stop2, i.e. a board/SoC LDO-Stop2 characteristic rather than a wrong PM policy.
The board has **no SMPS inductor fitted** (VLXSMPS/VFBSMPS tied — LDO bypass mode), so the
SMPS low-power mode cannot be used.

---

## 6. Radio

| Event | Charge | When |
|---|---|---|
| OTAA join | ~22 mC over ~8–9 s | once per boot (if provisioned) |
| Uplink (TX + RX windows) | ~0.46 mC / ~85 ms | per report |
| Boot bring-up burst | ~6 mA mean / ~28 mA peak over ~1 s | every boot |

Setting `lrw-deveui` to all-zero puts LoRaWAN into the **radio-silent DISABLED** state
(#98): no join, no uplinks. Note that `app_lrw_init()` still runs `lorawan_start()` at boot
before entering DISABLED, so a one-time boot radio burst remains — tracked as **#175** (skip
the whole LoRaWAN bring-up when the DevEUI is zero).

---

## 7. Conclusions

- **Idle is optimal for everything under firmware control.** The accelerometer was the only
  continuous firmware-removable draw (−30 µA, #168). All other sensors are already on-demand;
  DS2484 and ST25DV (LPD pin) are correctly put to sleep.
- **The dominant residual is hardware / upstream:** ~55–58 µA STM32WL LDO Stop2 floor (also
  seen on a bare minimal Zephyr app) plus ~20–28 µA from the always-on hall switches.
- **Hardware levers** (not firmware): a load-switch on the hall VDD; fitting/using an SMPS;
  STM32WL/Zephyr PM upstream work on the Stop2 floor.
- **Measurement discipline:** detach the SWD probe physically for any floor measurement; use
  the `floor median` / mid-band for DC comparisons, not `avg` (which carries burst variance).
