// The web page and the firmware each hold their own copy of some things, with comments saying "keep
// these in step". This test checks they are, by reading both sides:
//   - the opcode table, ABI version and limits (src/pattern/expr_ops.h  <->  data/www/compiler.js)
//   - the built-in default pattern (src/pattern/slot_store.cpp slot 0  <->  presets.js PRESETS[0])
//   - the program fingerprint (programHash() in src/pattern/program_hash.h, built with g++  <->  slotHash() in index.html)
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { loadScript, extractFunction, WWW } = require('./load_web.js');
const { compile, evalCode, OP, ABI, MAX_OPS, STACK_MAX } = require('./load_compiler.js');
const { buildCpp } = require('./build_cpp.js');

const root = path.join(__dirname, '..', '..');
let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('FAIL:', m); } };

const opsH = fs.readFileSync(path.join(root, 'src', 'pattern', 'expr_ops.h'), 'utf8');
const slotStoreCpp = fs.readFileSync(path.join(root, 'src', 'pattern', 'slot_store.cpp'), 'utf8');   // holds the default pattern
const networkCpp = fs.readFileSync(path.join(root, 'src', 'net', 'network.cpp'), 'utf8');       // ports and beacon text
const indexHtml = fs.readFileSync(path.join(WWW, 'index.html'), 'utf8');

// ---- opcode table
const cOps = {};
for (const m of opsH.matchAll(/\b(OP_[A-Z]+)\s*=\s*(\d+)/g)) if (m[1] !== 'OP_INVALID') cOps[m[1].slice(3)] = Number(m[2]);
ok(Object.keys(cOps).length === 26, 'found the opcodes in expr_ops.h (' + Object.keys(cOps).length + ')');
for (const [name, num] of Object.entries(cOps))
  ok(OP[name] === num, 'opcode ' + name + ': firmware ' + num + ', compiler.js ' + OP[name]);
for (const name of Object.keys(OP))
  ok(name in cOps, 'compiler.js opcode ' + name + ' exists in expr_ops.h');

// ---- ABI and limits
const define = n => Number(new RegExp('#define\\s+' + n + '\\s+(\\d+)').exec(opsH)[1]);
ok(define('EXPR_ABI_VERSION') === ABI, 'ABI version: firmware ' + define('EXPR_ABI_VERSION') + ', compiler.js ' + ABI);
ok(define('EXPR_MAX_OPS') === MAX_OPS, 'max ops: firmware ' + define('EXPR_MAX_OPS') + ', compiler.js ' + MAX_OPS);
ok(define('EXPR_STACK_MAX') === STACK_MAX, 'stack max: firmware ' + define('EXPR_STACK_MAX') + ', compiler.js ' + STACK_MAX);

// ---- default pattern (slot 0)
const { PRESETS } = loadScript('presets.js', ['PRESETS']);
const defOps = [];
for (const m of slotStoreCpp.matchAll(/defProg\.ops\[(\d+)\]\s*=\s*\{\s*(OP_[A-Z]+)\s*,\s*([-0-9.eE+]+)f?\s*\}/g))
  defOps[Number(m[1])] = [cOps[m[2].slice(3)], Number(m[3])];
const countMatch = /defProg\.count\s*=\s*(\d+)/.exec(slotStoreCpp);
ok(countMatch && defOps.length === Number(countMatch[1]) && defOps.every(Boolean), 'read the default program out of slot_store.cpp (' + defOps.length + ' ops)');
const fromPreset = compile(PRESETS[0].expr);
// slot_store.cpp writes "x*0.3", the page compiles "0.3*x": same maths, different op order. So compare what
// the two programs compute, over a sweep of inputs, rather than the op lists.
let sameValues = true;
for (let k = 0; k < 60; k++) {
  const r = n => ((k * 7919 + n * 104729) % 1000) / 1000;
  const bands = [0, 1, 2, 3, 4, 5, 6, 7].map(r);
  const c = { x: k / 59, t: k * 1.37, i: 0, bands, level: r(9), bass: r(10), mid: r(11), treb: r(12) };
  if (Math.abs(evalCode(defOps, c) - evalCode(fromPreset, c)) > 1e-5) sameValues = false;
}
ok(sameValues, 'slot_store.cpp default program computes the same as presets.js "' + PRESETS[0].name + '" (' + PRESETS[0].expr + ')');
if (JSON.stringify(defOps) !== JSON.stringify(fromPreset))
  console.log('NOTE: slot 0 in slot_store.cpp and the page compile of the same formula use a different op order, so\n' +
              '      their fingerprints differ and the page uploads slot 0 once on first load. Same results.');

// ---- program fingerprint: slotHash() in index.html vs programHash() in program_hash.h
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hash-'));
try {
  const hashSrc = '#include <stdint.h>\n#include <string.h>\n#include <stdio.h>\n#include "pattern/expr_vm.cpp"\n#include "pattern/program_hash.h"\n' +
    'int main() { char line[8192];\n' +
    '  while (fgets(line, sizeof line, stdin)) { ExprProgram p{}; int n = 0, pos = 0, cnt = 0;\n' +
    '    if (sscanf(line, "%d%n", &cnt, &n) != 1) return 2; pos += n;\n' +
    '    for (int i = 0; i < cnt; i++) { int op; double a; if (sscanf(line + pos, "%d %lf%n", &op, &a, &n) != 2) return 2; pos += n;\n' +
    '      p.ops[p.count++] = ExprOpcode{(uint8_t)op, (float)a}; }\n' +
    '    printf("%08x\\n", (unsigned)programHash(p)); } return 0; }\n';
  fs.writeFileSync(path.join(dir, 'hash.cpp'), hashSrc);
  const exe = path.join(dir, process.platform === 'win32' ? 'hash.exe' : 'hash');
  const b = buildCpp([path.join(dir, 'hash.cpp')], exe, path.join(root, 'src'));
  if (!b.ok) { ok(false, 'could not build the programHash helper: ' + b.message); }
  else {
    const slotHash = new Function(extractFunction(indexHtml, 'slotHash') + '; return slotHash;')();
    const programs = PRESETS.map(p => compile(p.expr));
    programs.push(compile('1+2'), compile('-x'), compile('0.1+0.2'), compile('pow(2,0.5)*1e-9'), [[0, -0], [1]]);
    const input = programs.map(c => c.length + ' ' + c.map(([op, a]) => op + ' ' + (a === undefined ? 0 : a)).join(' ')).join('\n') + '\n';
    const r = spawnSync(exe, [], { input, encoding: 'utf8' });
    const got = r.stdout.trim().split(/\r?\n/);
    ok(got.length === programs.length, 'hash helper printed ' + got.length + ' fingerprints for ' + programs.length + ' programs');
    programs.forEach((c, k) => ok(got[k] === slotHash(c), 'fingerprint of program ' + k + ': firmware ' + got[k] + ', page ' + slotHash(c)));
  }
} finally { try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) { /* temp cleanup */ } }

// ---- UDP audio packet: the sender (server/server.js) against the receiver (src/audio/audio_packet.h)
{
  const serverJs = fs.readFileSync(path.join(root, 'server', 'server.js'), 'utf8');
  const packetH = fs.readFileSync(path.join(root, 'src', 'audio', 'audio_packet.h'), 'utf8');

  // Receiver: field order and sizes from the packed struct.
  const body = /struct __attribute__\(\(packed\)\) AudioPayload \{([\s\S]*?)\};/.exec(packetH);
  ok(!!body, 'found struct AudioPayload in audio_packet.h');
  const recv = {}; let offset = 0;
  if (body) for (const m of body[1].matchAll(/float\s+(\w+)(?:\[(\d+)\])?\s*;/g)) { recv[m[1]] = offset; offset += 4 * (m[2] ? Number(m[2]) : 1); }
  const bandCount = body ? Number((/float\s+bands\[(\d+)\]/.exec(body[1]) || [])[1]) : 0;
  const recvSize = offset;

  // Sender: the buffer size and every writeFloatLE offset.
  const alloc = Number((/Buffer\.alloc\((\d+)\)/.exec(serverJs) || [])[1]);
  const bandLoop = /for \(let i = 0; i < (\d+); i\+\+\) \{\s*buf\.writeFloatLE\(audioState\.bands\[i\], i \* 4\);/.exec(serverJs);
  const sent = {}; for (const m of serverJs.matchAll(/buf\.writeFloatLE\(audioState\.(level|bass|mid|treb),\s*(\d+)\)/g)) sent[m[1]] = Number(m[2]);

  ok(alloc === recvSize, 'packet size: server.js sends ' + alloc + ' bytes, the firmware struct is ' + recvSize);
  ok(!!bandLoop && Number(bandLoop[1]) === bandCount && recv.bands === 0, 'bands: server.js writes ' + (bandLoop ? bandLoop[1] : '?') + ' floats from offset 0, the struct has ' + bandCount + ' at ' + recv.bands);
  for (const name of ['level', 'bass', 'mid', 'treb']) ok(sent[name] === recv[name], name + ' offset: server.js ' + sent[name] + ', firmware ' + recv[name]);

  // Ports and the discovery beacon text, which both sides spell out.
  const num = (text, re) => Number((re.exec(text) || [])[1]);
  ok(num(serverJs, /const UDP_PORT = Number\(process\.env\.VIZ_UDP_PORT\) \|\| (\d+);/) === num(networkCpp, /const uint16_t UDP_PORT = (\d+);/), 'UDP audio port: server.js and network.cpp agree');
  ok(num(serverJs, /const BEACON_PORT = Number\(process\.env\.VIZ_BEACON_PORT\) \|\| (\d+);/) === num(networkCpp, /const uint16_t BEACON_PORT = (\d+);/), 'beacon port: server.js and network.cpp agree');
  const beaconMain = (/beaconUdp\.write\(\(const uint8_t\*\)"(\w+)", (\d+)\)/.exec(networkCpp) || []);
  const beaconServer = (/msg\.toString\(\) !== '(\w+)'/.exec(serverJs) || [])[1];
  ok(beaconMain[1] === beaconServer && Number(beaconMain[2]) === beaconMain[1].length, 'beacon text: network.cpp sends "' + beaconMain[1] + '", server.js listens for "' + beaconServer + '"');
}

console.log('firmware sync: ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
