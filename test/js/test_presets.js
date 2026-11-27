// Tests for data/www/presets.js: the built-in patterns, slot numbers, and the white-light colours.
const fs = require('fs');
const path = require('path');
const { loadScript } = require('./load_web.js');
const { compile, evalCode } = require('./load_compiler.js');

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('FAIL:', m); } };

const { PRESETS, PRESET_FIRST_SLOT, ADVANCED_SLOT, BASIC_SLOT, WHITE_MIN_K, WHITE_MAX_K, whiteRGB, whiteName } =
  loadScript('presets.js',
    ['PRESETS', 'PRESET_FIRST_SLOT', 'ADVANCED_SLOT', 'BASIC_SLOT', 'WHITE_MIN_K', 'WHITE_MAX_K', 'whiteRGB', 'whiteName']);

// Limits live in the firmware header; read them so the test follows the firmware.
const ops = fs.readFileSync(path.join(__dirname, '..', '..', 'src', 'expr_ops.h'), 'utf8');
const define = n => Number(new RegExp('#define\\s+' + n + '\\s+(\\d+)').exec(ops)[1]);
const NUM_SLOTS = define('EXPR_NUM_SLOTS'), NAME_MAX = define('EXPR_NAME_MAX');

// ---- presets
ok(Array.isArray(PRESETS) && PRESETS.length > 0, 'PRESETS is a non-empty array');
ok(new Set(PRESETS.map(p => p.name)).size === PRESETS.length, 'preset names are unique');
ok(PRESETS[0].name === 'audio_reactive', 'slot 0 is the firmware default, audio_reactive');

const contexts = [];
for (let k = 0; k < 40; k++) {
  const r = n => ((k * 7919 + n * 104729) % 1000) / 1000;   // deterministic pseudo-random 0..1
  const bands = [0, 1, 2, 3, 4, 5, 6, 7].map(r);
  contexts.push({ x: k / 39, t: k * 1.7, i: k % 4, bands,
                  level: bands.reduce((a, b) => a + b, 0) / 8,
                  bass: (bands[0] + bands[1]) / 2, mid: (bands[2] + bands[3] + bands[4]) / 3,
                  treb: (bands[5] + bands[6] + bands[7]) / 3 });
}
contexts.push({ x: 0, t: 0, i: 0, bands: [0, 0, 0, 0, 0, 0, 0, 0], level: 0, bass: 0, mid: 0, treb: 0 });   // silence
contexts.push({ x: 1, t: 3600, i: 3, bands: [1, 1, 1, 1, 1, 1, 1, 1], level: 1, bass: 1, mid: 1, treb: 1 }); // full scale

for (const p of PRESETS) {
  ok(typeof p.name === 'string' && /^[A-Za-z0-9_]+$/.test(p.name), p.name + ': name is a plain identifier');
  ok(p.name.length <= NAME_MAX, p.name + ': name fits the firmware limit of ' + NAME_MAX);
  let code = null;
  try { code = compile(p.expr); } catch (e) { ok(false, p.name + ' does not compile: ' + e.message); continue; }
  ok(code.length <= define('EXPR_MAX_OPS'), p.name + ': fits in ' + define('EXPR_MAX_OPS') + ' ops');
  let allFinite = true;
  for (const c of contexts) if (!Number.isFinite(evalCode(code, c))) allFinite = false;
  ok(allFinite, p.name + ': gives a finite value for every input (incl. silence and full scale)');
}

// ---- slots
ok(PRESET_FIRST_SLOT === 0, 'presets start at slot 0');
ok(PRESET_FIRST_SLOT + PRESETS.length - 1 < ADVANCED_SLOT, 'presets do not run into the Custom slot');
ok(ADVANCED_SLOT !== BASIC_SLOT, 'Custom and Basic use different slots');
ok(BASIC_SLOT >= PRESET_FIRST_SLOT + PRESETS.length, 'Basic slot is not a preset slot');
ok(ADVANCED_SLOT < NUM_SLOTS && BASIC_SLOT < NUM_SLOTS, 'special slots exist on the board (' + NUM_SLOTS + ' slots)');
ok(PRESETS.length <= NUM_SLOTS, 'presets fit in the board slots');

// ---- white light
ok(WHITE_MIN_K < WHITE_MAX_K, 'white range is ordered');
let valid = true;
for (let k = WHITE_MIN_K - 500; k <= WHITE_MAX_K + 500; k += 50) {
  const c = whiteRGB(k);
  if (c.length !== 3 || !c.every(v => Number.isInteger(v) && v >= 0 && v <= 255)) valid = false;
}
ok(valid, 'whiteRGB always returns three integers 0..255, even outside the range');
ok(JSON.stringify(whiteRGB(WHITE_MIN_K - 1000)) === JSON.stringify(whiteRGB(WHITE_MIN_K)), 'below the range clamps to the warm end');
ok(JSON.stringify(whiteRGB(WHITE_MAX_K + 1000)) === JSON.stringify(whiteRGB(WHITE_MAX_K)), 'above the range clamps to the cool end');
const warm = whiteRGB(WHITE_MIN_K), cool = whiteRGB(WHITE_MAX_K);
ok(warm[0] === 255 && warm[0] > warm[1] && warm[1] > warm[2], 'warm white is red-heavy (r > g > b)');
ok(cool[2] > cool[1] && cool[1] > cool[0], 'cool white is blue-heavy (b > g > r): ' + cool);
ok(cool[2] > warm[2] && cool[0] < warm[0], 'going cooler adds blue and takes away red');
let bluePrev = -1, blueRises = true;
for (let k = WHITE_MIN_K; k <= WHITE_MAX_K; k += 50) { const b = whiteRGB(k)[2]; if (b < bluePrev) blueRises = false; bluePrev = b; }
ok(blueRises, 'blue never decreases as the colour temperature goes up');
ok(whiteName(2700) === 'Warm white' && whiteName(3499) === 'Warm white', 'names: warm below 3500 K');
ok(whiteName(3500) === 'White' && whiteName(4999) === 'White', 'names: white from 3500 K to 4999 K');
ok(whiteName(5000) === 'Cool white' && whiteName(6500) === 'Cool white', 'names: cool from 5000 K');

console.log('presets: ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
