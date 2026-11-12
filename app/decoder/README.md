# STICKER payload codec

| file | |
|---|---|
| `decoder.js` | Source of the LoRaWAN codec, comments stripped (protocol docs in `doc/`) (TTN / TTS / ChirpStack: `decodeUplink`, `encodeDownlink`, `decodeDownlink`). Edit this one. |
| `decoder.min.js` | **Upload this one** as the payload formatter. Minified `decoder.js` (~34 kB; the TTS formatter limit is ~40 kB). |
| `p2p.js` | Hub (raw LoRa P2P) envelope decoder, uses `decoder.js`. |
| `calibration.js` | Calibration helpers. |

After changing `decoder.js`:

```sh
npm run build      # regenerate decoder.min.js (terser 5.46.1), commit it
npm test           # tests on decoder.js
npm run test:min   # the same tests on decoder.min.js
```

CI (`npm run check:min`) fails when `decoder.min.js` is stale, over 38 kB, or
fails the tests.
