# STICKER payload codec

| file | |
|---|---|
| `decoder.js` | Source of the LoRaWAN codec, comments stripped (protocol docs in `doc/`) (TTN / TTS / ChirpStack: `decodeUplink`, `encodeDownlink`, `decodeDownlink`). Edit this one. |
| `decoder.tts.js` | **Upload this one** as the payload formatter (TTS / ChirpStack). Built by CI from `decoder.js`, minified (~34 kB; the TTS formatter limit is ~40 kB). |
| `p2p.js` | Hub (raw LoRa P2P) envelope decoder, uses `decoder.js`. |
| `calibration.js` | Calibration helpers. |

`decoder.tts.js` is not committed: CI builds it from `decoder.js`
(`npm run check:tts`: build, size cap 38 kB, the same tests), uploads it as the
`hio-sticker-decoder-tts` artifact of every run and attaches it to a release
(with its SHA256 in `SHA256SUMS`). Local build:

```sh
npm run build      # decoder.tts.js (terser 5.46.1)
npm test           # tests on decoder.js
npm run test:tts   # the same tests on decoder.tts.js
```
