// On-board test of the UDP audio path (needs the board flashed with the UDP audio source):
//
//   1. flash the UDP build:   PLATFORMIO_BUILD_FLAGS=-DAUDIO_USE_UDP pio run -e esp32-s3-devkitc-1-n32r16v -t upload
//   2. run this:               node test/hardware/test_udp_audio.js
//   3. flash the normal (microphone) build again:   pio run -e esp32-s3-devkitc-1-n32r16v -t upload
//
// With the normal build the board listens to its microphone and ignores UDP audio, so this script stops with a
// message instead of failing. It starts the real laptop server (server/server.js) pointed at the board, compares
// what the board reports at /api/audio with what the server sent, then sends the board malformed packets (NaN,
// infinity, huge numbers, too-short, too-long, random bytes) and checks the features stay within 0..1 and the board
// keeps running. Uses UDP port 49152 and the beacon port 49153, so stop any dev server running on this laptop first.
//
//   VIZ_BOARD  board address (default: the upload_port IP in platformio.ini)
const { spawn } = require('child_process');
const dgram = require('dgram');
const fs = require('fs');
const http = require('http');
const path = require('path');
const { suite } = require('../js/testkit.js');

const root = path.join(__dirname, '..', '..');
const t = suite('udp audio');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const ini = fs.readFileSync(path.join(root, 'platformio.ini'), 'utf8');
const HOST = process.env.VIZ_BOARD || (/upload_port\s*=\s*(\S+)/.exec(ini) || [])[1];
const SERVER_HTTP = 3017;

const get = (host, port, p) => new Promise(resolve => {
  const req = http.get({ host, port, path: p, agent: false, timeout: 4000 }, res => {
    let d = ''; res.on('data', c => d += c);
    res.on('end', () => { try { resolve(JSON.parse(d)); } catch (e) { resolve(null); } });
  });
  req.on('error', () => resolve(null)); req.on('timeout', () => { req.destroy(); resolve(null); });
});
const board = p => get(HOST, 80, p);
const server = p => get('127.0.0.1', SERVER_HTTP, p);
const features = a => a && Array.isArray(a.bands) ? [...a.bands, a.level, a.bass, a.mid, a.treb] : null;
const inRange = f => f && f.length === 12 && f.every(v => typeof v === 'number' && Number.isFinite(v) && v >= 0 && v <= 1);

function packet(values) { const b = Buffer.alloc(values.length * 4); values.forEach((v, i) => b.writeFloatLE(v, i * 4)); return b; }
const udp = dgram.createSocket('udp4');
const send = buf => new Promise(r => udp.send(buf, 49152, HOST, r));

(async () => {
  const first = await board('/api/slots');
  if (!first) { console.log('FAIL: the board at ' + HOST + ' does not answer'); process.exit(1); }
  if (first.source !== 'udp') {
    t.skip('the board is using its ' + first.source + ' as the audio source. This test needs the UDP build (see the top of this file).');
  }

  let child = null;
  try {
    // ---- the real server streams to the board
    child = spawn(process.execPath, [path.join(root, 'server', 'server.js'), HOST], { env: { ...process.env, VIZ_HTTP_PORT: String(SERVER_HTTP) }, stdio: 'ignore', cwd: path.join(root, 'server') });
    for (let i = 0; i < 50 && !(await server('/api/config')); i++) await sleep(200);
    await sleep(1500);                                                  // let it resolve the board and start streaming

    await t.test('the board reports what the server streams', async () => {
      let worst = 0, n = 0, within = 0, changed = new Set();
      for (let k = 0; k < 40; k++) {
        const s1 = features(await server('/api/audio')), b = features(await board('/api/audio')), s2 = features(await server('/api/audio'));
        if (!s1 || !b || !s2) continue;
        n++;
        t.ok(inRange(b), 'the board reports 12 numbers within 0..1');
        // The board's reading was taken between the two server readings, so it lies between them (give or take the
        // few tens of ms the HTTP requests take and a packet in flight).
        let ok = true;
        for (let i = 0; i < 12; i++) {
          const lo = Math.min(s1[i], s2[i]) - 0.12, hi = Math.max(s1[i], s2[i]) + 0.12;
          if (b[i] < lo || b[i] > hi) ok = false;
          worst = Math.max(worst, Math.min(Math.abs(b[i] - s1[i]), Math.abs(b[i] - s2[i])));
        }
        if (ok) within++;
        changed.add(b[0].toFixed(2));
        await sleep(120);
      }
      t.ok(n >= 30, 'enough samples were taken (' + n + ')');
      t.ok(within >= n * 0.9, within + ' of ' + n + ' board readings match the server\'s values around them (the largest gap was ' + worst.toFixed(3) + ')');
      t.ok(changed.size > 5, 'the values move over time (' + changed.size + ' distinct readings of band 0): the stream is live');
    });

    await t.test('each band is in the right slot of the packet (the board matches the server band by band)', async () => {
      // Bands move at different rates (1.3 + b*0.4 rad/s), so a swapped pair would not track the server.
      const err = Array(8).fill(0); let n = 0;
      for (let k = 0; k < 40; k++) {
        const s1 = features(await server('/api/audio')), b = features(await board('/api/audio')), s2 = features(await server('/api/audio'));
        if (!s1 || !b || !s2) continue;
        n++;
        for (let i = 0; i < 8; i++) err[i] += Math.min(Math.abs(b[i] - s1[i]), Math.abs(b[i] - s2[i]));
        await sleep(150);
      }
      err.forEach((e, i) => t.ok(e / n < 0.08, 'band ' + i + ' tracks the server (mean gap ' + (e / n).toFixed(3) + ')'));
    });

    await t.test('level, bass, mid and treb arrive as the server computed them', async () => {
      for (let k = 0; k < 10; k++) {
        const b = features(await board('/api/audio')); if (!b) continue;
        const mean = (...i) => i.reduce((s, x) => s + b[x], 0) / i.length;
        t.ok(Math.abs(b[8] - mean(0, 1, 2, 3, 4, 5, 6, 7)) < 0.03, 'level is the mean of the bands (' + b[8].toFixed(3) + ')');
        t.ok(Math.abs(b[9] - mean(0, 1)) < 0.03 && Math.abs(b[10] - mean(2, 3, 4)) < 0.03 && Math.abs(b[11] - mean(5, 6, 7)) < 0.03, 'bass, mid and treble are the means of their bands');
        await sleep(100);
      }
    });

    // ---- stop the server, so nothing overwrites what we send by hand
    child.kill(); child = null;
    await sleep(800);

    const hold = async () => features(await board('/api/audio'));
    const status0 = await board('/api/status');

    await t.test('a packet of NaN is cleaned to 0, not passed on', async () => {
      await send(packet(Array(12).fill(NaN))); await sleep(250);
      const f = await hold();
      t.ok(inRange(f), 'every feature is a finite number within 0..1 (got ' + JSON.stringify(f) + ')');
      t.ok(f && f.every(v => v === 0), 'NaN became 0');
    });
    await t.test('huge, negative and infinite values are clamped', async () => {
      await send(packet([1e30, -1e30, Infinity, -Infinity, 5, -5, 1.0001, -0.0001, 3e38, -3e38, Infinity, -Infinity])); await sleep(250);
      const f = await hold();
      t.eq(f, [1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0], 'each value is limited to 0..1');
    });
    await t.test('a normal packet is accepted and read back exactly', async () => {
      const v = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.45, 0.15, 0.4, 0.7];
      await send(packet(v)); await sleep(250);
      const f = await hold();
      t.ok(f && f.every((x, i) => Math.abs(x - v[i]) < 1e-6), 'the 12 values arrive in order: ' + JSON.stringify(f));
    });
    const valid = [0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85, 0.5, 0.2, 0.4, 0.8];
    const accepted = f => f && f.every((x, i) => Math.abs(x - valid[i]) < 1e-6);
    await t.test('a packet shorter than 48 bytes is ignored, and the receiver keeps working afterwards', async () => {
      await send(packet([0.9, 0.9, 0.9, 0.9, 0.9, 0.9, 0.9, 0.9, 0.9, 0.9, 0.9])); await sleep(250);   // 44 bytes
      const f = await hold();
      t.ok(f && Math.abs(f[0] - 0.1) < 1e-6, 'the short packet changed nothing (band 0 is still 0.1, got ' + (f && f[0]) + ')');
      await send(packet(valid)); await sleep(250);
      t.ok(accepted(await hold()), 'a valid packet sent afterwards is accepted (the board is not left waiting on the short one)');
    });
    await t.test('a packet longer than 48 bytes uses its first 48 bytes, and the receiver keeps working afterwards', async () => {
      const v = [0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.99, 0.99, 0.99, 0.99];
      await send(packet(v)); await sleep(250);
      const f = await hold();
      t.ok(f && f.every(x => Math.abs(x - 0.3) < 1e-6), 'the first 48 bytes are used and the extra bytes ignored: ' + JSON.stringify(f));
      await send(packet(valid)); await sleep(250);
      t.ok(accepted(await hold()), 'a valid packet sent afterwards is accepted');
    });
    await t.test('a single byte, an empty packet and an oversized packet are survived', async () => {
      await send(Buffer.alloc(0)); await send(Buffer.alloc(1)); await send(Buffer.alloc(1400, 0x7f)); await sleep(250);
      await send(packet(valid)); await sleep(250);
      t.ok(accepted(await hold()), 'a valid packet sent afterwards is accepted');
    });
    await t.test('random bytes never push anything out of range, and the board keeps running', async () => {
      let bad = 0;
      for (let k = 0; k < 200; k++) {
        const b = Buffer.alloc(48); for (let i = 0; i < 48; i++) b[i] = Math.floor(Math.random() * 256);
        await send(b);
        if (k % 10 === 9) { await sleep(120); const f = await hold(); if (!inRange(f)) bad++; }
      }
      t.eq(bad, 0, 'every sampled reading stayed within 0..1');
      await send(packet(valid)); await sleep(250);
      t.ok(accepted(await hold()), 'and a valid packet after all that random data is still accepted');
      const s = await board('/api/status');
      t.ok(s && s.abnormalReset === false && (!status0 || s.uptimeS >= status0.uptimeS), 'the board did not restart or crash (uptime ' + (s && s.uptimeS) + ' s)');
    });
  } finally {
    if (child) child.kill();
    udp.close();
  }
  t.done();
})();
