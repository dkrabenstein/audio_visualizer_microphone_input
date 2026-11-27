// Differential test: the browser preview (compiler.js evalCode) must give the same numbers as the
// firmware's VM (src/expr_vm.cpp). Each expression is compiled by compiler.js, then evaluated twice:
// once in JS, once by test/helpers/emit_vm.cpp (built with g++ and fed the same bytecode).
// A mismatch means the preview shows something the lights will not do, or the opcode tables differ.
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { compile, evalCode } = require('./load_compiler.js');
const { buildCpp } = require('./build_cpp.js');

const root = path.join(__dirname, '..', '..');
let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('FAIL:', m); } };

// ---- build the helper
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'emit_vm-'));
const out = path.join(dir, process.platform === 'win32' ? 'emit_vm.exe' : 'emit_vm');
const build = buildCpp([path.join(root, 'test', 'helpers', 'emit_vm.cpp')], out, path.join(root, 'src'));
if (!build.ok) {
  console.log('could not build emit_vm.cpp with g++:', build.message);
  process.exit(1);
}

// ---- cases
const expressions = [
  '0.5+0.5*sin(tau*x+t)', 'fract(x*8+t*0.5)', 'band(0)*2+treb', 'tri(x+t)', 'sat(bass*3-1)',
  '1+2', '2*3+4', '2+3*4', '(2+3)*4', '10-3-2', '2^3^2', '-3+5', '--4', '-2^2', '7 % 3',
  'x', 't*2', 'i', 'level', 'bass', 'mid', 'treb', 'bass*mid-treb',
  'sin(pi/2)', 'cos(pi)', 'cos(x*tau)', 'sin(t*3)*cos(x*7)',
  'band(0)+band(1)+band(2)+band(3)+band(4)+band(5)+band(6)+band(7)',
  'min(3,7)', 'max(3,7)', 'min(x,0.5)', 'max(x,0.5)', 'pow(2,10)', 'pow(x,2)', 'pow(-2,0.5)', 'pow(-2,3)',
  'fract(3.75)', 'fract(-0.25)', 'floor(-2.5)', 'floor(x*10)', 'abs(-5)', 'abs(x-0.5)',
  'sqrt(16)', 'sqrt(-4)', 'sqrt(x)', 'sat(2)', 'sat(-1)', 'sat(x*3)',
  'tri(0.25)', 'tri(0.75)', 'tri(-0.25)', 'tri(t*0.3)', '1/0', '5%0', '1/x', 'x%0.3',
  '1e30*1e30', 'sin(1e30*1e30)', 'sin(1e10)', 'pow(10,100)',
];
const base = { x: 0.37, t: 2.5, i: 1, level: 0.6, bass: 0.8, mid: 0.4, treb: 0.2,
               bands: [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8] };
const contexts = [];
for (let k = 0; k < 7; k++) {
  contexts.push({ ...base, x: k / 6, t: 0.7 * k + 0.13, i: k % 4, level: k / 6,
                  bass: (6 - k) / 6, mid: (k % 3) / 2, treb: ((k * 5) % 7) / 7,
                  bands: base.bands.map((b, j) => (b + 0.11 * k + 0.05 * j) % 1) });
}

const progs = [];
for (const src of expressions) {
  let code;
  try { code = compile(src); } catch (e) { ok(false, src + ' did not compile: ' + e.message); continue; }
  for (const ctx of contexts) progs.push({ src, code, ctx });
}

const lines = progs.map(({ code, ctx }) => {
  const nums = [ctx.x, ctx.t, ctx.i, ctx.level, ctx.bass, ctx.mid, ctx.treb, ...ctx.bands];
  const ops = code.map(([op, arg]) => op + ' ' + (arg === undefined ? 0 : arg)).join(' ');
  return nums.map(n => n.toPrecision(17)).join(' ') + ' ' + code.length + ' ' + ops;
});

const run = spawnSync(out, [], { input: lines.join('\n') + '\n', encoding: 'utf8' });
ok(run.status === 0, 'emit_vm exited with status ' + run.status);
const results = (run.stdout || '').trim().split(/\r?\n/);
ok(results.length === progs.length, 'emit_vm printed ' + results.length + ' results for ' + progs.length + ' programs');

for (let k = 0; k < progs.length && k < results.length; k++) {
  const { src, code, ctx } = progs[k];
  const want = evalCode(code, ctx);
  if (results[k].startsWith('INVALID')) { ok(false, src + ': firmware rejected compiler output (' + results[k] + ')'); continue; }
  const got = parseFloat(results[k]);
  const trig = /sin|cos/.test(src);
  const tol = (trig ? 5e-5 : 2e-5) * Math.max(1, Math.abs(want));
  ok(Math.abs(got - want) <= tol,
     src + ' @x=' + ctx.x.toFixed(3) + ' t=' + ctx.t.toFixed(3) + ': firmware ' + got + ', preview ' + want);
}

try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) { /* temp dir cleanup only */ }
console.log('differential: ' + pass + ' passed, ' + fail + ' failed (' + progs.length + ' programs)');
process.exit(fail ? 1 : 0);
