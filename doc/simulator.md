# STICKER simulator

The real STICKER application runs on the host as `native_sim/native/64`. The
peripherals are emulated at bus level (I2C, SPI, GPIO, ADC, flash), so the
application code, the Zephyr drivers and the LoRaMac stack all run unchanged.
Host-side Python tools play the other ends: a LoRaWAN network server and a P2P
hub for the radio, and an NFC phone for the tag.

What it is good for: protocol and configuration work, command/paging/NFC flows,
LoRaWAN and P2P end-to-end runs, alarm and sensor logic, settings persistence
and reboots, all without a STICKER or a gateway. What it is not: timing-exact
radio or power behaviour (see [Limitations](#limitations)); HIL stays the final
check.

## Build

```bash
source ~/.venv/bin/activate
pip install -r scripts/sim/requirements.txt          # once: host tools
cd app
west build -p always -b native_sim/native/64 -d build-sim
```

The board files `app/boards/native_sim_native_64.{overlay,conf}` sit on top
of `prj.conf`: debug-like (log + shell on the console), no PM, no watchdog,
LTO off, the board's 10 kHz kernel tick. The emulators live in `drivers/sim/`.

## Run

```bash
python scripts/sim/sim_air.py --appkey 000102030405060708090a0b0c0d0e0f   # terminal 1
app/build-sim/zephyr/zephyr.exe --flash=sticker.bin                       # terminal 2
python scripts/sim/sim_nfc.py basic                                       # terminal 3
```

`zephyr.exe` reads the shell from stdin and writes the log to stdout. It runs
in real time by default; `--no-rt` runs as fast as possible.

| Option | Meaning |
|---|---|
| `--flash=<file>` | Flash image (settings NVS + history ring). It persists across runs and reboots. Without it every start is a factory-fresh device. |
| `--air=<host:port>` | UDP peer of the radio, default `127.0.0.1:47800` (`sim_air.py`) |
| `--air_off` | Radio without air: transmissions go nowhere, nothing is received |
| `--nfc_port=<port>` | UDP port of the NFC phone link on 127.0.0.1, default 47801 |
| `--nfc_off` | No phone link; the RF field is switched by `sim nfc` only |
| `--stop_at=<s>` | Exit after the given simulated time |
| `--pin_low=<names>` | GP / hall pins held low from power-on, comma-separated (`gp_a,gp_b`); the default level is high |

`sys_reboot()` (for example after `settings save`) re-executes the simulator
with the same arguments. The flash file survives the reboot, but the
emulators' state (sensor values set with `sim`, the ST25DV registers) starts
over.

To provision a fresh device, use the shell, as on the production line:

```
config radio-deveui 5876070000000001
config radio-appkey 000102030405060708090a0b0c0d0e0f
config radio-mode lorawan            # or p2p
config cap-barometer true
settings save                        # reboots
```

Several STICKERs can share the air: give each its own `--flash` and
`--nfc_port`.

## Emulated hardware

| Part | Bus | Emulator | Set from the shell |
|---|---|---|---|
| SHT40 temperature/humidity | I2C 0x44 | `emul_sht4x.c` | `sim sht <°C> <%RH> \| on \| off` |
| OPT3001 light | I2C 0x45 | `emul_opt3001.c` | `sim light <lux> \| on \| off` |
| MPL3115A2 barometer | I2C 0x60 | `emul_mpl3115a2.c` | `sim baro <hPa> [°C] \| on \| off`; altitude is derived from the pressure |
| LIS2DH12 accelerometer | I2C 0x19 | `emul_lis2dh.c` | `sim accel <x> <y> <z> \| flat \| shake \| freefall \| on \| off` (m/s²) |
| DS2484 + up to 4 DS18B20 | I2C 0x18 + 1-Wire | `emul_ds2484.c` | `sim w1 <0..3> <°C> \| <0..3> on\|off \| on\|off` (bridge) |
| ST25DV04K NFC tag | I2C 0x53/0x57 | `emul_st25dv.c` | `sim nfc [on\|off]` (RF field), phone link over UDP |
| SX1262 radio | SPI | `emul_sx126x.c` | `sim radio` (state); the air is `sim_air.py` |
| Battery, analog inputs A/B | ADC | `sim_adc.c` | `sim battery <V>`, `sim analog a\|b <V>` |
| Hall sensors, GP pins | GPIO | `gpio_emul` | `sim pin <name> [0\|1]` (also counts the edges an output drives), `sim pulse <pin> <count> [ms] [bounce]` |
| Flash (history, NVS) | — | flash simulator | `--flash=<file>`; `sim nvs write <key> <hex>`, `sim nvs delete <key>` (raw settings values, e.g. an old firmware's) |

`sim status` prints every value. The defaults are 23 °C / 45 %RH, 250 lux,
1013.25 hPa, a STICKER lying flat (0, 0, 9.81 m/s²), DS18B20 #0 and #1
present at 20 / 21 °C, 3.0 V battery.

Emulator notes:

- **LIS2DH12**: both interrupt generators (high/low per axis, OR/AND,
  threshold in the full-scale LSB) are evaluated on every new sample, which is
  every `sim accel`. With the high-pass filter on a generator's path, it sees
  the change since the last `REFERENCE` read; the app snaps the reference when
  it arms motion detection. `shake` is a 15 m/s² jolt and back (one any-motion
  event). `freefall` is 0 g (the free-fall AND-of-lows on INT1, plus a motion
  event). INT1/INT2 sit on a small input-only port (`sim_gpio.c`), because
  `gpio_emul` runs every pin callback on each configure, and the Zephyr LIS2DH
  driver shares INT1 with the app.
- **DS2484**: the bus is modelled per time slot, so search ROM, match/skip ROM
  and the scratchpad CRCs run bit by bit as on the wire. A conversion finishes
  at once. DS28E17 machine probes are not emulated.
- **SX1262**: commands, buffer, IRQs and BUSY at register level. Every TX goes
  to the air peer as a UDP datagram with frequency, modulation, sync word and
  IQ polarity. The peer answers with a downlink timed in simulated time, or
  with NONE, and the emulator delivers it in the RX window it falls into.
- **ST25DV**: user/system/dynamic registers, the FTM mailbox with
  `RF_PUT`/`HOST_PUT`, GPO pulses on field change and RF put, and VCC from the
  LPD pin (the chip NACKs when unpowered).

## Air peer: `scripts/sim/sim_air.py`

One process serves both radio modes, told apart by the sync word:

- **LoRaWAN** (EU868, 1.0.x): OTAA join for `--appkey` (any DevEUI) or
  `--device DEVEUI:APPKEY`, MIC check, decryption, LinkCheckAns,
  DeviceTimeAns. Uplinks are decoded with the app's TTN decoder
  (`app/decoder/ttn.js`, needs `node`).
- **P2P** (TOWER over LoRa, [p2p.md](p2p.md)), in `sim_p2p.py`:
  JoinRequest/JoinAccept with the derived session key, replay lane, a local
  ACK with PENDING and a CTRL tail (LinkCheckAns, TimeAns, PendingInfo), and
  confirmed downlinks from the queue. `--net-id` and `--p2p-eui-prefix` set
  the hub identity and the DevEUIs it accepts with the default AppKey.

Commands on its stdin:

```
cmd <deveui|*> {"seq":1,"command":"get_info"}   # DownlinkCommand via encodeDownlink()
down <deveui|*> <fport> <hex> [c]                # raw downlink (P2P: fPort 85 -> port 86)
ctrl <deveui|*> 3000                             # P2P control TLVs (DevStatusReq)
list                                             # sessions, counters, queues
```

## NFC phone: `scripts/sim/sim_nfc.py`

It talks to the emulated ST25DV through the FTM mailbox, the way the
Manager-App does ([nfc mailbox plan](plan/v1.5.0_mailbox.md)):

```bash
python scripts/sim/sim_nfc.py basic                                  # plaintext get_basic_info
python scripts/sim/sim_nfc.py cmd '{"get_info": {}}'                  # owner channel, all pages
python scripts/sim/sim_nfc.py --key <hex> cmd '{"set_param": {...}}'
python scripts/sim/sim_nfc.py vnd '{"get_basic_info": {}}'            # vendor channel
python scripts/sim/sim_nfc.py raw 03 <hex>                            # raw channel frame
python scripts/sim/sim_nfc.py field on|off | status | mboff
python scripts/sim/sim_nfc.py                                         # interactive
```

The owner channel uses `--key` (the device's secret key, default
`000102…0f`) and the vendor channel `--vendor-token`. The nonce counter is
read from `get_basic_info` before each sealed request. The protobuf schema is
compiled from `app/src/app_config.proto` at start-up.

## Smoke test and CI

`scripts/sim/smoke_test.py` runs one STICKER against `sim_air.py`: shell
provisioning, LoRaWAN join and decoded telemetry, NFC `get_basic_info`,
1-Wire enrolment, barometer and accelerometer readings with a motion event,
then the P2P join and telemetry. It takes about 45 s.

```bash
python scripts/sim/smoke_test.py --exe app/build-sim/zephyr/zephyr.exe --log-dir /tmp/sim-logs
```

The CI job *Simulator smoke test (native_sim)* in
`.github/workflows/build.yml` builds the simulator and runs this test on
every PR, and uploads the logs when it fails.

## v2.0.0 HIL plan

`scripts/sim/hil_v200.py` runs steps H1–H7 of the v2.0.0 HIL plan
([plan](plan/509%20-%20v2.0.0%20release%20review%20fixes.md)) on the simulator:
pulse counter at both idle levels and with bounce, RATE alarms on live
counters, stillness rules with the capability off and on, the 11 B tier
(decoded with `ttn.js`), settings save over NFC and the radio, the v1.4
stale-key upgrade, buzzer and PIR modes. About 20 minutes; name steps to run a
subset.

```bash
west build -p always -b native_sim/native/64 -d build-sim-hil -- \
  -DCONFIG_LOG_MODE_IMMEDIATE=y -DCONFIG_LOG_BACKEND_UART=y
python scripts/sim/hil_v200.py --exe app/build-sim-hil/zephyr/zephyr.exe --log-dir /tmp/sim-hil h1 h6
```

The two log options make the boot-time log of the settings load visible (the
shell log backend starts after it); H6 needs them. Stop2, on-air US915 and the
release soak stay on hardware.

## Limitations

- **Timing**: the kernel tick matches the board (10 kHz), but CPU time is
  free. Duty-cycle and airtime are computed by LoRaMac as on the device, while
  the air has no collisions, no path loss and a fixed RSSI/SNR (`--rssi`,
  `--snr`).
- **Power**: no PM (`CONFIG_PM=n`), no current model, no Stop2. Power work
  needs the PPK2 on HW.
- **Not emulated**: the FSK P2P profile (stock TOWER gateway, OTA pairing),
  DS28E17 machine probes, the PIR sensor (its data line is the `gp_a` pin: high =
  motion, held high it reports motion on every 100 ms poll), the buzzer (it
  drives a GPIO only, `sim pin gp_b` counts the edges),
  the watchdog, the RTC calibration and the hardware AES (the soft-SE AES is
  used).
- **Reboot**: the flash file persists; the emulated chips start from their
  defaults (a real ST25DV keeps its EEPROM and field state).
- **TimeAns / DeviceTimeAns** use the host clock.
- Some early-boot log lines can be dropped when the log buffer overflows at
  start-up.
