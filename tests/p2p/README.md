# P2P gw-sim bench firmware

Standalone, minimal firmware for the "network side" of the two-STICKER bench
rig (`doc/p2p.md` §14, issue #118). Flash this onto a SECOND STICKER (not the
device under test) to listen for / inject raw P2P frames without any real
FIBER v2 gateway or central service.

Deliberately separate from the main app: only `app_ccm.c` (AES-CCM/CMAC/ECB) is
shared. No sensors, NFC, LoRaWAN, history, or protobuf — this is why it fits
comfortably (~24 % flash / ~21 % RAM) where a `debug.conf` + `CONFIG_RADIO_P2P=y`
combination of the main app currently does not (see `doc/p2p.md` §11).

## Build + flash

```bash
source /home/hymbajs/.venv/bin/activate
west build -b sticker sticker/tests/p2p -d <scratchpad>/build-gwsim
# flash however you flash the main app (JLinkExe -loadfile, preserves nothing
# here since this firmware has no NVS/settings of its own to preserve)
```

## Usage (over RTT, `p2p ...` commands — no other UI)

Nothing persists across reboot: re-enter radio params/keys every session.

```
p2p status                                   # show current config
p2p radio 868100000 10 14                    # freq_hz, sf (6-12), tx_power_dbm
p2p key <app_key 32 hex digits> <serial_number>      # DUT identity (its LoRaWAN AppKey)
p2p session <dev_nonce> <central_nonce>      # derive session_key for data-plane frames
p2p listen on                                # continuous RX: verify/decrypt + log DUT frames
p2p listen off
p2p tx <frame_type> <hex_body> [counter] [dir 0|1]   # craft + send a frame
```

`p2p key`'s `app_key`/`serial_number` must match the device under test's own
provisioned values — `app_key` is the DUT's existing LoRaWAN OTAA AppKey
(read it off the DUT, e.g. `config lrw_appkey` or bench records), the sole
root secret for the whole P2P transport (doc/p2p.md §4, #118 phase 2
revision; there is no separate `join_key`/`secret_key` involved in P2P).
Both sides compute the same tags/keys from the same inputs; nothing is
transmitted or provisioned separately.

`p2p session` derives the data-plane `session_key = AES128-CMAC(app_key,
"HIO-P2P-SES" ‖ 0x01 ‖ dev_nonce(4 BE) ‖ central_nonce(4 BE) ‖
serial_number(4 BE) ‖ zero-pad to 32 B)` — read `dev_nonce`/`central_nonce`
off the join exchange this sim observed or crafted (this sim does not track
a join state machine itself).

`p2p listen on` logs every received frame's header (`net_id`/`dev_addr`/
`frame_type`/`counter`) and RSSI/SNR. JoinRequest/JoinAccept bodies are
cleartext and logged with their CMAC tag verdict (needs `p2p key`);
data-plane bodies decrypt under `session_key` (needs `p2p session`) — feed
the hex to `app/decoder/ttn.js`/`p2p.js` to decode the actual payload.

`p2p tx` is for manual frame injection — the sim side's JoinAccept/Ack
injector for the join/ACK handshake, and raw radio testing generally.
`JOIN_REQUEST` (0xF0) / `JOIN_ACCEPT` (0xF1) frames get a cleartext body +
16 B CMAC tag under `app_key`; any other frame type gets AES-CCM under
`session_key`. The frame-type/counter/dir arguments are fully free-form.

## Wire format

Mirrors `app_p2p.c` exactly (kept in sync **by hand** — this is a deliberately
separate, minimal firmware, not a shared module):

```
header:     net_id(4 BE) | dev_addr(2 BE) | frame_type(1) | counter(4 BE)   11 B

join handshake (0xF0/0xF1, doc/p2p.md §5.3):
  body:     cleartext (JoinRequest 10 B, JoinAccept 15 B)
  tag:      16 B plain AES-CMAC = CMAC(app_key, label ‖ header ‖ body)
            label = "HIO-P2P-JOIN" (0xF0) / "HIO-P2P-ACC" (0xF1); no nonce, no CCM

data plane (telemetry/alarm/response/ack, doc/p2p.md §3.1):
  nonce:    counter(4 BE) | dev_addr(2 BE) | frame_type(1) | direction(1) | 0*5
  crypto:   AES-CCM (AES-128, 4 B tag) under session_key, 11 B header as AAD
```

The join handshake always runs at the pre-join `net_id`/`dev_addr` of `0`
(doc/p2p.md §5.3); data-plane frames use whatever the JoinAccept assigned.
