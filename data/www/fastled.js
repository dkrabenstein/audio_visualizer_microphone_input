// fastled.js -- turns an expression result into the colour the LED strips show.
//
// The firmware does CHSV(h, 240, 255) and FastLED converts that with
// hsv2rgb_rainbow(), whose hue scale is NOT the usual HSV one:
//   hue  0 red, 32 orange, 64 yellow, 96 green, 128 aqua, 160 blue,
//        192 purple, 224 pink, back to red at 256.
// This is a port of that function (FastLED src/hsv2rgb.cpp, Y1 level, no
// green scaling) so every preview on the page matches the real strip.
//
// Keep this file identical in data/www/ and server/public/.

function rainbowRGB(hue) {
  const scale8 = (i, sc) => (i * (1 + sc)) >> 8;
  const offset8 = (hue & 0x1f) << 3;
  const third = scale8(offset8, 85), two = scale8(offset8, 170);
  let r, g, b;
  if (!(hue & 0x80)) {
    if (!(hue & 0x40)) {
      if (!(hue & 0x20)) { r = 255 - third; g = third; b = 0; }          // red -> orange
      else               { r = 171; g = 85 + third; b = 0; }             // orange -> yellow
    } else {
      if (!(hue & 0x20)) { r = 171 - two; g = 170 + third; b = 0; }      // yellow -> green
      else               { r = 0; g = 255 - third; b = third; }          // green -> aqua
    }
  } else {
    if (!(hue & 0x40)) {
      if (!(hue & 0x20)) { r = 0; g = 171 - two; b = 85 + two; }         // aqua -> blue
      else               { r = third; g = 0; b = 255 - third; }          // blue -> purple
    } else {
      if (!(hue & 0x20)) { r = 85 + third; g = 0; b = 171 - third; }     // purple -> pink
      else               { r = 170 + third; g = 0; b = 85 - third; }     // pink -> red
    }
  }
  // saturation 240, as the firmware uses (FastLED scales down and adds a floor of 1)
  r = Math.min(255, scale8(r, 254) + 1);
  g = Math.min(255, scale8(g, 254) + 1);
  b = Math.min(255, scale8(b, 254) + 1);
  return [r, g, b];
}

// Expression result -> CSS colour, the way the firmware does it:
// fract(v), then (uint8)(fraction * 255), so only hues 0..254 can occur.
function fastledCss(v) {
  const f = v - Math.floor(v);
  const hue = Math.min(254, Math.trunc(Math.fround(f * 255)));
  const c = rainbowRGB(hue);
  return 'rgb(' + c[0] + ',' + c[1] + ',' + c[2] + ')';
}
