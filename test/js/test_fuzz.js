// Random-program test: thousands of generated expressions, checked three ways.
//
//   reference   the expression tree evaluated directly in double precision (knows nothing of bytecode)
//   preview     compiler.js compile() + evalCode(), what the browser shows
//   firmware    the same bytecode run by the real VM (src/expr_vm.cpp, built with g++)
//
// reference vs preview catches compiler mistakes (operand order, wrong opcode, precedence); preview vs
// firmware catches the two evaluators drifting apart. Cases where the answer is ill-conditioned (a rounding
// difference of 1e-7 could legitimately change it, e.g. next to a floor() step or inside sin(1e9*x)) are
// detected by nudging the inputs, and skipped. Also fuzzes the compiler with garbage text.
//
//   FUZZ_SEED=123 FUZZ_N=5000 node test/js/test_fuzz.js     (defaults: seed 20240611, 2500 expressions)
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { compile, evalCode, CompileError, OP, sSin, sCos } = require('./load_compiler.js');
const { buildCpp } = require('./build_cpp.js');
const { suite } = require('./testkit.js');

const root = path.join(__dirname, '..', '..');
const t = suite('fuzz');
const SEED = Number(process.env.FUZZ_SEED) || 20240611;
const N = Number(process.env.FUZZ_N) || 2500;
const CONTEXTS = 5;

// ---- seeded random numbers (mulberry32), so a failure can be reproduced with FUZZ_SEED
let state = SEED >>> 0;
function rnd() { state = (state + 0x6D2B79F5) >>> 0; let z = state; z = Math.imul(z ^ (z >>> 15), z | 1); z ^= z + Math.imul(z ^ (z >>> 7), z | 61); return ((z ^ (z >>> 14)) >>> 0) / 4294967296; }
const pick = a => a[Math.floor(rnd() * a.length)];

// ---- expression trees
const NUMBERS = ['0', '1', '2', '3', '0.5', '0.25', '10', '100', '255', '0.001', '3.14159', '7', '1e3', '1e-3', '1e6', '1e-6', '1e10'];
const VARS = ['x', 't', 'i', 'level', 'bass', 'mid', 'treb'];
const CONSTS = { pi: Math.PI, tau: 2 * Math.PI, e: Math.E };
const FN1 = ['sin', 'cos', 'abs', 'floor', 'fract', 'sqrt', 'sat', 'tri'];
const FN2 = ['min', 'max', 'pow'];
const BIN = ['+', '-', '*', '/', '%', '^'];

function gen(depth) {
  if (depth <= 0 || rnd() < 0.22) {
    const r = rnd();
    if (r < 0.40) return { k: 'num', text: rnd() < 0.8 ? pick(NUMBERS) : (rnd() * 20).toFixed(3) };
    if (r < 0.80) return { k: 'var', n: pick(VARS) };
    if (r < 0.90) return { k: 'band', i: Math.floor(rnd() * 8) };
    return { k: 'const', n: pick(Object.keys(CONSTS)) };
  }
  const r = rnd();
  if (r < 0.50) return { k: 'bin', op: pick(BIN), a: gen(depth - 1), b: gen(depth - 1) };
  if (r < 0.62) return { k: 'neg', a: gen(depth - 1) };
  if (r < 0.88) return { k: 'fn1', n: pick(FN1), a: gen(depth - 1) };
  return { k: 'fn2', n: pick(FN2), a: gen(depth - 1), b: gen(depth - 1) };
}
// Every binary operation is parenthesised, so the text has exactly one meaning whatever the precedence rules are.
function print(n) {
  switch (n.k) {
    case 'num': return n.text;
    case 'var': case 'const': return n.n;
    case 'band': return 'band(' + n.i + ')';
    case 'neg': return '(-' + print(n.a) + ')';
    case 'bin': return '(' + print(n.a) + ' ' + n.op + ' ' + print(n.b) + ')';
    case 'fn1': return n.n + '(' + print(n.a) + ')';
    case 'fn2': return n.n + '(' + print(n.a) + ', ' + print(n.b) + ')';
  }
}

// Reference value in double precision, with the same guards the firmware documents. Constants and inputs are
// rounded to float32 first, because that is how the board holds them. The answer is only a fair yardstick
// when float32 can follow it, so the result says why not otherwise:
//   nonFinite  an intermediate value is NaN, infinite or beyond float32's range. The browser (JavaScript
//              Math.pow(1, Infinity) is NaN) and the board (C powf(1, inf) is 1) differ there.
//   fuzzy      float32 cannot keep what the next step needs: a value above 2000 (no fractional part left for
//              fract/floor/%), a huge angle for sin/cos, a subtraction that cancels most of its value, or a step function (floor, fract, tri, %, the
//              divide-by-zero and negative-power guards) whose input sits within 1e-5 of its step.
function reference(tree, c) {
  let fuzzy = false, nonFinite = false, bigAngle = false;
  const big = v => { if (!Number.isFinite(v) || Math.abs(v) > 1e30) nonFinite = true; else if (Math.abs(v) > 2000 || (v !== 0 && Math.abs(v) < 1e-30)) fuzzy = true; return v; };   // tiny non-zero values underflow to 0 in float32
  const nearInt = v => { if (Math.abs(v - Math.round(v)) < 1e-5 * Math.max(1, Math.abs(v))) fuzzy = true; return v; };
  const trig = v => { if (Math.abs(v) > 1e4) { fuzzy = true; bigAngle = true; } };     // float32 cannot place a huge angle on the sine table
  const cancel = (r, a, b) => { if (r !== 0 && Math.abs(r) < 1e-3 * Math.max(Math.abs(a), Math.abs(b))) fuzzy = true; return r; };
  const fract = v => v - Math.floor(nearInt(v));
  function ev(n) {
    switch (n.k) {
      case 'num': return big(Math.fround(Number(n.text)));
      case 'var': return Math.fround(n.n === 'i' ? c.i : c[n.n]);
      case 'const': return Math.fround(CONSTS[n.n]);
      case 'band': return Math.fround(c.bands[n.i]);
      case 'neg': return big(-ev(n.a));
      case 'bin': {
        const a = ev(n.a), b = ev(n.b);
        switch (n.op) {
          // Cancellation: when most of the value cancels, float32's rounding of the operands becomes the answer.
          case '+': return big(cancel(a + b, a, b));
          case '-': return big(cancel(a - b, a, b));
          case '*': return big(a * b);
          case '/': if (b !== 0 && Math.abs(b) < 1e-6) fuzzy = true; return big(b !== 0 ? a / b : 0);
          // fmod is exact, so the error comes from its inputs: |a| * 1.2e-7 in absolute terms, and a small modulus
          // makes the result's range small relative to that.
          case '%': if (b !== 0) { nearInt(a / b); if (Math.abs(b) < 1e-2 || Math.abs(a) > 800) fuzzy = true; } return big(b !== 0 ? a % b : 0);
          case '^': if (a < 0 && !Number.isInteger(b)) { nearInt(b); return 0; } return big(Math.pow(a, b));
        }
      }
      case 'fn1': {
        const v = ev(n.a);
        switch (n.n) {
          // The sine table's own accuracy (about 1e-5) is tested in test_vm and test_compiler; using the same
          // table here keeps that small error from being magnified by %, ^ and fract into false alarms.
          case 'sin': trig(v); return sSin(v);
          case 'cos': trig(v); return sCos(v);
          case 'abs': return big(Math.abs(v));
          case 'floor': return big(Math.floor(nearInt(v)));
          case 'fract': return big(fract(v));
          case 'sqrt': return big(v > 0 ? Math.sqrt(v) : 0);
          case 'sat': return big(Math.min(1, Math.max(0, v)));
          case 'tri': { const f = fract(v); return big(f < 0.5 ? f * 2 : 2 - f * 2); }
        }
      }
      case 'fn2': {
        const a = ev(n.a), b = ev(n.b);
        switch (n.n) {
          case 'min': return big(Math.min(a, b));
          case 'max': return big(Math.max(a, b));
          case 'pow': if (a < 0 && !Number.isInteger(b)) { nearInt(b); return 0; } return big(Math.pow(a, b));
        }
      }
    }
  }
  const v = ev(tree);
  return { v, fuzzy, bigAngle, nonFinite: nonFinite || !Number.isFinite(v) };
}

function makeContext() {
  const bands = Array.from({ length: 8 }, () => rnd());
  return { x: rnd(), t: rnd() < 0.3 ? 0 : rnd() * 3600, i: Math.floor(rnd() * 4), bands,
           level: rnd(), bass: rnd(), mid: rnd(), treb: rnd() };
}
// A variant of the program and its inputs with every input and every constant changed by a random relative
// amount of up to 1e-6 (about 16 times float32's own rounding noise). A well-conditioned program barely
// moves; one that is ill-conditioned (a step function, a huge angle, a division by nearly zero) jumps.
function jiggle(code, c) {
  const f = v => v * (1 + (rnd() * 2 - 1) * 1e-6) + (rnd() * 2 - 1) * 1e-7;
  return { code: code.map(([op, arg]) => (op === OP.CONST ? [op, f(arg)] : arg === undefined ? [op] : [op, arg])),
           ctx: { x: f(c.x), t: f(c.t), i: f(c.i), bands: c.bands.map(f), level: f(c.level), bass: f(c.bass), mid: f(c.mid), treb: f(c.treb) } };
}

// ---- generate
const cases = [];            // { text, tree, code, ctxs }
const rejected = {};
let generated = 0;
for (let n = 0; n < N; n++) {
  const tree = gen(2 + Math.floor(rnd() * 5));
  const text = print(tree);
  generated++;
  let code;
  try { code = compile(text); }
  catch (e) {
    if (!(e instanceof CompileError)) { t.ok(false, 'compile() threw a non-CompileError on ' + text + ': ' + e); continue; }
    rejected[e.message.replace(/\d+/g, 'N')] = (rejected[e.message.replace(/\d+/g, 'N')] || 0) + 1;
    t.ok(/too (many|long|deep)|nests too deeply|exceed|limit|op/i.test(e.message), 'a valid expression was rejected for a reason other than size: ' + e.message + '  <= ' + text);
    continue;
  }
  cases.push({ text, tree, code, ctxs: Array.from({ length: CONTEXTS }, makeContext) });
}

// ---- garbage text: the compiler must answer with a program or a CompileError, never anything else
const PIECES = ['x', 't', 'i', 'level', 'bass', 'mid', 'treb', 'pi', 'tau', 'e', 'band(', 'band(3)', 'band(9)', 'sin(', 'cos(', 'min(', 'pow(', 'sat(', ')', '(', ',',
  '+', '-', '*', '/', '%', '^', '1', '2.5', '1e3', '1e', '..', '.', ' ', '@', 'abc', '--', '0', '1e999'];
const garbage = [];
for (let n = 0; n < 3000; n++) {
  let s = ''; const len = 1 + Math.floor(rnd() * 12);
  for (let k = 0; k < len; k++) s += pick(PIECES);
  let code = null;
  try { code = compile(s); }
  catch (e) { if (!(e instanceof CompileError)) t.ok(false, 'compile() threw a non-CompileError on ' + JSON.stringify(s) + ': ' + e); continue; }
  garbage.push({ text: s, code });
}

// ---- run everything through the firmware VM
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fuzz-'));
const exe = path.join(dir, process.platform === 'win32' ? 'emit_vm.exe' : 'emit_vm');
const built = buildCpp([path.join(root, 'test', 'helpers', 'emit_vm.cpp')], exe, path.join(root, 'src'));
if (!built.ok) { console.log('could not build emit_vm.cpp:', built.message); process.exit(1); }

const line = (code, c) => [c.x, c.t, c.i, c.level, c.bass, c.mid, c.treb, ...c.bands].map(v => Number(v).toPrecision(17)).join(' ') +
  ' ' + code.length + ' ' + code.map(([op, arg]) => op + ' ' + (arg === undefined ? 0 : arg)).join(' ');
const jobs = [];
for (const c of cases) c.ctxs.forEach((ctx, k) => jobs.push({ code: c.code, ctx, owner: c, k }));
const zero = { x: 0, t: 0, i: 0, level: 0, bass: 0, mid: 0, treb: 0, bands: Array(8).fill(0) };
for (const g of garbage) jobs.push({ code: g.code, ctx: zero, owner: g, k: 0, garbage: true });

const run = spawnSync(exe, [], { input: jobs.map(j => line(j.code, j.ctx)).join('\n') + '\n', encoding: 'utf8', maxBuffer: 1 << 26 });
const out = (run.stdout || '').trim().split(/\r?\n/);
t.ok(run.status === 0, 'firmware helper exited with ' + run.status);
t.eq(out.length, jobs.length, 'firmware helper answered every program');

// ---- compare
let stable = 0, skippedUnstable = 0, skippedNonFinite = 0, refChecked = 0, infDisagree = 0;
const infExamples = [];
const mismatches = [];
jobs.forEach((j, n) => {
  const fw = out[n];
  if (fw === undefined) return;
  if (fw.startsWith('INVALID')) { mismatches.push(`${j.owner.text}: the firmware rejected what the compiler produced (${fw})`); return; }
  const got = parseFloat(fw);
  if (!Number.isFinite(got)) { mismatches.push(`${j.owner.text}: the firmware returned ${fw}`); return; }
  if (j.garbage) { t.ok(Number.isFinite(evalCode(j.code, j.ctx)), 'preview of ' + j.owner.text + ' is finite'); return; }

  const ref = reference(j.owner.tree, j.ctx);
  if (ref.nonFinite) {                                      // JavaScript and C differ on infinity / NaN arithmetic
    skippedNonFinite++;
    const pv = evalCode(j.code, j.ctx);
    if (Math.abs(got - pv) > 1e-4 * Math.max(1, Math.abs(pv))) { infDisagree++; if (infExamples.length < 3 && !infExamples.some(e => e.startsWith(j.owner.text))) infExamples.push(`${j.owner.text}: preview ${pv}, firmware ${got}`); }
    return;
  }
  const preview = evalCode(j.code, j.ctx);
  const scale = Math.max(1, Math.abs(preview));
  const refTol = 2e-4 * scale;
  let wellConditioned = true;
  for (let k = 0; k < 4 && wellConditioned; k++) {
    const v = jiggle(j.code, j.ctx);
    if (!(Math.abs(evalCode(v.code, v.ctx) - preview) <= 2 * refTol)) wellConditioned = false;
  }
  if (!wellConditioned) { skippedUnstable++; return; }     // ill-conditioned: a rounding difference could change the answer
  stable++;
  // preview vs firmware: both are float32 models of the same program
  const fwTol = 1e-4 * scale;
  if (Math.abs(got - preview) > fwTol) mismatches.push(`${j.owner.text} @x=${j.ctx.x.toFixed(3)} t=${j.ctx.t.toFixed(1)}: preview ${preview}, firmware ${got}`);
  // reference vs preview: the maths itself
  if (ref.fuzzy) return;
  refChecked++;
  if (Math.abs(preview - ref.v) > refTol) mismatches.push(`${j.owner.text} @x=${j.ctx.x.toFixed(3)} t=${j.ctx.t.toFixed(1)}: reference ${ref.v}, preview ${preview}`);
});
try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) { /* temp cleanup */ }

t.ok(mismatches.length === 0, mismatches.length + ' mismatches, first few (FUZZ_SEED=' + SEED + '):\n    ' + mismatches.slice(0, 8).join('\n    '));
const total = cases.length * CONTEXTS;
t.ok(cases.length >= N * 0.5, 'enough of the generated expressions fit the limits (' + cases.length + ' of ' + generated + ')');
t.ok(stable >= total * 0.5, 'enough cases were well-conditioned to be compared (' + stable + ' of ' + total + ')');
t.ok(refChecked >= total * 0.3, 'enough cases were checked against the double-precision reference (' + refChecked + ')');
console.log(`  seed ${SEED}: ${generated} expressions (${cases.length} compiled, ${generated - cases.length} over the size limits), ${total} evaluations:` +
  ` ${stable} compared, ${skippedUnstable} skipped as ill-conditioned, ${skippedNonFinite} skipped for infinity/NaN, ${refChecked} also checked against double precision; ` +
  `${garbage.length} garbage strings compiled and ran in the firmware`);
if (infDisagree) console.log(`  NOTE: of the ${skippedNonFinite} evaluations with an infinite or NaN intermediate value, the preview and the board show different results in ${infDisagree} ` +
  `(JavaScript's Math.pow(1, Infinity) is NaN, C's powf(1, inf) is 1). Examples:\n    ` + infExamples.join('\n    '));
t.done();
