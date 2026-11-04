const {compile, evalCode, CompileError} = require('../web/compiler.js');
let pass=0, fail=0;
function ok(c,m){ if(c){pass++;} else {fail++; console.log("FAIL:",m);} }

const ctx = {x:0.37, t:2.5, i:1, level:0.6, bass:0.8, mid:0.4, treb:0.2,
             bands:[0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8]};

const cases = [
  ["1+2", () => 3],
  ["2*3+4", () => 10],
  ["2+3*4", () => 14],
  ["(2+3)*4", () => 20],
  ["10-3-2", () => 5],
  ["2^3^2", () => Math.pow(2,Math.pow(3,2))],
  ["-3+5", () => 2],
  ["--4", () => 4],
  ["-2^2", () => -Math.pow(2,2)],
  ["x", () => ctx.x],
  ["t*2", () => ctx.t*2],
  ["sin(0)", () => 0],
  ["sin(pi/2)", () => 1],
  ["0.5+0.5*sin(tau*x+t)", () => 0.5+0.5*Math.sin(2*Math.PI*ctx.x+ctx.t)],
  ["band(0)+band(7)", () => ctx.bands[0]+ctx.bands[7]],
  ["min(3,7)", () => 3],
  ["max(3,7)", () => 7],
  ["pow(2,10)", () => 1024],
  ["fract(3.75)", () => 0.75],
  ["floor(-2.5)", () => -3],
  ["abs(-5)", () => 5],
  ["sqrt(16)", () => 4],
  ["sqrt(-4)", () => 0],
  ["1/0", () => 0],
  ["5%0", () => 0],
  ["sat(2)", () => 1],
  ["sat(-1)", () => 0],
  ["tri(0.25)", () => 0.5],
  ["tri(0.75)", () => 0.5],
  ["bass*2", () => ctx.bass*2],
  ["i", () => ctx.i],
  ["7 % 3", () => 1],
  ["fract(x*8+t*0.5)", () => {const v=ctx.x*8+ctx.t*0.5; return v-Math.floor(v);}],
];
// Expressions containing sin/cos are compared against idealised Math.sin
// with a looser bound: evalCode deliberately models the firmware's 512-entry
// float32 sine LUT, whose interpolation error is ~2e-5. Tightening this
// tolerance would be asserting that the preview is WRONG about the hardware.
for (const [src, ref] of cases) {
  let got;
  try { got = evalCode(compile(src), ctx); }
  catch(e){ ok(false, src+" threw "+e.message); continue; }
  const want = ref();
  const trig = /sin|cos/.test(src);
  const tol = trig ? 3e-5 : 1e-6;
  ok(Math.abs(got-want) < tol, src+" => "+got+", expected "+want);
}

const bad = ["1+", "(1+2", "1+2)", "sin()", "sin(1,2)", "min(1)", "nope(1)",
  "zzz", "band(8)", "band(-1)", "band(x)", "1 2", "*3", "1..2", "@"];
for (const src of bad) {
  let threw=false;
  try { compile(src); } catch(e){ threw = e instanceof CompileError; }
  ok(threw, '"'+src+'" should have been rejected');
}

let deep = "1"; for(let k=0;k<20;k++) deep = "sin("+deep+")";
ok(compile(deep).length===21, "deep unary chain compiles");
let wide = "1"; for(let k=0;k<20;k++) wide = wide+"+1";
ok(compile(wide).length===41, "wide chain compiles");
let huge = "1"; for(let k=0;k<40;k++) huge = huge+"+1";
let hugeThrew=false; try{compile(huge);}catch(e){hugeThrew=true;}
ok(hugeThrew, "over-length expression rejected");

console.log("compiler: "+pass+" passed, "+fail+" failed");
process.exit(fail?1:0);
