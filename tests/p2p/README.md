# P2P gw-sim bench firmware

Standalone, minimal firmware for the "network side" of the two-STICKER bench
rig (`doc/p2p.md` §14, issue #118). Flash this onto a SECOND STICKER (not the
device under test) to listen for / inject raw P2P frames without any real
FIBER v2 gateway or central service.

Deliberately separate from the main app: only `app_ccm.c` (AES-CCM/ECB) is
shared. No sensors, NFC, LoRaWAN, history, or protobuf — this is why it fits
comfortably (~24 % flash / ~21 % RAM) where a `debug.conf` + `CONFIG_APP_LORA_P2P=y`
combination of the main app currently does not (see `doc/p2p.md` §11).

## Build + flash

```bash
source /home/hymbajs/.venv/bin/activate
west build -b sticker sticker/tests/p2p -d <scratchpad>/build-gwsim
# flash however you flash the main app (JLinkExe -loadfile, preserves nothing
# here since this firmware has no NVS/settings of its own to preserve)
```

## Usage (over RTT, `p2p ...` commands — no other UI)

Nothing persists across reboot: re-enter radio params/key every session.

```
p2p status                                   # show current config
p2p radio 868100000 10 14                    # freq_hz, sf (6-12), tx_power_dbm
p2p key <secret_key 32 hex digits> <serial_number>   # DUT identity -> join_key
p2p listen on                                # continuous RX: decrypt + log DUT frames
p2p listen off
p2p tx <frame_type> <hex_body> [counter] [dir 0|1]   # encrypt + send a frame
```

`p2p key`'s `secret_key`/`serial_number` must match the device under test's own
provisioned values (read them off the DUT, e.g. bench records or
`ats device info`) — `join_key = AES128-ECB(secret_key, "HIO-P2P-JOIN" ||
serial_number)`, doc/p2p.md §4. Both sides derive the same key from the same
inputs; nothing is transmitted or provisioned separately.

`p2p listen on` logs every received frame's header (`net_id`/`dev_addr`/
`frame_type`/`counter`), RSSI/SNR, and — once a key is set — the decrypted body
hex (feed it to `app/decoder/ttn.js`/`p2p.js` to decode the actual payload).

`p2p tx` is for manual frame injection: phase 1's DUT is TX-only (no RX/join),
so this is mainly useful for raw radio testing today and becomes the sim
side's JoinAccept/Ack injector once phase 2 (join/ACK state machine) lands —
the frame-type/counter/dir arguments are already fully free-form for that.

## Wire format

Mirrors `app_p2p.c` exactly (kept in sync **by hand** — this is a deliberately
separate, minimal firmware, not a shared module):

```
header:  net_id(4 BE) | dev_addr(2 BE) | frame_type(1) | counter(4 BE)   11 B
nonce:   counter(4 BE) | dev_addr(2 BE) | frame_type(1) | direction(1) | 0*5
crypto:  AES-CCM (AES-128, 4 B tag), 11 B header as AAD
```

Phase 1: `net_id`/`dev_addr` are always `0` (the fixed pre-join value,
doc/p2p.md §5.3) on both the DUT and this sim.
