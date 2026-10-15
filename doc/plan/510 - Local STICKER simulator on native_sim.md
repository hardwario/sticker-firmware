# 510 - Local STICKER simulator on native_sim

Goal: test as much as possible without a STICKER, a gateway or a phone. The
real app runs on `native_sim/native/64`; the hardware is emulated at bus
level, the far ends of the radio and NFC are host Python tools.

| Phase | Content | Commit |
|---|---|---|
| 1 | Board files, SHT40/OPT3001/ADC/GPIO/flash, reboot by re-exec, `sim` shell | d9d7c064 |
| 2 | SX1262 emulator, UDP air, `sim_air.py` LoRaWAN LNS + TTN decoder | a2262dbc |
| 3 | ST25DV04K emulator (FTM mailbox, GPO), `sim_nfc.py` | 36a1d2c9 |
| 4 | P2P hub (`sim_p2p.py`, TOWER crypto, CTRL tail) | 63b51200 |
| 5 | MPL3115A2, LIS2DH12 (+ `sim_gpio` INT port), DS2484 + DS18B20 | 63d1f2f4 |
| 6 | `smoke_test.py`, CI job, `doc/simulator.md` | 6ff45477 + doc |

Out of scope: FSK P2P profile, DS28E17, PM/power model, timing-exact air.

Finding: LIS2DH free-fall fires once per arm (driver INT1 callback shares
the app's pin and disables it). Separate fix.
