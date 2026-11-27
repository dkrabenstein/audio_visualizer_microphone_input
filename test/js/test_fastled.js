// Tests for data/www/fastled.js: the hue -> colour conversion the page uses to preview the strips.
const { loadScript } = require('./load_web.js');

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('FAIL:', m); } };
const { rainbowRGB, fastledCss } = loadScript('fastled.js', ['rainbowRGB', 'fastledCss']);
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

// Anchor colours of FastLED's "rainbow" hue scale (0 red, 32 orange, 64 yellow, 96 green, 128 aqua,
// 160 blue, 192 purple, 224 pink). The +1 floor and the 240 saturation come from the firmware's CHSV.
ok(same(rainbowRGB(0),   [255, 1, 1]), 'hue 0 is red: ' + rainbowRGB(0));
ok(same(rainbowRGB(96),  [1, 255, 1]), 'hue 96 is green: ' + rainbowRGB(96));
ok(same(rainbowRGB(160), [1, 1, 255]), 'hue 160 is blue: ' + rainbowRGB(160));
const y = rainbowRGB(64);
ok(y[0] > 100 && y[1] > 100 && Math.abs(y[0] - y[1]) <= 5 && y[2] <= 2, 'hue 64 is yellow (r about equal to g, b low): ' + y);
const o = rainbowRGB(32);
ok(o[0] > o[1] && o[1] > 50 && o[2] <= 2, 'hue 32 is orange (r > g > 50, b low): ' + o);

let valid = true, smooth = true, prev = null;
for (let h = 0; h < 256; h++) {
  const c = rainbowRGB(h);
  if (c.length !== 3 || !c.every(v => Number.isInteger(v) && v >= 0 && v <= 255)) valid = false;
  if (prev && c.some((v, k) => Math.abs(v - prev[k]) > 6)) smooth = false;
  prev = c;
}
ok(valid, 'every hue 0..255 gives three integers 0..255');
ok(smooth, 'neighbouring hues differ by at most 6 per channel (no jumps in the wheel)');
const wrap = rainbowRGB(255), start = rainbowRGB(0);
ok(wrap.every((v, k) => Math.abs(v - start[k]) <= 6), 'the wheel wraps: hue 255 is next to hue 0');

// fastledCss: fract(v) -> hue 0..254 -> css
ok(fastledCss(0) === 'rgb(255,1,1)', 'fastledCss(0) is red: ' + fastledCss(0));
ok(fastledCss(1) === fastledCss(0) && fastledCss(7) === fastledCss(0), 'whole numbers all land on the same colour (fract)');
ok(fastledCss(-0.25) === fastledCss(0.75), 'negative values wrap like the firmware (fract)');
ok(fastledCss(0.3) === fastledCss(2.3) && fastledCss(0.3) === fastledCss(-0.7), 'adding or subtracting whole numbers changes nothing');
const hue254 = rainbowRGB(254);
const top = fastledCss(0.99999999);
ok(top === 'rgb(' + hue254.join(',') + ')', 'the top of the range maps to hue 254, never 255: ' + top);
ok(fastledCss(0.375) === 'rgb(' + rainbowRGB(95).join(',') + ')', 'the README hue 0.375 (green) maps to hue 95');
ok(/^rgb\(\d{1,3},\d{1,3},\d{1,3}\)$/.test(fastledCss(0.5)), 'output is a CSS rgb() string');
let allCss = true;
for (let v = -3; v <= 3; v += 0.013) if (!/^rgb\(\d{1,3},\d{1,3},\d{1,3}\)$/.test(fastledCss(v))) allCss = false;
ok(allCss, 'a sweep of values -3..3 always gives a valid colour');

console.log('fastled: ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
