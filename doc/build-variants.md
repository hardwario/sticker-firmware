# Build variants

CI builds three images:

| Image | Command | Purpose |
|---|---|---|
| release | `west build -b sticker app` | Production firmware |
| debug | `west build -b sticker app -- -DEXTRA_CONF_FILE=debug.conf` | Development: RTT shell, fault dumps |
| provisioning | `west build -b sticker app -- "-DEXTRA_CONF_FILE=debug.conf;provisioning.conf"` | Production test: debug + 1-Wire, accelerometer, PIR, buzzer |

The provisioning image is the debug image with the peripherals needed for the
production test turned back on (`app/provisioning.conf`): `CONFIG_W1`,
`CONFIG_LIS2DH`, `CONFIG_APP_PYQ1648`, `CONFIG_APP_BUZZER`. History is off
entirely (`CONFIG_APP_HISTORY=n`): a no-op stub reports an empty buffer, so
`req_history` returns no data and there is no `history` shell. Everything else
in the table below stays off.

## What the debug image turns off

The debug image adds the RTT shell and fault dumps, so other parts are
removed to fit into flash (`app/debug.conf`):

| Area | Setting | Effect |
|---|---|---|
| Logging | `CONFIG_LOG=n` | No log output on RTT; use shell commands instead |
| Flash history | `CONFIG_APP_HISTORY_FLASH=n` | History only in RAM (`CONFIG_APP_HISTORY_BYTES=992`), lost on reset |
| Event log | (follows flash history) | Replaced by a no-op stub: no fPort 4, `req_events` returns no data, no `eventlog` shell |
| LoRaWAN regions | `CONFIG_LORAMAC_REGION_AS923/AU915/US915=n` | EU868 only |
| P2P radio | `CONFIG_RADIO_P2P=n` | No raw-LoRa / HARDWARIO Hub mode |
| 1-Wire | `CONFIG_W1=n` | No Dallas temperature probes, no `w1` shell |
| Accelerometer | `CONFIG_LIS2DH=n` | No motion/tilt features |
| PIR | `CONFIG_APP_PYQ1648=n` | No PIR motion |
| Buzzer | `CONFIG_APP_BUZZER=n` | No buzzer |
| Power management | `CONFIG_PM=n` | No low-power sleep; consumption is not representative |
| Shell extras | `CONFIG_I2C_SHELL=n`, `CONFIG_LORA_SHELL=n`, `CONFIG_SHELL_VT100_COMMANDS=n` | Fewer shell commands |
| Float printing | `CONFIG_CBPRINTF_FP_SUPPORT=n` | `printk` cannot format floats |

Features that need any of the above must be tested with the release image
(1-Wire, accelerometer, PIR and buzzer also with the provisioning image).
