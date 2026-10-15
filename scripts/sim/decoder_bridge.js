// STICKER simulator: runs the application's TTN decoder (app/decoder/ttn.js)
// for sim_air.py. One JSON request per input line, one JSON reply per line:
//   {"op":"up","fPort":10,"bytes":"0a0b..."}  -> decodeUplink() result
//   {"op":"down","input":{...}}               -> encodeDownlink() result

"use strict";

const path = require("path");
const readline = require("readline");

const codec = require(path.join(__dirname, "..", "..", "app", "decoder", "ttn.js"));

const rl = readline.createInterface({ input: process.stdin });

rl.on("line", (line) => {
  let reply;

  try {
    const req = JSON.parse(line);

    if (req.op === "up") {
      reply = codec.decodeUplink({ fPort: req.fPort, bytes: Array.from(Buffer.from(req.bytes, "hex")) });
    } else if (req.op === "down") {
      reply = codec.encodeDownlink(req.input);
    } else {
      reply = { errors: ["unknown op " + req.op] };
    }
  } catch (e) {
    reply = { errors: [String(e)] };
  }
  process.stdout.write(JSON.stringify(reply) + "\n");
});
