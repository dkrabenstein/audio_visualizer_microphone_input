// Tests for server/server.js, the laptop's dev server. It is started for real (on free ports, through the
// VIZ_*_PORT environment variables, so a server you have running is not disturbed) and driven over HTTP
// and UDP, the same way the page and the board use it.
const { spawn } = require('child_process');
const dgram = require('dgram');
const fs = require('fs');
const http = require('http');
const net = require('net');
const path = require('path');

const root = path.join(__dirname, '..', '..');
const WWW = path.join(root, 'data', 'www');
let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('FAIL:', m); } };
const sleep = ms => new Promise(r => setTimeout(r, ms));

function freePort(type) {
  return new Promise((resolve, reject) => {
    if (type === 'udp') {
      const s = dgram.createSocket('udp4');
      s.bind(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); });
      s.on('error', reject);
    } else {
      const s = net.createServer();
      s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); });
      s.on('error', reject);
    }
  });
}

function get(port, urlPath) {
  return new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port, path: urlPath }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => resolve({ status: res.statusCode, type: res.headers['content-type'], body: Buffer.concat(chunks) }));
    }).on('error', reject);
  });
}
function post(port, urlPath, text) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, path: urlPath, method: 'POST' }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => resolve({ status: res.statusCode, body: Buffer.concat(chunks).toString() }));
    });
    req.on('error', reject);
    req.end(text);
  });
}
// A raw request, so the path reaches the server exactly as written (http.get would tidy "/../").
function raw(port, requestPath) {
  return new Promise(resolve => {
    const s = net.connect(port, '127.0.0.1', () =>
      s.write('GET ' + requestPath + ' HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'));
    let data = '';
    s.on('data', d => { data += d; });
    s.on('close', () => resolve(data));
    s.on('error', () => resolve(data));
  });
}
const statusOf = text => Number((/^HTTP\/1\.[01] (\d+)/.exec(text) || [])[1] || 0);

async function start(host, ports) {
  const child = spawn(process.execPath, [path.join(root, 'server', 'server.js'), host], {
    env: { ...process.env, VIZ_HTTP_PORT: String(ports.http), VIZ_UDP_PORT: String(ports.udp), VIZ_BEACON_PORT: String(ports.beacon) },
    stdio: ['ignore', 'pipe', 'pipe'], cwd: path.join(root, 'server'),
  });
  let log = '';
  child.stdout.on('data', d => { log += d; });
  child.stderr.on('data', d => { log += d; });
  child.getLog = () => log;
  for (let i = 0; i < 60; i++) {
    try { await get(ports.http, '/api/config'); return child; } catch (e) { await sleep(100); }
  }
  child.kill();
  throw new Error('server did not start. Output:\n' + log);
}

(async () => {
  const A = { http: await freePort('tcp'), udp: await freePort('udp'), beacon: await freePort('udp') };
  const B = { http: await freePort('tcp'), udp: await freePort('udp'), beacon: await freePort('udp') };

  // Fake "board": receives what the server streams.
  const board = dgram.createSocket('udp4');
  const packets = [];
  board.on('message', m => packets.push(m));
  await new Promise(r => board.bind(A.udp, '127.0.0.1', r));

  const a = await start('127.0.0.1', A);          // host is an IP: the server streams to it
  const b = await start('viz-test.invalid', B);   // host is a name that never resolves: beacon tests
  try {
    // ---- static files
    const index = await get(A.http, '/');
    ok(index.status === 200 && /text\/html/.test(index.type), 'GET / serves HTML');
    ok(index.body.equals(fs.readFileSync(path.join(WWW, 'index.html'))), 'GET / is exactly data/www/index.html (one copy of the page)');
    for (const [file, type] of [['compiler.js', /javascript/], ['presets.js', /javascript/], ['fastled.js', /javascript/]]) {
      const r = await get(A.http, '/' + file);
      ok(r.status === 200 && type.test(r.type) && r.body.equals(fs.readFileSync(path.join(WWW, file))), 'GET /' + file + ' serves the file with a JavaScript type');
    }
    ok(statusOf(await raw(A.http, '/index.html?x=1')) === 200, 'a query string is ignored when finding the file');
    const missing = await get(A.http, '/nope.js');
    ok(missing.status === 404, 'a missing file is a 404');

    // ---- host.js switches the page into laptop mode
    const host = await get(A.http, '/host.js');
    ok(host.status === 200 && /javascript/.test(host.type), '/host.js is served as JavaScript');
    ok(/window\.VIZ_LAPTOP\s*=\s*true/.test(host.body.toString()), '/host.js sets window.VIZ_LAPTOP = true');

    // ---- directory traversal must stay inside data/www
    for (const p of ['/../server/server.js', '/%2e%2e/server/server.js', '/..%2fserver%2fserver.js', '/%2e%2e%2f%2e%2e%2fplatformio.ini'])
      ok(statusOf(await raw(A.http, p)) === 403, 'path ' + p + ' is forbidden (403)');
    // A backslash is a folder separator only on Windows. There, "..\server\server.js" climbs out of data/www and is
    // refused (403). On Linux and macOS it is just an odd file name inside data/www that does not exist (404). Either
    // way nothing outside data/www may be served.
    const backslash = await raw(A.http, '/..%5cserver%5cserver.js');
    ok(statusOf(backslash) === (process.platform === 'win32' ? 403 : 404), 'path /..%5cserver%5cserver.js is refused (403 on Windows, 404 elsewhere); got ' + statusOf(backslash));
    for (const p of ['/../server/server.js', '/%2e%2e/server/server.js', '/..%5cserver%5cserver.js', '/..%2fserver%2fserver.js', '/%2e%2e%2f%2e%2e%2fplatformio.ini'])
      ok(!/createServer|\[env:/.test(await raw(A.http, p)), 'path ' + p + ' never returns the contents of a file outside data/www');

    // ---- /api/audio
    const au = JSON.parse((await get(A.http, '/api/audio')).body.toString());
    ok(Array.isArray(au.bands) && au.bands.length === 8, '/api/audio has 8 bands');
    for (const k of ['level', 'bass', 'mid', 'treb']) ok(typeof au[k] === 'number', '/api/audio has ' + k);
    // Every sample is in range, and the summary values are the same means the firmware uses.
    let inRange = true, summaryOk = true;
    for (let i = 0; i < 20; i++) {
      const j = JSON.parse((await get(A.http, '/api/audio')).body.toString());
      if (![...j.bands, j.level, j.bass, j.mid, j.treb].every(v => v >= -1e-6 && v <= 1 + 1e-6)) inRange = false;
      const m = (...i2) => i2.reduce((s, k) => s + j.bands[k], 0) / i2.length;
      if (Math.abs(j.level - m(0, 1, 2, 3, 4, 5, 6, 7)) > 1e-5 || Math.abs(j.bass - m(0, 1)) > 1e-5 ||
          Math.abs(j.mid - m(2, 3, 4)) > 1e-5 || Math.abs(j.treb - m(5, 6, 7)) > 1e-5) summaryOk = false;
      await sleep(15);
    }
    ok(inRange, 'simulated audio stays within 0..1');
    ok(summaryOk, 'level/bass/mid/treb are the means of their bands (bands 0-7, 0-1, 2-4, 5-7)');

    // ---- /api/config
    const cfg = JSON.parse((await get(A.http, '/api/config')).body.toString());
    ok(cfg.host === '127.0.0.1', 'config shows the host given on the command line');
    ok(cfg.ip === '127.0.0.1' && cfg.source === 'manual', 'an IP address as host is a manual target: ' + JSON.stringify(cfg));
    for (const bad of ['', 'not json', '{}', '{"host":""}', '{"host":"   "}', '[]', 'null'])
      ok((await post(A.http, '/api/config', bad)).status === 400, 'POST /api/config with ' + JSON.stringify(bad) + ' is a 400');
    const setOk = await post(A.http, '/api/config', JSON.stringify({ host: ' 127.0.0.1 ' }));
    const setJson = JSON.parse(setOk.body);
    ok(setOk.status === 200 && setJson.ok === true && setJson.host === '127.0.0.1' && setJson.ip === '127.0.0.1' && setJson.source === 'manual',
       'setting a reachable IP replies ok and trims spaces: ' + setOk.body);
    const setBad = JSON.parse((await post(B.http, '/api/config', JSON.stringify({ host: 'still-not-real.invalid' }))).body);
    ok(setBad.ok === false && setBad.ip === null && setBad.source === 'none' && setBad.error, 'an unresolvable name replies ok:false with an error: ' + JSON.stringify(setBad));

    // ---- UDP stream to the board
    await sleep(400);
    ok(packets.length > 10, 'the server streams packets to the board (' + packets.length + ' in the first moments)');
    ok(packets.every(p => p.length === 48), 'every packet is exactly 48 bytes (12 little-endian floats)');
    const f = i => packets[packets.length - 1].readFloatLE(i * 4);
    const vals = Array.from({ length: 12 }, (_, i) => f(i));
    ok(vals.every(v => Number.isFinite(v) && v >= -1e-6 && v <= 1 + 1e-6), 'packet values are finite and within 0..1');
    const mean = (...ix) => ix.reduce((s, k) => s + vals[k], 0) / ix.length;
    ok(Math.abs(vals[8] - mean(0, 1, 2, 3, 4, 5, 6, 7)) < 1e-5 && Math.abs(vals[9] - mean(0, 1)) < 1e-5 &&
       Math.abs(vals[10] - mean(2, 3, 4)) < 1e-5 && Math.abs(vals[11] - mean(5, 6, 7)) < 1e-5,
       'packet layout is 8 bands, then level, bass, mid, treb (the order AudioFeatures uses)');
    const before = packets.length;
    await sleep(500);
    const rate = (packets.length - before) / 0.5;
    ok(rate > 30 && rate < 90, 'about 60 packets per second (measured ' + rate.toFixed(0) + '/s)');

    // ---- beacon: the board announces itself, the server follows its address
    const udp = dgram.createSocket('udp4');
    const send = text => new Promise(r => udp.send(Buffer.from(text), B.beacon, '127.0.0.1', r));
    let c0 = JSON.parse((await get(B.http, '/api/config')).body.toString());
    ok(c0.ip === null && c0.source === 'none', 'an unresolvable name starts with no address: ' + JSON.stringify(c0));
    await send('HELLO'); await send('viz1'); await send('');
    await sleep(150);
    c0 = JSON.parse((await get(B.http, '/api/config')).body.toString());
    ok(c0.ip === null, 'packets that are not exactly "VIZ1" are ignored');
    await send('VIZ1');
    await sleep(150);
    const c1 = JSON.parse((await get(B.http, '/api/config')).body.toString());
    ok(c1.ip === '127.0.0.1' && c1.source === 'beacon', 'a VIZ1 beacon sets the address and the source is "beacon": ' + JSON.stringify(c1));
    udp.close();

    // ---- a badly encoded path must get an error response, not crash the server
    const bad = await raw(B.http, '/%E0%A4%A');
    ok(statusOf(bad) === 400, 'a badly encoded path is a 400');
    let alive = true;
    try { await get(B.http, '/api/config'); } catch (e) { alive = false; }
    ok(alive, 'the server is still running after a badly encoded path');
  } catch (e) {
    ok(false, 'test run aborted: ' + e.stack);
  } finally {
    a.kill(); b.kill(); board.close();
  }
  console.log('server: ' + pass + ' passed, ' + fail + ' failed');
  process.exit(fail ? 1 : 0);
})();
