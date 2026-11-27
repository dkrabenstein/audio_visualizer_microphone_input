// On-board tests: run against the REAL ESP32 (not part of run_all.js, which needs no hardware).
//
//   VIZ_PORT=COM3 node test/hardware/test_board.js         (board on Wi-Fi and plugged in over USB)
//   node test/hardware/test_board.js                       (no USB: the reboot / NVS-persistence part is skipped)
//
// Environment: VIZ_BOARD  address of the board (default: the upload_port IP in platformio.ini)
//              VIZ_PORT   serial port, used only to reset the board between steps
//
// What it covers
//   API guard (src/net/api_guard.cpp): reads open, POSTs refused without / with a wrong token, accepted with the
//     right one, HEAD/OPTIONS open, the guard answers before unknown routes, refused requests change nothing.
//   NVS saving (strip_config.cpp, audio_source.cpp): a live change is lost on reboot, a saved change survives
//     one, "reset" forgets the saved copy, bad input is refused, tuning values are clamped.
//   Not covered: the setup-access-point exemption (needs a device joined to the board's own network).
//
// Safe by construction: the token is read from include/secrets.h and never printed; the lights, patterns and
// Wi-Fi are never touched; the strip and tuning settings found at the start are put back at the end (also when a
// check fails part-way) and compared with the start.
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');
const { suite } = require('../js/testkit.js');

const root = path.join(__dirname, '..', '..');
const t = suite('board');

// ---- configuration and HTTP client (shared with the other on-board tests)
const { HOST, TOKEN, sleep, request, get, post } = require('./board_common.js');
const PORT = process.env.VIZ_PORT || '';

// ---- state snapshots
async function readStrips() { const j = (await get('/api/strips')).json; return { counts: j.counts, reversed: j.reversed }; }
async function readTuning() { const j = (await get('/api/tuning')).json; return { floors: j.floors, margin: j.margin, minCeil: j.minCeil, smoothness: j.smoothness }; }
const near = (a, b, eps = 1e-3) => a.length === b.length && a.every((v, i) => Math.abs(v - b[i]) <= eps);
const sameStrips = (a, b) => JSON.stringify(a.counts) === JSON.stringify(b.counts) && JSON.stringify(a.reversed) === JSON.stringify(b.reversed);
const sameTuning = (a, b) => near(a.floors, b.floors) && Math.abs(a.margin - b.margin) < 1e-3 && Math.abs(a.minCeil - b.minCeil) < 1e-3 && Math.abs(a.smoothness - b.smoothness) < 1e-3;

// ---- reboot over USB (the same reset line "pio run -t upload" uses)
const py = path.join(os.homedir(), '.platformio', 'penv', 'Scripts', 'python.exe');
const esptool = path.join(os.homedir(), '.platformio', 'packages', 'tool-esptoolpy', 'esptool.py');
async function reboot() {
  // This board is back on the network about a second after a reset, reporting an uptime of 1 s, so it is never
  // seen "down". Detect the restart from the uptime instead: after a reset it is far lower than it would be had
  // the board kept running since we last looked (that includes the ~1.6 s esptool itself takes). uptimeS is
  // whole seconds, so allow 1.2 s of rounding.
  const before = (await get('/api/status')).json;
  const t0 = Date.now();
  const r = spawnSync(py, [esptool, '--chip', 'esp32s3', '--port', PORT, '--after', 'hard_reset', 'read_mac'], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error('could not reset the board over ' + PORT + ': ' + (r.stderr || r.error));
  let sawDown = false;
  for (let i = 0; i < 300; i++) {                       // up to ~90 s: Wi-Fi can be slower than usual
    await sleep(300);
    const s = (await get('/api/status')).json;
    if (!s) { sawDown = true; continue; }
    const expectedIfNotReset = (before ? before.uptimeS : 0) + (Date.now() - t0) / 1000;
    if (sawDown || s.uptimeS < expectedIfNotReset - 1.2) { await sleep(1500); return s; }
  }
  throw new Error('board did not come back after the reset');
}

(async () => {
  const start = await get('/api/status');
  if (start.status !== 200) { console.log('FAIL: the board at ' + HOST + ' does not answer (status ' + start.status + ')'); process.exit(1); }
  const originalStrips = await readStrips();
  const originalTuning = await readTuning();
  console.log('board ' + HOST + ' answers; strips ' + JSON.stringify(originalStrips.counts) + ', ' + (PORT ? 'reset over ' + PORT : 'no USB port given (no reset tests)'));

  try {
    // =================================================================== API guard
    await t.test('reads need no token', async () => {
      for (const p of ['/api/status', '/api/strips', '/api/tuning', '/api/slots', '/api/audio', '/api/wifi']) {
        const r = await get(p);
        t.eq(r.status, 200, 'GET ' + p + ' without a token');
      }
      t.eq((await get('/')).status, 200, 'the web page itself is open');
    });

    await t.test('HEAD and OPTIONS (browser preflight) are not refused', async () => {
      for (const m of ['HEAD', 'OPTIONS']) {
        const r = await request(m, '/api/strips');
        t.ok(r.status !== 401 && r.status !== 0, m + ' /api/strips is not a 401 (got ' + r.status + ')');
      }
    });

    await t.test('a POST without a token is refused with a JSON 401', async () => {
      const r = await request('POST', '/api/auth');
      t.eq(r.status, 401, 'status');
      t.ok(/json/.test(r.type), 'content type is JSON: ' + r.type);
      t.eq(r.json && r.json.error, 'missing or wrong API token', 'the message');
    });

    await t.test('wrong tokens are all refused', async () => {
      const flip = (s, i) => s.slice(0, i) + (s[i] === 'x' ? 'y' : 'x') + s.slice(i + 1);
      const swapped = TOKEN === TOKEN.toUpperCase() ? TOKEN.toLowerCase() : TOKEN.toUpperCase();
      const wrong = { 'empty': '', 'one letter': 'x', 'first character changed': flip(TOKEN, 0), 'last character changed': flip(TOKEN, TOKEN.length - 1),
        'middle character changed': flip(TOKEN, TOKEN.length >> 1), 'one character short': TOKEN.slice(0, -1), 'one character extra': TOKEN + '0',
        'token twice': TOKEN + TOKEN };
      if (swapped !== TOKEN) wrong['letter case swapped'] = swapped;
      for (const [label, tok] of Object.entries(wrong)) {
        const r = await request('POST', '/api/auth', { token: tok });
        t.eq(r.status, 401, 'token ' + label);
      }
    });

    await t.test('the right token is accepted', async () => {
      const r = await post('/api/auth');
      t.eq([r.status, r.json && r.json.ok], [200, true], 'POST /api/auth');
    });

    await t.test('the guard answers before routing: unknown POST paths are 401 without a token, 404 with one', async () => {
      t.eq((await request('POST', '/api/does-not-exist')).status, 401, 'no token');
      t.eq((await post('/api/does-not-exist')).status, 404, 'right token');
    });

    await t.test('a refused request changes nothing', async () => {
      const s0 = await readStrips(), u0 = await readTuning();
      const a = await request('POST', '/api/strips', { body: { counts: [61, 2, 3, 4], reversed: [true, true, true, true] } });
      const b = await request('POST', '/api/tuning', { body: { margin: 19, smoothness: 3 } });
      const c = await request('POST', '/api/strips', { token: 'wrong', body: { counts: [61, 2, 3, 4] } });
      t.eq([a.status, b.status, c.status], [401, 401, 401], 'all three refused');
      t.ok(sameStrips(await readStrips(), s0), 'strip settings unchanged');
      t.ok(sameTuning(await readTuning(), u0), 'tuning unchanged');
    });

    await t.test('many wrong tries do not lock the right token out, and the board stays healthy', async () => {
      for (let i = 0; i < 25; i++) await request('POST', '/api/auth', { token: 'bad' + i });
      t.eq((await post('/api/auth')).status, 200, 'right token still works after 25 refusals');
      const s = (await get('/api/status')).json;
      t.ok(s && s.abnormalReset === false, 'no crash restart reported');
    });

    // =================================================================== validation (no reboot needed)
    await t.test('strip settings: bad input is refused and nothing changes', async () => {
      const s0 = await readStrips();
      for (const [label, body] of [['zero count', { counts: [0, 1, 1, 1] }], ['over the limit', { counts: [301, 1, 1, 1] }],
        ['fraction', { counts: [1.5, 1, 1, 1] }], ['negative', { counts: [-5, 1, 1, 1] }], ['too few counts', { counts: [1, 1, 1] }],
        ['text count', { counts: ['a', 1, 1, 1] }], ['too few flags', { reversed: [true] }]]) {
        const r = await post('/api/strips', body);
        t.eq(r.status, 400, label + ' is a 400');
      }
      t.eq((await post('/api/strips', '{not json')).status, 400, 'malformed JSON is a 400');
      t.ok(sameStrips(await readStrips(), s0), 'strips unchanged after all of that');
      const edge = await post('/api/strips', { counts: [1, 300, 1, 300] });
      t.eq(edge.status, 200, 'the limits 1 and 300 themselves are accepted');
      t.eq((await readStrips()).counts, [1, 300, 1, 300], 'and applied');
    });

    await t.test('mic tuning: bad input is refused, extreme values are clamped', async () => {
      t.eq((await post('/api/tuning', { floors: [1, 2, 3] })).status, 400, 'wrong number of floors');
      t.eq((await post('/api/tuning', '{oops')).status, 400, 'malformed JSON');
      t.eq((await post('/api/tuning', { floors: Array(8).fill(50), margin: 99, minCeil: -500, smoothness: 500 })).status, 200, 'extreme values are accepted...');
      const hi = await readTuning();
      t.ok(hi.floors.every(v => v === -20) && hi.margin === 20 && hi.minCeil === -80 && hi.smoothness === 100, '...and clamped to the limits: ' + JSON.stringify([hi.floors[0], hi.margin, hi.minCeil, hi.smoothness]));
      await post('/api/tuning', { floors: Array(8).fill(-500), margin: -9, minCeil: 5, smoothness: -5 });
      const lo = await readTuning();
      t.ok(lo.floors.every(v => v === -120) && lo.margin === 0 && lo.minCeil === -20 && lo.smoothness === 0, 'and the other end: ' + JSON.stringify([lo.floors[0], lo.margin, lo.minCeil, lo.smoothness]));
      await post('/api/tuning', originalTuning);   // back to the starting values (live)
      t.ok(sameTuning(await readTuning(), originalTuning), 'starting values put back');
      await post('/api/strips', originalStrips);
    });

    // =================================================================== NVS persistence (needs a reset)
    if (!PORT) {
      console.log('SKIP: no VIZ_PORT given, so the reboot / NVS-persistence checks were not run');
    } else {
      const testStrips = { counts: [30, 2, 3, 4], reversed: [true, false, true, false] };
      const testTuning = { floors: [-100, -99, -98, -97, -96, -95, -94, -93], margin: 7, minCeil: -60, smoothness: 70 };

      await t.test('a live change is applied at once but is lost on reboot', async () => {
        t.eq((await post('/api/strips', testStrips)).status, 200, 'strips set');
        t.eq((await post('/api/tuning', testTuning)).status, 200, 'tuning set');
        t.ok(sameStrips(await readStrips(), testStrips) && sameTuning(await readTuning(), testTuning), 'both are live');
        t.eq((await get('/api/slots')).json.stripLengths, testStrips.counts, 'the renderer uses the new lengths (slots.stripLengths)');
        await reboot();
        t.ok(sameStrips(await readStrips(), originalStrips), 'after a reboot the strips are back to the saved ones');
        t.ok(sameTuning(await readTuning(), originalTuning), 'after a reboot the tuning is back to the saved one');
      });

      await t.test('a saved change survives a reboot', async () => {
        await post('/api/strips', testStrips); await post('/api/tuning', testTuning);
        t.eq((await post('/api/leds/save')).status, 200, 'strips saved');
        t.eq((await post('/api/mic/save')).status, 200, 'tuning saved');
        await reboot();
        t.ok(sameStrips(await readStrips(), testStrips), 'strips are the saved ones after a reboot: ' + JSON.stringify(await readStrips()));
        t.ok(sameTuning(await readTuning(), testTuning), 'tuning is the saved one after a reboot');
        t.eq((await get('/api/slots')).json.stripLengths, testStrips.counts, 'and the renderer uses them');
      });

      await t.test('save needs the token', async () => {
        t.eq((await request('POST', '/api/leds/save')).status, 401, 'strips save refused without a token');
        t.eq((await request('POST', '/api/mic/save')).status, 401, 'tuning save refused without a token');
      });

      await t.test('reset restores the built-in defaults and forgets the saved copy', async () => {
        const defaults = (await get('/api/strips')).json.defaults;
        t.eq((await post('/api/leds/reset')).status, 200, 'strip reset');
        t.eq((await post('/api/mic/reset')).status, 200, 'tuning reset');
        t.eq((await readStrips()).counts, defaults, 'strips are the as-built counts at once');
        const d = await readTuning();
        t.ok(near(d.floors, [-78, -80, -84, -86, -85, -89, -92, -95]) && d.margin === 5 && d.minCeil === -55 && d.smoothness === 40, 'tuning is the built-in default at once');
        await reboot();
        t.eq((await readStrips()).counts, defaults, 'after a reboot the strips are still the defaults (the saved copy is gone)');
        const d2 = await readTuning();
        t.ok(near(d2.floors, d.floors) && d2.margin === 5, 'and so is the tuning');
      });
    }
  } catch (e) {
    t.ok(false, 'test run aborted: ' + (e && e.stack ? e.stack.split('\n').slice(0, 3).join(' | ') : e));
  } finally {
    // ---- put the board back exactly as found, saved copy included
    console.log('restoring the original settings...');
    const ok1 = (await post('/api/strips', originalStrips)).status === 200;
    const ok2 = (await post('/api/tuning', originalTuning)).status === 200;
    const ok3 = (await post('/api/leds/save')).status === 200;
    const ok4 = (await post('/api/mic/save')).status === 200;
    t.ok(ok1 && ok2 && ok3 && ok4, 'the original settings were re-applied and saved');
    if (PORT) { try { await reboot(); } catch (e) { t.ok(false, String(e)); } }
    t.ok(sameStrips(await readStrips(), originalStrips), 'FINAL: strips are as they were at the start: ' + JSON.stringify((await readStrips()).counts));
    t.ok(sameTuning(await readTuning(), originalTuning), 'FINAL: tuning is as it was at the start');
    t.eq((await get('/api/status')).json.abnormalReset, false, 'FINAL: the board never crashed during the tests');
  }
  t.done();
})();
