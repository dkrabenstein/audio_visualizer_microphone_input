// On-board tests of the HTTP API beyond the guard and flash saving (test_board.js):
//
//   node test/hardware/test_board_api.js          (board on Wi-Fi; USB is not needed)
//
// Covers /api/pattern (everything the board must refuse, and what it accepts), /api/select, /api/power,
// /api/brightness, /api/white, and the safe parts of the Wi-Fi API (status, scan, input validation).
//
// What it changes, and puts back: the lights DO change while it runs (patterns, power, brightness, white are all
// exercised). Before it starts it records the active pattern, power, brightness and white setting, and in a
// `finally` block it puts them back and checks the board is rendering a valid program (frames advancing).
// Patterns are uploaded only to one spare slot: the one an earlier run named "hw_test", else the highest unused one.
// That slot keeps a tiny "hw_test" program afterwards, because the API has no way to delete a slot. Presets, the
// Custom slot and the Basic slot are never written.
//
// What it will NOT do: send a valid Wi-Fi network (the board drops its connection to trial it) or call
// /api/wifi/forget with the token (it would erase the saved network). Those are only tested without a token,
// where the guard must refuse them.
const fs = require('fs');
const path = require('path');
const { suite } = require('../js/testkit.js');
const { extractFunction, loadScript } = require('../js/load_web.js');
const { compile } = require('../js/load_compiler.js');
const { PRESETS } = loadScript('presets.js', ['PRESETS']);
const net = require('net');
const common = require('./board_common.js');
const { root, HOST, TOKEN, sleep } = common;

// A request that gets no answer at all (status 0) is tried again, up to twice: over Wi-Fi an occasional request is
// lost, and none of the calls below is harmed by being sent twice. How often it happens is checked at the end, so a
// board that really stops answering still fails. (The deliberate "no answer" probes use raw sockets, not these.)
let retries = 0;
async function request(method, urlPath, opts) {
  let r = await common.request(method, urlPath, opts);
  for (let i = 0; i < 2 && r.status === 0; i++) { retries++; await sleep(300); r = await common.request(method, urlPath, opts); }
  return r;
}
const get = p => request('GET', p);
const post = (p, body, token = common.TOKEN) => request('POST', p, { token, body });
const postForm = (p, body, token = common.TOKEN) => request('POST', p, { token, body, form: true });

const t = suite('board api');

// ---- the page's own program fingerprint, to compare with the board's
const coreJs = fs.readFileSync(path.join(root, 'data', 'www', 'core.js'), 'utf8');
const slotHash = new Function(extractFunction(coreJs, 'slotHash') + '; return slotHash;')();

// ---- opcodes (src/pattern/expr_ops.h)
const OP = { CONST: 0, X: 1, T: 2, ADD: 16, SUB: 17, MUL: 18, DIV: 19, ABS: 35, SQRT: 38, SIN: 33 };
const payload = (slot, code, extra = {}) => Object.assign({ abi: 1, slot, name: 'hw_test', code }, extra);
const slotsNow = async () => (await get('/api/slots')).json;
const slotInfo = async n => (await slotsNow()).slots.find(s => s.i === n);

// Waits for the render task to advance, so "the board is drawing frames" is a measurement, not an assumption.
async function rendering() {
  const a = await slotsNow(); await sleep(400); const b = await slotsNow();
  return { advancing: b.frames > a.frames, fps: b.fps, evalUs: b.evalUs, active: b.active };
}
const msg = r => (r.json && r.json.error) || r.text;

(async () => {
  const start = await slotsNow();
  if (!start) { console.log('FAIL: the board does not answer'); process.exit(1); }
  const original = { active: start.active, on: start.on, brightness: start.brightness, white: start.white };
  const unused = start.slots.filter(s => !s.used).map(s => s.i);
  // Reuse a slot an earlier run named "hw_test": the API cannot delete a slot, so a fresh one every run would use them
  // all up. Otherwise take the highest unused slot. Never an existing pattern.
  const mine = start.slots.filter(s => s.used && s.name === 'hw_test').map(s => s.i);
  const SCRATCH = mine.length ? Math.max(...mine) : (unused.length ? Math.max(...unused) : -1);
  if (SCRATCH < 0) { console.log('FAIL: no unused slot to test with; refusing to overwrite a pattern'); process.exit(1); }
  console.log('board answers; active slot ' + original.active + ', on ' + original.on + ', brightness ' + original.brightness + ', white ' + original.white + '; scratch slot ' + SCRATCH);

  try {
    // =================================================================== /api/pattern: refusals
    const base = [[OP.X]];                         // the one-op program "x"
    await t.test('a valid program is accepted and its slot shows the name, op count and fingerprint', async () => {
      const code = [[OP.X], [OP.CONST, 0.5], [OP.MUL]];
      const r = await post('/api/pattern', payload(SCRATCH, code));
      t.eq(r.status, 200, 'accepted: ' + r.text);
      t.eq([r.json.ok, r.json.slot, r.json.ops, r.json.depth], [true, SCRATCH, 3, 2], 'the answer reports slot, 3 ops and depth 2');
      const s = await slotInfo(SCRATCH);
      t.eq([s.used, s.name, s.ops], [true, 'hw_test', 3], 'the slot is used, named and 3 ops long');
      t.eq(s.hash, slotHash(code), 'the board\'s fingerprint equals the one the web page computes (slotHash in core.js)');
    });

    const refusals = [
      ['malformed JSON', '{oops', /malformed JSON/],
      ['no ABI version', { slot: SCRATCH, name: 'x', code: base }, /ABI/],
      ['the wrong ABI version', payload(SCRATCH, base, { abi: 2 }), /ABI/],
      ['the ABI as text', payload(SCRATCH, base, { abi: '1' }), /ABI/],
      ['a negative slot', payload(-1, base), /slot out of range/],
      ['slot 16 (one past the end)', payload(16, base), /slot out of range/],
      ['a huge slot number', payload(1000000, base), /slot out of range/],
      ['no slot', { abi: 1, name: 'x', code: base }, /slot out of range/],
      ['the slot as text', payload('3', base), /slot out of range/],
      ['no code', { abi: 1, slot: SCRATCH, name: 'x' }, /bad code length/],
      ['an empty program', payload(SCRATCH, []), /bad code length/],
      ['code that is not a list', payload(SCRATCH, 5), /bad code length/],
      ['65 ops (one over the limit)', payload(SCRATCH, [[OP.CONST, 1], ...Array(32).fill([[OP.CONST, 1], [OP.ADD]]).flat(), [OP.ABS], [OP.ABS]].slice(0, 65)), /bad code length/],
      ['400 ops', payload(SCRATCH, Array(400).fill([OP.ABS])), /bad code length/],
      ['an op with nothing in it', payload(SCRATCH, [[]]), /bad op/],
      ['an unknown opcode (200)', payload(SCRATCH, [[200]]), /unknown opcode/],
      ['an unknown opcode in a gap (9)', payload(SCRATCH, [[9]]), /unknown opcode/],
      ['an unknown opcode in a gap (41)', payload(SCRATCH, [[41]]), /unknown opcode/],
      // An opcode is one byte: anything outside 0..255, or not a whole number, is refused rather than wrapped round.
      ['a negative opcode', payload(SCRATCH, [[-1]]), /opcode must be/],
      ['opcode 256 (would wrap to 0)', payload(SCRATCH, [[256, 1]]), /opcode must be/],
      ['opcode 272 (would wrap to 16, add)', payload(SCRATCH, [[OP.CONST, 1], [OP.CONST, 2], [272]]), /opcode must be/],
      ['opcode 1000', payload(SCRATCH, [[1000]]), /opcode must be/],
      ['opcode 4294967297 (would wrap to 1)', payload(SCRATCH, [[4294967297]]), /opcode must be/],
      ['a fractional opcode', payload(SCRATCH, [[4.5, 1]]), /opcode must be/],
      ['an opcode given as text', payload(SCRATCH, [['16']]), /opcode must be/],
      ['an opcode that is null', payload(SCRATCH, [[null]]), /opcode must be/],
      ['an operator with nothing to work on', payload(SCRATCH, [[OP.ADD]]), /underflow/],
      ['a function with nothing to work on', payload(SCRATCH, [[OP.SIN]]), /underflow/],
      ['two values left over', payload(SCRATCH, [[OP.CONST, 1], [OP.CONST, 2]]), /exactly one value/],
      ['a band number above 7', payload(SCRATCH, [[4, 8]]), /operand out of range/],
      ['a negative band number', payload(SCRATCH, [[4, -1]]), /operand out of range/],
      ['a fractional band number', payload(SCRATCH, [[4, 2.5]]), /operand out of range/],
      ['a stack 17 deep', payload(SCRATCH, [...Array(17).fill([OP.CONST, 1]), ...Array(16).fill([OP.ADD])]), /stack overflow/],
    ];
    await t.test('everything invalid is refused with a 400 and a reason, and the slot is left alone', async () => {
      const before = await slotInfo(SCRATCH);
      for (const [label, body, want] of refusals) {
        const r = await post('/api/pattern', body);
        t.eq(r.status, 400, label + ' is a 400 (got ' + r.status + ': ' + msg(r) + ')');
        t.ok(want.test(msg(r)), label + ': the reason says "' + msg(r) + '"');
      }
      const after = await slotInfo(SCRATCH);
      t.eq([after.hash, after.name, after.ops], [before.hash, before.name, before.ops], 'after all of that the slot still holds the same program');
    });

    await t.test('a POST with an empty body is answered at once with a 400, on every endpoint that reads a JSON body', async () => {
      // These routes answer from inside the body callback, which only runs when there is a body. Before the fix an
      // empty body got no answer and the board dropped the connection after about 3.5 s.
      const before = await slotInfo(SCRATCH);
      const tuning0 = (await get('/api/tuning')).json, strips0 = (await get('/api/strips')).json, wifi0 = (await get('/api/wifi')).json;
      for (const p of ['/api/pattern', '/api/tuning', '/api/strips', '/api/wifi']) {
        const t0 = Date.now();
        const r = await request('POST', p, { token: TOKEN, body: '' });
        const secs = (Date.now() - t0) / 1000;
        t.eq(r.status, 400, p + ': an empty body is a 400 (got ' + r.status + ')');
        t.eq(msg(r), 'empty body', p + ': the reason is "empty body"');
        t.ok(secs < 2, p + ': answered at once (' + secs.toFixed(2) + ' s), not after the old 3.5 s timeout');
      }
      t.eq((await request('POST', '/api/pattern', { body: '' })).status, 401, 'an empty body without a token is still refused by the guard first (401)');
      // Nothing changed, and a normal request still works straight afterwards.
      t.eq((await slotInfo(SCRATCH)).hash, before.hash, 'the slot is unchanged');
      t.eq([(await get('/api/tuning')).json.margin, (await get('/api/strips')).json.counts], [tuning0.margin, strips0.counts], 'tuning and strip settings are unchanged');
      const w1 = (await get('/api/wifi')).json;
      t.eq([w1.state, w1.ssid, w1.ip], [wifi0.state, wifi0.ssid, wifi0.ip], 'the Wi-Fi state is unchanged');
      t.eq((await post('/api/pattern', payload(SCRATCH, [[OP.X]]))).status, 200, 'a normal upload still works afterwards');
      const st = (await get('/api/status')).json;
      t.ok(st && st.abnormalReset === false, 'the board is healthy');
    });

    await t.test('every program the web page can produce is accepted: the presets and a spread of expressions', async () => {
      // Compiled by the page's own compiler.js and sent exactly as the page sends it (to the scratch slot only). The
      // board's fingerprint of each must equal the page's, so both sides agree on what was stored.
      const expressions = [...PRESETS.map(p => p.expr),
        '0.5+0.5*sin(tau*x+t)', 'fract(x*8+t*0.5)', 'tri(x+t)', 'sat(bass*3-1)', 'band(0)*2+treb', 'level*mid-bass',
        'min(x,0.5)+max(x,0.5)', 'pow(x,2)+sqrt(x)', 'abs(x-0.5)+floor(x*10)', 'cos(x*tau) % 0.3', '-x^2', '1/0', 'i*0.25+x'];
      const bad = [];
      for (const src of expressions) {
        const code = compile(src);
        const r = await post('/api/pattern', payload(SCRATCH, code, { name: 'page_check' }));
        if (r.status !== 200) { bad.push(src + ' -> ' + r.status + ' ' + msg(r)); continue; }
        const s = await slotInfo(SCRATCH);
        if (s.hash !== slotHash(code) || s.ops !== code.length) bad.push(src + ' -> stored as ' + s.hash + '/' + s.ops + ', the page expects ' + slotHash(code) + '/' + code.length);
      }
      t.eq(bad, [], 'all ' + expressions.length + ' compiled programs were accepted and stored as the page expects');
      await post('/api/pattern', payload(SCRATCH, [[OP.X]]));          // back to the plain test program
    });

    await t.test('limits: 64 ops and a stack 16 deep are accepted', async () => {
      const longCode = [[OP.CONST, 1]]; while (longCode.length < 63) longCode.push([OP.CONST, 1], [OP.ADD]); longCode.push([OP.ABS]);
      t.eq(longCode.length, 64, 'built a 64-op program');
      const a = await post('/api/pattern', payload(SCRATCH, longCode));
      t.eq([a.status, a.json && a.json.ops], [200, 64], '64 ops: ' + msg(a));
      const deep = [...Array(16).fill([OP.CONST, 1]), ...Array(15).fill([OP.ADD])];
      const b = await post('/api/pattern', payload(SCRATCH, deep));
      t.eq([b.status, b.json && b.json.depth], [200, 16], 'depth 16: ' + msg(b));
    });

    await t.test('guarded maths is accepted (division by zero, square root of a negative)', async () => {
      const r = await post('/api/pattern', payload(SCRATCH, [[OP.CONST, 1], [OP.CONST, 0], [OP.DIV], [OP.CONST, -4], [OP.SQRT], [OP.ADD]]));
      t.eq(r.status, 200, 'accepted: ' + msg(r));
    });

    await t.test('names: long names are cut to 31 characters, a missing or non-text name becomes "untitled"', async () => {
      await post('/api/pattern', payload(SCRATCH, base, { name: 'n'.repeat(200) }));
      t.eq((await slotInfo(SCRATCH)).name, 'n'.repeat(31), 'a 200-character name is cut to 31');
      await post('/api/pattern', { abi: 1, slot: SCRATCH, code: base });
      t.eq((await slotInfo(SCRATCH)).name, 'untitled', 'no name gives "untitled"');
      await post('/api/pattern', payload(SCRATCH, base, { name: 12345 }));
      t.eq((await slotInfo(SCRATCH)).name, 'untitled', 'a number as the name gives "untitled"');
      await post('/api/pattern', payload(SCRATCH, base, { name: 'hw_test' }));
    });

    await t.test('uploading the same program again is accepted and changes nothing', async () => {
      const code = [[OP.X], [OP.CONST, 0.25], [OP.ADD]];
      await post('/api/pattern', payload(SCRATCH, code));
      const a = await slotInfo(SCRATCH);
      const r = await post('/api/pattern', payload(SCRATCH, code));
      const b = await slotInfo(SCRATCH);
      t.eq([r.status, b.hash], [200, a.hash], 'accepted, same fingerprint');
    });

    await t.test('every opcode from 0 to 255 is either accepted for what it is or refused, never wrapped', async () => {
      // The valid ones (0..8 sources, 16..23 operators, 32..40 functions) in a program that makes each legal; all
      // others must be refused with the "unknown opcode" reason, because they fit in a byte but are not opcodes.
      const sources = [0, 1, 2, 3, 4, 5, 6, 7, 8], binary = [16, 17, 18, 19, 20, 21, 22, 23], unary = [32, 33, 34, 35, 36, 37, 38, 39, 40];
      const known = new Set([...sources, ...binary, ...unary]);
      let wrong = [];
      for (const op of [...sources, ...binary, ...unary]) {
        const code = sources.includes(op) ? [[op, 0]] : binary.includes(op) ? [[OP.CONST, 3], [OP.CONST, 2], [op]] : [[OP.CONST, 3], [op]];
        const r = await post('/api/pattern', payload(SCRATCH, code));
        if (r.status !== 200) wrong.push(op + ' refused: ' + msg(r));
      }
      t.eq(wrong, [], 'all 26 real opcodes are accepted');
      let accepted = [];
      for (let op = 0; op < 256; op += (op < 60 ? 1 : 17)) {                 // every byte up to 59, then a spread to 255
        if (known.has(op)) continue;
        const r = await post('/api/pattern', payload(SCRATCH, [[OP.CONST, 3], [OP.CONST, 2], [op]]));
        if (r.status !== 400) accepted.push(op);
      }
      t.eq(accepted, [], 'no byte that is not an opcode is accepted');
    });

    // =================================================================== /api/select
    await t.test('select: a valid slot becomes active and the board keeps rendering', async () => {
      await post('/api/pattern', payload(SCRATCH, [[OP.X]]));
      const r = await postForm('/api/select', 'slot=' + SCRATCH);
      t.eq([r.status, r.json && r.json.ok], [200, true], 'accepted: ' + msg(r));
      const s = await rendering();
      t.eq(s.active, SCRATCH, 'the active slot is now ' + SCRATCH);
      t.ok(s.advancing && s.fps > 5, 'frames are advancing (' + s.fps.toFixed(0) + ' fps, eval ' + s.evalUs + ' us)');
    });
    await t.test('select: the slot can also be given in the query string', async () => {
      const r = await post('/api/select?slot=' + original.active, undefined);
      t.eq(r.status, 200, 'accepted: ' + msg(r));
      t.eq((await slotsNow()).active, original.active, 'and applied');
      await postForm('/api/select', 'slot=' + SCRATCH);
    });
    await t.test('select: unused, out-of-range and missing slots are refused and the pattern keeps running', async () => {
      const spare = unused.find(n => n !== SCRATCH);
      const cases = [['no slot given', undefined, 'form'], ['an unused slot', spare === undefined ? 14 : spare, 'form'], ['slot 16', 16, 'form'], ['slot 99', 99, 'form'], ['slot -1', -1, 'form']];
      for (const [label, n] of cases) {
        const r = n === undefined ? await postForm('/api/select', 'other=1') : await postForm('/api/select', 'slot=' + n);
        t.eq(r.status, 400, label + ' is a 400 (' + msg(r) + ')');
      }
      t.eq((await slotsNow()).active, SCRATCH, 'the active slot did not change');
    });
    await t.test('select: slot numbers that would wrap round a byte are refused, not turned into another slot', async () => {
      // 256 would be slot 0, 272 would be 16 (itself out of range), 257 would be slot 1, and so on.
      for (const n of [256, 257, 272, 512, 1000, 65536, 4294967296, 4294967295]) {
        const r = await postForm('/api/select', 'slot=' + n);
        t.eq(r.status, 400, 'slot=' + n + ' is a 400 (' + msg(r) + ')');
      }
      t.eq((await slotsNow()).active, SCRATCH, 'the active slot did not change');
    });
    await t.test('select: anything that is not a plain whole number is refused, not read as some other slot', async () => {
      // The active slot is SCRATCH, so a value wrongly read as 0 or 3 would visibly change it.
      const odd = ['abc', '', '3abc', '3.9', '3.0', '+3', '03', ' 3', '3 ', '0x3', '1e1', '--3', 'null', 'three'];
      for (const v of odd) {
        const r = await postForm('/api/select', 'slot=' + encodeURIComponent(v));
        t.eq(r.status, 400, 'slot=' + JSON.stringify(v) + ' is a 400 (' + msg(r) + ')');
      }
      t.eq((await post('/api/select?slot=abc', undefined)).status, 400, 'the query string gets the same rule');
      t.eq((await slotsNow()).active, SCRATCH, 'the active slot never changed');
    });
    await t.test('select: every slot that holds a program can still be chosen by its plain number', async () => {
      const used = (await slotsNow()).slots.filter(s => s.used).map(s => s.i);
      const failed = [];
      for (const n of used) {
        const r = await postForm('/api/select', 'slot=' + n);
        if (r.status !== 200 || (await slotsNow()).active !== n) failed.push(n + ' -> ' + r.status);
      }
      t.eq(failed, [], 'all ' + used.length + ' used slots (' + used.join(', ') + ') were selected');
      await postForm('/api/select', 'slot=' + SCRATCH);
    });

    // =================================================================== /api/power
    await t.test('power: on=0 and on=1 switch the lights and are reported', async () => {
      t.eq((await postForm('/api/power', 'on=0')).json, { ok: true, on: false }, 'off is acknowledged');
      t.eq((await slotsNow()).on, false, '/api/slots says off');
      t.eq((await postForm('/api/power', 'on=1')).json, { ok: true, on: true }, 'on is acknowledged');
      t.eq((await slotsNow()).on, true, '/api/slots says on');
      t.eq((await post('/api/power?on=0')).status, 200, 'the query string works too');
      t.eq((await slotsNow()).on, false, 'and is applied');
    });
    await t.test('power: anything but 0 or 1 is refused and nothing changes', async () => {
      const before = (await slotsNow()).on;
      for (const v of ['2', '', 'yes', 'true', '-1', '01', ' 1']) t.eq((await postForm('/api/power', 'on=' + encodeURIComponent(v))).status, 400, 'on=' + JSON.stringify(v) + ' is a 400');
      t.eq((await postForm('/api/power', 'x=1')).status, 400, 'a missing parameter is a 400');
      t.eq((await slotsNow()).on, before, 'the power state did not change');
    });

    // =================================================================== /api/brightness
    await t.test('brightness: 0..100 are applied and reported, including both ends', async () => {
      for (const v of [0, 1, 50, 99, 100]) {
        const r = await postForm('/api/brightness', 'value=' + v);
        t.eq([r.status, r.json && r.json.value, (await slotsNow()).brightness], [200, v, v], 'value ' + v + ' is applied');
      }
      t.eq((await post('/api/brightness?value=40')).status, 200, 'the query string works too');
      t.eq((await slotsNow()).brightness, 40, 'and is applied');
    });
    await t.test('brightness: out-of-range, fractional and non-numeric values are refused and nothing changes', async () => {
      const before = (await slotsNow()).brightness;
      for (const v of ['-1', '101', '1000', '50.5', 'abc', '', ' 50', '050', '+5', '5e1']) t.eq((await postForm('/api/brightness', 'value=' + encodeURIComponent(v))).status, 400, 'value=' + JSON.stringify(v) + ' is a 400');
      t.eq((await postForm('/api/brightness', 'x=1')).status, 400, 'a missing parameter is a 400');
      t.eq((await slotsNow()).brightness, before, 'the brightness did not change');
    });

    // =================================================================== /api/white
    // The board turns a colour temperature into RGB (Tanner Helland's approximation, then a 2.2 gamma decode for the
    // LEDs). Recompute it here, independently, and compare with the colour the board reports it is showing.
    function kelvinToRGB(k) {
      const x = k / 100; let r, g, b;
      if (x <= 66) { r = 255; g = 99.4708025861 * Math.log(x) - 161.1195681661; }
      else { r = 329.698727446 * Math.pow(x - 60, -0.1332047592); g = 288.1221695283 * Math.pow(x - 60, -0.0755148492); }
      if (x >= 66) b = 255; else if (x <= 19) b = 0; else b = 138.5177312231 * Math.log(x - 10) - 305.0447927307;
      const to8 = v => { v = Math.min(255, Math.max(0, v)); return Math.floor(255 * Math.pow(v / 255, 2.2) + 0.5); };
      return [to8(r), to8(g), to8(b)];
    }
    await t.test('white: each temperature is applied and the colour the board reports is the one the formula gives', async () => {
      let worst = 0;
      for (const k of [2000, 2700, 3000, 4000, 5000, 6000, 6500]) {
        const r = await postForm('/api/white', 'kelvin=' + k);
        t.eq([r.status, r.json && r.json.kelvin], [200, k], 'kelvin ' + k + ' accepted');
        const s = await slotsNow();
        t.eq(s.white, k, 'slots.white is ' + k);
        const want = kelvinToRGB(k), got = s.whiteRgb;
        t.ok(Array.isArray(got) && got.length === 3, 'the board reports the RGB it is showing');
        if (got) got.forEach((v, i) => { worst = Math.max(worst, Math.abs(v - want[i])); });
        t.ok(got && got.every((v, i) => Math.abs(v - want[i]) <= 1), 'kelvin ' + k + ': board RGB ' + JSON.stringify(got) + ', formula ' + JSON.stringify(want));
      }
      t.ok(worst <= 1, 'the largest difference over all temperatures is ' + worst + ' (rounding only)');
    });
    await t.test('white: "off" switches it off, a pattern choice ends it, and the boundaries are exact', async () => {
      t.eq((await postForm('/api/white', 'kelvin=off')).json.kelvin, null, 'off is acknowledged');
      t.eq((await slotsNow()).white, null, 'and reported as null');
      await postForm('/api/white', 'kelvin=4000');
      await postForm('/api/select', 'slot=' + SCRATCH);
      t.eq((await slotsNow()).white, null, 'selecting a pattern ends the white light');
      t.eq((await post('/api/white?kelvin=2000')).status, 200, '2000 K (the lowest) via the query string');
      t.eq((await postForm('/api/white', 'kelvin=6500')).status, 200, '6500 K (the highest)');
    });
    await t.test('white: values outside the range or not whole numbers are refused and nothing changes', async () => {
      await postForm('/api/white', 'kelvin=3300');
      for (const v of ['1999', '6501', '0', '-3000', '4000.5', 'warm', '', ' 3000', '03000', '3e3']) t.eq((await postForm('/api/white', 'kelvin=' + encodeURIComponent(v))).status, 400, 'kelvin=' + JSON.stringify(v) + ' is a 400');
      t.eq((await postForm('/api/white', 'x=1')).status, 400, 'a missing parameter is a 400');
      t.eq((await slotsNow()).white, 3300, 'the white light did not change');
      await postForm('/api/white', 'kelvin=off');
    });

    // =================================================================== the guard on these endpoints
    await t.test('without a token every one of these is refused, and nothing changes', async () => {
      const s0 = await slotsNow();
      // Each request below repeats the CURRENT state, so even if the guard failed open nothing would visibly change.
      const attempts = [
        ['/api/pattern', { body: payload(SCRATCH, [[OP.X]]) }],
        ['/api/select', { body: 'slot=' + s0.active, form: true }],
        ['/api/power', { body: 'on=' + (s0.on ? 1 : 0), form: true }],
        ['/api/brightness', { body: 'value=' + s0.brightness, form: true }],
        ['/api/white', { body: 'kelvin=off', form: true }],
        ['/api/wifi', { body: { ssid: 'x', password: '' } }],
        ['/api/wifi/forget', {}],
      ];
      for (const [p, opts] of attempts) t.eq((await request('POST', p, opts)).status, 401, 'POST ' + p + ' without a token is a 401');
      const s1 = await slotsNow();
      t.eq([s1.active, s1.on, s1.brightness, s1.white], [s0.active, s0.on, s0.brightness, s0.white], 'the board state is unchanged');
      t.eq((await get('/api/wifi')).json.connected, true, 'and the board is still on its network (forget was refused)');
    });

    // =================================================================== Wi-Fi (read-only, plus refused input)
    await t.test('wifi: the status shows the network, address, signal and host', async () => {
      const w = (await get('/api/wifi')).json;
      t.eq([w.state, w.connected, w.ap], ['connected', true, false], 'connected, not in setup mode');
      t.ok(typeof w.ssid === 'string' && w.ssid.length > 0, 'has the network name');
      t.eq(w.ip, require('./board_common.js').HOST, 'the IP is the one we are talking to');
      t.ok(Number.isInteger(w.rssi) && w.rssi < 0 && w.rssi > -100, 'a signal strength in dBm (' + w.rssi + ')');
      t.eq(w.host, 'visualizer-esp-32.local', 'the mDNS name');
    });
    await t.test('wifi: a scan finishes and lists networks with name, signal and security', async () => {
      let r = null;
      for (let i = 0; i < 40; i++) { r = (await get('/api/wifi/scan')).json; if (r && !r.scanning) break; await sleep(500); }
      t.ok(r && r.scanning === false && Array.isArray(r.networks), 'the scan completed with a list');
      if (r && r.networks) {
        t.ok(r.networks.length > 0, 'at least one network was found (' + r.networks.length + ')');
        t.ok(r.networks.every(n => typeof n.ssid === 'string' && Number.isInteger(n.rssi) && typeof n.secure === 'boolean'), 'every entry has ssid, rssi and secure');
        t.ok((await get('/api/wifi')).json.connected, 'the board is still connected after scanning');
      }
    });
    await t.test('wifi: invalid network settings are refused before anything is attempted', async () => {
      const w0 = (await get('/api/wifi')).json;
      const cases = [
        ['malformed JSON', '{oops', /malformed/],
        ['no network name', { password: 'longenough1' }, /network name/],
        ['an empty network name', { ssid: '', password: '' }, /network name/],
        ['a 33-character name', { ssid: 'x'.repeat(33), password: '' }, /network name/],
        ['a 7-character password', { ssid: 'SomeNet', password: 'short12' }, /password/],
        ['a 64-character password', { ssid: 'SomeNet', password: 'p'.repeat(64) }, /password/],
      ];
      for (const [label, body, want] of cases) {
        const r = await post('/api/wifi', body);
        t.eq(r.status, 400, label + ' is a 400 (' + msg(r) + ')');
        t.ok(want.test(msg(r)), label + ': the reason says "' + msg(r) + '"');
      }
      const w1 = (await get('/api/wifi')).json;
      t.eq([w1.state, w1.connected, w1.ssid, w1.ip], [w0.state, w0.connected, w0.ssid, w0.ip], 'the board is still on the same network, in the same state');
    });
  } catch (e) {
    t.ok(false, 'test run aborted: ' + (e && e.stack ? e.stack.split('\n').slice(0, 3).join(' | ') : e));
  } finally {
    // ---- put the lights back and make sure a valid program is running
    console.log('restoring the original state...');
    await postForm('/api/select', 'slot=' + original.active);
    if (original.white) await postForm('/api/white', 'kelvin=' + original.white); else await postForm('/api/white', 'kelvin=off');
    await postForm('/api/brightness', 'value=' + original.brightness);
    await postForm('/api/power', 'on=' + (original.on ? 1 : 0));
    await sleep(300);
    const end = await slotsNow();
    t.eq([end.active, end.on, end.brightness, end.white], [original.active, original.on, original.brightness, original.white], 'FINAL: active slot, power, brightness and white are as they were at the start');
    const active = end.slots.find(s => s.i === end.active);
    t.ok(active && active.used && active.ops > 0, 'FINAL: the active slot holds a valid program (' + (active && active.name) + ', ' + (active && active.ops) + ' ops)');
    const render = await rendering();
    t.ok(render.advancing && render.fps > 5, 'FINAL: the board is rendering it (' + render.fps.toFixed(0) + ' fps)');
    const st = (await get('/api/status')).json;
    t.eq(st && st.abnormalReset, false, 'FINAL: the board never crashed during the tests');
    t.ok(retries <= 6, 'FINAL: requests that had to be repeated because the board did not answer: ' + retries + ' (a few lost packets are normal)');
    if (retries) console.log('  (' + retries + ' request(s) were repeated after getting no answer)');
  }
  t.done();
})();
