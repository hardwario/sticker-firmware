// TTS runs the formatter as a plain script and calls these globals by name;
// terser --toplevel drops any it considers unused.
const fs = require('fs');
const vm = require('vm');
const file = process.argv[2] || 'decoder.tts.js';
const ctx = { module: {} };
vm.runInNewContext(fs.readFileSync(file, 'utf8'), ctx);
const missing = ['decodeUplink', 'encodeDownlink', 'decodeDownlink'].filter((f) => typeof ctx[f] !== 'function');
if (missing.length) {
	console.error(`${file}: missing entry points: ${missing.join(', ')}`);
	process.exit(1);
}
console.log(`${file}: entry points OK`);
