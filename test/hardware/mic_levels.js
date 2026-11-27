// Prints the board's per-band microphone levels (dBFS), averaged over a few seconds: node test/hardware/mic_levels.js [label]
// Used to compare the microphone path before and after a firmware change; ambient noise varies a few dB.
const http = require('http');
const host = process.env.VIZ_BOARD || '192.168.1.82';
const get = p => new Promise(r => http.get({ host, path: p, agent: false, timeout: 4000 }, res => { let d = ''; res.on('data', c => d += c); res.on('end', () => { try { r(JSON.parse(d)); } catch (e) { r(null); } }); }).on('error', () => r(null)));
(async () => {
  const sum = Array(8).fill(0); let n = 0, lvl = 0, src = '';
  for (let i = 0; i < 20; i++) {
    const j = await get('/api/tuning'); const s = await get('/api/slots');
    if (j && j.raw) { j.raw.forEach((v, k) => { sum[k] += v; }); n++; }
    if (s) { lvl += s.level || 0; src = s.source; }
    await new Promise(r => setTimeout(r, 250));
  }
  console.log((process.argv[2] || 'levels') + ' (' + n + ' samples, source ' + src + '): ' + sum.map(v => (v / n).toFixed(1)).join(' ') + ' dBFS, mean level ' + (lvl / 20).toFixed(3));
})();
