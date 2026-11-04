// compiler.js -- expression -> postfix bytecode, entirely in the browser.
//
// The ESP32 never sees infix notation, never sees an identifier, and never
// parses anything but a flat array of [opcode, arg] pairs. Every syntax
// question is answered here, where a mistake costs a red underline instead
// of a watchdog reset.
//
// Opcode numbers MUST match firmware/expr_ops.h.

const ABI = 1;

const OP = {
  CONST: 0, X: 1, T: 2, IDX: 3, BAND: 4,
  LEVEL: 5, BASS: 6, MID: 7, TREB: 8,
  ADD: 16, SUB: 17, MUL: 18, DIV: 19, MOD: 20, POW: 21, MIN: 22, MAX: 23,
  NEG: 32, SIN: 33, COS: 34, ABS: 35, FLOOR: 36, FRACT: 37,
  SQRT: 38, SAT: 39, TRI: 40,
};

const VARS = {
  x: OP.X, t: OP.T, i: OP.IDX, level: OP.LEVEL,
  bass: OP.BASS, mid: OP.MID, treb: OP.TREB,
};

// name -> [opcode, arity]
const FUNCS = {
  sin: [OP.SIN, 1], cos: [OP.COS, 1], abs: [OP.ABS, 1],
  floor: [OP.FLOOR, 1], fract: [OP.FRACT, 1], sqrt: [OP.SQRT, 1],
  sat: [OP.SAT, 1], tri: [OP.TRI, 1],
  min: [OP.MIN, 2], max: [OP.MAX, 2], pow: [OP.POW, 2],
};

const CONSTS = { pi: Math.PI, tau: 2 * Math.PI, e: Math.E };

// prec, rightAssoc
const BINOPS = {
  '+': [1, false], '-': [1, false],
  '*': [2, false], '/': [2, false], '%': [2, false],
  '^': [3, true],
};

const MAX_OPS = 64;
const STACK_MAX = 16;

class CompileError extends Error {
  constructor(msg, pos) { super(msg); this.pos = pos; }
}

// --- tokenizer -------------------------------------------------------
function tokenize(src) {
  const toks = [];
  let i = 0;
  while (i < src.length) {
    const c = src[i];
    if (/\s/.test(c)) { i++; continue; }
    if (/[0-9.]/.test(c)) {
      let j = i;
      while (j < src.length && /[0-9.]/.test(src[j])) j++;
      // Scientific notation. Only consume "e" as an exponent marker when it
      // is actually followed by digits, so "2*e" still means 2 times Euler's
      // number while "2e3" means 2000. Without this lookahead the two
      // readings collide.
      if (j < src.length && /[eE]/.test(src[j])) {
        let k = j + 1;
        if (k < src.length && /[+-]/.test(src[k])) k++;
        if (k < src.length && /[0-9]/.test(src[k])) {
          while (k < src.length && /[0-9]/.test(src[k])) k++;
          j = k;
        }
      }
      const text = src.slice(i, j);
      if ((text.match(/\./g) || []).length > 1)
        throw new CompileError(`malformed number "${text}"`, i);
      const v = parseFloat(text);
      if (!isFinite(v)) throw new CompileError(`number out of range "${text}"`, i);
      toks.push({ k: 'num', v, pos: i });
      i = j; continue;
    }
    if (/[A-Za-z_]/.test(c)) {
      let j = i;
      while (j < src.length && /[A-Za-z0-9_]/.test(src[j])) j++;
      toks.push({ k: 'ident', v: src.slice(i, j), pos: i });
      i = j; continue;
    }
    if ('+-*/%^'.includes(c)) { toks.push({ k: 'op', v: c, pos: i++ }); continue; }
    if (c === '(') { toks.push({ k: 'lp', pos: i++ }); continue; }
    if (c === ')') { toks.push({ k: 'rp', pos: i++ }); continue; }
    if (c === ',') { toks.push({ k: 'comma', pos: i++ }); continue; }
    throw new CompileError(`unexpected character "${c}"`, i);
  }
  return toks;
}

// --- parser: precedence climbing, emits postfix directly -------------
function compile(src) {
  const toks = tokenize(src);
  const code = [];
  let p = 0;

  const peek = () => toks[p];
  const at = () => (toks[p] ? toks[p].pos : src.length);
  const emit = (op, arg) => {
    if (code.length >= MAX_OPS)
      throw new CompileError(`expression too complex (limit ${MAX_OPS} ops)`, at());
    code.push(arg === undefined ? [op] : [op, arg]);
  };

  function parseExpr(minPrec) {
    parseUnary();
    for (;;) {
      const tk = peek();
      if (!tk || tk.k !== 'op' || !BINOPS[tk.v]) break;
      const [prec, right] = BINOPS[tk.v];
      if (prec < minPrec) break;
      p++;
      parseExpr(right ? prec : prec + 1);
      emit({ '+': OP.ADD, '-': OP.SUB, '*': OP.MUL,
             '/': OP.DIV, '%': OP.MOD, '^': OP.POW }[tk.v]);
    }
  }

  // Unary minus sits between multiplicative and power precedence, matching
  // ordinary maths convention: -2^2 is -(2^2) = -4, but -2*3 is (-2)*3.
  // Parsing the operand at POWER_PREC lets "^" bind first while "*" does not.
  const POWER_PREC = BINOPS['^'][0];

  function parseUnary() {
    const tk = peek();
    if (tk && tk.k === 'op' && tk.v === '-') {
      p++; parseExpr(POWER_PREC); emit(OP.NEG); return;
    }
    if (tk && tk.k === 'op' && tk.v === '+') {
      p++; parseExpr(POWER_PREC); return;
    }
    parseAtom();
  }

  function parseAtom() {
    const tk = peek();
    if (!tk) throw new CompileError('unexpected end of expression', src.length);

    if (tk.k === 'num') { p++; emit(OP.CONST, tk.v); return; }

    if (tk.k === 'lp') {
      p++; parseExpr(0);
      if (!peek() || peek().k !== 'rp')
        throw new CompileError('missing ")"', at());
      p++; return;
    }

    if (tk.k === 'ident') {
      const name = tk.v;
      p++;

      // band(n) -- n must be an integer literal so the firmware can bounds
      // check it once at upload rather than on every pixel.
      if (name === 'band') {
        if (!peek() || peek().k !== 'lp')
          throw new CompileError('band requires "(index)"', at());
        p++;
        const arg = peek();
        if (!arg || arg.k !== 'num')
          throw new CompileError('band index must be a literal number', at());
        if (!Number.isInteger(arg.v) || arg.v < 0 || arg.v > 7)
          throw new CompileError('band index must be an integer 0..7', arg.pos);
        p++;
        if (!peek() || peek().k !== 'rp')
          throw new CompileError('missing ")" after band index', at());
        p++;
        emit(OP.BAND, arg.v);
        return;
      }

      if (FUNCS[name]) {
        const [opcode, arity] = FUNCS[name];
        if (!peek() || peek().k !== 'lp')
          throw new CompileError(`${name} requires arguments`, at());
        p++;
        let n = 0;
        if (peek() && peek().k !== 'rp') {
          for (;;) {
            parseExpr(0); n++;
            if (peek() && peek().k === 'comma') { p++; continue; }
            break;
          }
        }
        if (!peek() || peek().k !== 'rp')
          throw new CompileError(`missing ")" after ${name}(`, at());
        p++;
        if (n !== arity)
          throw new CompileError(`${name} takes ${arity} argument${arity > 1 ? 's' : ''}, got ${n}`, tk.pos);
        emit(opcode);
        return;
      }

      if (CONSTS[name] !== undefined) { emit(OP.CONST, CONSTS[name]); return; }
      if (VARS[name] !== undefined)   { emit(VARS[name]); return; }

      const known = [...Object.keys(VARS), ...Object.keys(FUNCS),
                     ...Object.keys(CONSTS), 'band'].sort().join(', ');
      throw new CompileError(`unknown name "${name}". known: ${known}`, tk.pos);
    }

    throw new CompileError('expected a value', tk.pos);
  }

  parseExpr(0);
  if (p < toks.length)
    throw new CompileError('trailing input after expression', toks[p].pos);

  // Run the same stack check the firmware runs. Catching it here means the
  // device should never see an invalid program -- and if it does, that is a
  // genuine bug rather than user error.
  verifyStack(code);
  return code;
}

function verifyStack(code) {
  let depth = 0;
  for (const [op] of code) {
    let pops;
    if (op <= OP.TREB) pops = 0;
    else if (op >= OP.ADD && op <= OP.MAX) pops = 2;
    else pops = 1;
    if (depth < pops) throw new CompileError('stack underflow (compiler bug)', 0);
    depth = depth - pops + 1;
    if (depth > STACK_MAX)
      throw new CompileError(`expression nests too deeply (limit ${STACK_MAX})`, 0);
  }
  if (depth !== 1)
    throw new CompileError('expression must produce exactly one value', 0);
  return depth;
}

// --- local preview evaluator ----------------------------------------
// Mirrors expr_eval() so the canvas preview shows what the device will show.
//
// Every value is passed through Math.fround(). JavaScript numbers are float64
// and the ESP32 stack is float32, and the difference is not cosmetic: pow(10,40)
// is an ordinary finite double but overflows to Infinity in float32. Without
// fround the preview would happily animate a pattern the strip cannot produce.
// Caught by differential testing against the compiled VM.
const f32 = Math.fround;

// The device does not call sinf(). It does a float32 range reduction into a
// 512-entry LUT with linear interpolation. Modelling that exactly here matters
// for more than the ~2e-5 interpolation error: for large arguments the float32
// reduction has no fractional bits left and collapses to zero, so sin(x*1e20)
// is flat on hardware while Math.sin would happily return a varying value.
// Simulating idealised sine would make the preview a different pattern, not
// just a slightly more accurate one.
const LUT_SIZE = 512;
const INV_2PI = f32(0.15915494309);
const sinLut = new Float32Array(LUT_SIZE + 1);
for (let i = 0; i <= LUT_SIZE; i++)
  sinLut[i] = f32(Math.sin(i * 2 * Math.PI / LUT_SIZE));

function sSin(rad) {
  if (!Number.isFinite(rad)) return 0;           // matches fastSin() guard
  let turns = f32(rad * INV_2PI);
  turns = f32(turns - Math.floor(turns));
  const fp = f32(turns * LUT_SIZE);
  let i = Math.trunc(fp);
  let frac = f32(fp - i);
  if (i < 0) { i = 0; frac = 0; }
  if (i >= LUT_SIZE) { i = LUT_SIZE - 1; frac = 1; }
  return f32(sinLut[i] + f32(f32(sinLut[i + 1] - sinLut[i]) * frac));
}
const sCos = rad => sSin(f32(rad + 1.57079632679));

function evalCode(code, ctx) {
  const st = [];
  for (const [op, arg] of code) {
    switch (op) {
      case OP.CONST: st.push(f32(arg)); break;
      case OP.X:     st.push(f32(ctx.x)); break;
      case OP.T:     st.push(f32(ctx.t)); break;
      case OP.IDX:   st.push(f32(ctx.i)); break;
      case OP.BAND:  st.push(f32(ctx.bands[arg])); break;
      case OP.LEVEL: st.push(f32(ctx.level)); break;
      case OP.BASS:  st.push(f32(ctx.bass)); break;
      case OP.MID:   st.push(f32(ctx.mid)); break;
      case OP.TREB:  st.push(f32(ctx.treb)); break;
      case OP.ADD: { const b = st.pop(); st.push(f32(st.pop() + b)); break; }
      case OP.SUB: { const b = st.pop(); st.push(f32(st.pop() - b)); break; }
      case OP.MUL: { const b = st.pop(); st.push(f32(st.pop() * b)); break; }
      case OP.DIV: { const b = st.pop(); const a = st.pop();
                     st.push(b !== 0 ? f32(a / b) : 0); break; }
      case OP.MOD: { const b = st.pop(); const a = st.pop();
                     st.push(b !== 0 ? f32(a % b) : 0); break; }
      case OP.POW: { const b = st.pop(); const a = st.pop();
                     st.push(a < 0 && !Number.isInteger(b) ? 0 : f32(Math.pow(a, b)));
                     break; }
      case OP.MIN: { const b = st.pop(); st.push(Math.min(st.pop(), b)); break; }
      case OP.MAX: { const b = st.pop(); st.push(Math.max(st.pop(), b)); break; }
      case OP.NEG:   st.push(f32(-st.pop())); break;
      case OP.SIN:   st.push(sSin(st.pop())); break;
      case OP.COS:   st.push(sCos(st.pop())); break;
      case OP.ABS:   st.push(f32(Math.abs(st.pop()))); break;
      case OP.FLOOR: st.push(f32(Math.floor(st.pop()))); break;
      case OP.FRACT: { const v = st.pop(); st.push(f32(v - Math.floor(v))); break; }
      case OP.SQRT:  { const v = st.pop(); st.push(v > 0 ? f32(Math.sqrt(v)) : 0); break; }
      case OP.SAT:   st.push(Math.min(1, Math.max(0, st.pop()))); break;
      case OP.TRI:   { const v0 = st.pop(); const v = f32(v0 - Math.floor(v0));
                       st.push(v < 0.5 ? f32(v * 2) : f32(2 - v * 2)); break; }
    }
  }
  const r = st[0];
  return Number.isFinite(r) ? r : 0;
}

function toPayload(name, slot, code) {
  return { abi: ABI, slot, name, code };
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { compile, evalCode, toPayload, tokenize,
                     CompileError, OP, ABI, MAX_OPS, STACK_MAX };
}
