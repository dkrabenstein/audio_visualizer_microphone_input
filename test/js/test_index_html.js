// Tests for the page itself (data/www/index.html), without a browser:
//   - structure: the scripts it loads exist, every inline script is valid JavaScript, ids are unique
//     and every id the scripts look up is in the page
//   - slotHash() and presetsOnBoard(), the pure functions that decide whether presets are re-uploaded
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { WWW, extractFunction } = require('./load_web.js');

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('FAIL:', m); } };

const html = fs.readFileSync(path.join(WWW, 'index.html'), 'utf8');

// ---- structure
const scripts = [...html.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/g)].map(m => ({ attrs: m[1], body: m[2] }));
ok(scripts.length > 5, 'found the page\'s scripts (' + scripts.length + ')');
for (const s of scripts) {
  const src = /\bsrc\s*=\s*"([^"]+)"/.exec(s.attrs);
  if (src) {
    if (src[1] === '/host.js') continue;   // only the laptop server provides it; the board 404s it on purpose
    ok(fs.existsSync(path.join(WWW, src[1])), 'script ' + src[1] + ' exists in data/www');
  } else {
    try { new vm.Script(s.body); ok(true, ''); }
    catch (e) { ok(false, 'inline script has a syntax error: ' + e.message + ' (starts: ' + s.body.trim().slice(0, 50).replace(/\s+/g, ' ') + ')'); }
  }
}
ok((html.match(/<script\b/g) || []).length === (html.match(/<\/script>/g) || []).length, 'every <script> is closed');

const ids = [...html.matchAll(/\bid\s*=\s*"([^"]+)"/g)].map(m => m[1]);
const dup = ids.filter((id, k) => ids.indexOf(id) !== k);
ok(dup.length === 0, 'element ids are unique' + (dup.length ? ' (repeated: ' + [...new Set(dup)].join(', ') + ')' : ''));

const body = scripts.filter(s => !/\bsrc\s*=/.test(s.attrs)).map(s => s.body).join('\n');
const wanted = new Set();
for (const m of body.matchAll(/getElementById\(\s*'([^']+)'\s*\)/g)) wanted.add(m[1]);
for (const m of body.matchAll(/\$\(\s*'#([A-Za-z][\w-]*)'\s*\)/g)) wanted.add(m[1]);
const made = new Set([...body.matchAll(/\.id\s*=\s*'([^']+)'/g)].map(m => m[1]));   // elements the scripts create themselves
const missing = [...wanted].filter(id => !ids.includes(id) && !made.has(id));
ok(wanted.size > 10, 'found the ids the scripts look up (' + wanted.size + ')');
ok(missing.length === 0, 'every id the scripts look up exists in the page' + (missing.length ? ' (missing: ' + missing.join(', ') + ')' : ''));

// ---- slotHash / presetsOnBoard, run out of the real page source
const ctx = vm.createContext({});
for (const f of ['compiler.js', 'presets.js'])
  vm.runInContext(fs.readFileSync(path.join(WWW, f), 'utf8'), ctx, { filename: f });
vm.runInContext(extractFunction(html, 'slotHash') + '\n' + extractFunction(html, 'presetsOnBoard'), ctx);
const slotHash = vm.runInContext('slotHash', ctx);
const presetsOnBoard = vm.runInContext('presetsOnBoard', ctx);
const compile = vm.runInContext('compile', ctx);
const PRESETS = vm.runInContext('PRESETS', ctx);
const FIRST = vm.runInContext('PRESET_FIRST_SLOT', ctx);

// Independent FNV-1a (32-bit) over each op byte and the little-endian float32 of its argument.
function fnv(code) {
  let h = 0x811c9dc5n;
  const eat = b => { h = ((h ^ BigInt(b)) * 0x01000193n) & 0xffffffffn; };
  for (const [op, arg] of code) {
    eat(op & 255);
    const buf = Buffer.alloc(4); buf.writeFloatLE(arg === undefined ? 0 : arg, 0);
    for (const b of buf) eat(b);
  }
  return h.toString(16).padStart(8, '0');
}
ok(slotHash([]) === '811c9dc5', 'an empty program hashes to the FNV offset basis: ' + slotHash([]));
for (const src of ['1', 'x', '0.5+0.5*sin(tau*x+t)', 'band(3)*2', PRESETS[1].expr]) {
  const code = compile(src);
  ok(slotHash(code) === fnv(code), 'slotHash(' + src + ') matches an independent FNV-1a: ' + slotHash(code));
}
ok(/^[0-9a-f]{8}$/.test(slotHash(compile('x+1'))), 'fingerprint is 8 lowercase hex digits');
ok(slotHash(compile('x+1')) === slotHash(compile('x+1')), 'fingerprint is repeatable');
ok(slotHash(compile('x+1')) !== slotHash(compile('x+2')), 'a different constant changes the fingerprint');
ok(slotHash(compile('x+1')) !== slotHash(compile('x-1')), 'a different operator changes the fingerprint');
ok(slotHash([[0, -0]]) === slotHash([[0, 0]]), 'negative zero hashes like zero (JSON turns -0 into 0)');
ok(slotHash([[16]]) === slotHash([[16, 0]]), 'an op with no argument hashes like argument 0');

// presetsOnBoard: build the slot list the board would report.
const boardSlots = () => PRESETS.map((p, n) => {
  const code = compile(p.expr);
  return { i: FIRST + n, used: true, name: p.name, ops: code.length, hash: slotHash(code) };
});
ok(presetsOnBoard(boardSlots()) === true, 'all presets present and identical: nothing to upload');
ok(presetsOnBoard([]) === false && presetsOnBoard(undefined) === false && presetsOnBoard(null) === false, 'an empty or missing slot list needs uploading');
{ const s = boardSlots(); s.pop(); ok(presetsOnBoard(s) === false, 'one preset missing: upload'); }
{ const s = boardSlots(); s[2].used = false; ok(presetsOnBoard(s) === false, 'an unused slot: upload'); }
{ const s = boardSlots(); s[1].name = 'something_else'; ok(presetsOnBoard(s) === false, 'a different name: upload'); }
{ const s = boardSlots(); s[3].hash = '00000000'; ok(presetsOnBoard(s) === false, 'same name and length but a different program: upload'); }
{ const s = boardSlots(); s.reverse(); ok(presetsOnBoard(s) === true, 'slot order in the list does not matter, the slot number does'); }
{ const s = boardSlots(); s[0].i = 99; ok(presetsOnBoard(s) === false, 'a preset in the wrong slot: upload'); }
{ const s = boardSlots(); s.forEach(x => delete x.hash); ok(presetsOnBoard(s) === true, 'old firmware without a fingerprint: compared by name and op count'); }
{ const s = boardSlots(); s.forEach(x => delete x.hash); s[4].ops += 1; ok(presetsOnBoard(s) === false, 'old firmware: a different op count is noticed'); }
{ const s = boardSlots(); s.push({ i: 12, used: true, name: 'mine', ops: 3, hash: 'deadbeef' }); ok(presetsOnBoard(s) === true, 'the user\'s own patterns in other slots are ignored'); }

console.log('index.html: ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
