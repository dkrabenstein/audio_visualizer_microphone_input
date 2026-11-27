// presets.js -- the built-in microphone patterns, uploaded to slots 0-6
// whenever the page loads. Slot 0 is 'audio_reactive', the firmware's own default
// (same formula as in src/pattern/slot_store.cpp), so re-sending it changes nothing.
//
// These are the tested equations from README.md.
//
// The firmware turns each expression's result into a HUE only (fract(v) ->
// CHSV hue, fixed saturation and brightness), so every pattern here is a
// different way of mapping sound -> colour:
//   FastLED hue scale (see fastled.js): 0.00 red, 0.125 orange, 0.25 yellow,
//   0.375 green, 0.50 aqua, 0.625 blue, 0.75 purple, 0.875 pink
//
// Audio inputs are all 0..1: level, bass, mid, treb and band(0..7).
//
// Keep this file identical in data/www/ and server/public/.

const PRESETS = [
  {
    // The firmware's default: a slowly drifting rainbow along each strip whose colours
    // swing with the music (bass shifts the hue, overall loudness shifts it further).
    name: 'audio_reactive',
    expr: '0.3*x + 0.03*t + bass + 1.5*level'
  },
  {
    // Spectral centroid: bass-heavy sound is red, treble-heavy sound moves
    // around the wheel. One colour for the whole strip.
    name: 'dominant_frequency',
    expr: '7 * (0.07*bass + 0.43*mid + 0.86*treb) / (bass + mid + treb)'
  },
  {
    // Rainbow along the strip, with each region's colour shifted by the
    // matching part of the spectrum (bass at x=0, treble at x=1, squared so
    // the bass gets more of the strip).
    name: 'spectrum_hue_shift',
    expr: '0.745*x + 3*(bass*sat(1-2*x^2) + mid*sat(1-abs(2*x^2-1)) + treb*sat(2*x^2-1))'
  },
  {
    // Level line: a bar that grows from x=0 with bass. Inside the bar the
    // colour sweeps along x; outside it stays red.
    name: 'level_line',
    expr: '1 + sat((sat(sqrt(level*bass ^ 3)*2) - x)*160) * (-0.33 - .33*x - .2*bass)'
  },
  {
    // Level line, second colour scheme.
    name: 'level_line_2',
    expr: '.99 + 1.1 * sat((sat(sqrt(level*bass ^ 3)*2) - x)*160) * (-.24 - .2*x - .2*bass)'
  },
  {
    // Position along the strip = frequency. Four bands are blended with
    // triangular weights, and each spot's colour changes as its band gets louder.
    name: 'spectrum_bars',
    expr: '0.5 - 1.3*sat(1.5*(band(0)*sat(1-abs(x*3-0)) + band(2)*sat(1-abs(x*3-1)) + band(5)*sat(1-abs(x*3-2)) + band(7)*sat(1-abs(x*3-3))))'
  },
  {
    // Kick drum bloom: the middle of each strip changes colour when bass
    // and level are both high, while the ends keep the base colour.
    name: 'bass_bloom',
    expr: '.55 + 12*sat(bass*level*.5)*tri(x)'
  }
];

// The seven presets occupy slots 0..6 (slot 0 = the firmware default, re-sent as-is).
// The Custom tab always uploads to, and runs from, slot 7 -- it has no slot picker.
const PRESET_FIRST_SLOT = 0;
const ADVANCED_SLOT = 7;
// The Basic tab (single colour) uploads to, and runs from, slot 8.
const BASIC_SLOT = 8;

// White light (Basic tab). The LEDs have no white chip, so a white is a red/green/blue mix
// chosen by colour temperature (Tanner Helland's blackbody approximation). Keep whiteRGB() in
// step with kelvinToRGB() in src/led/light_state.cpp, so the page previews exactly what the lights show.
const WHITE_MIN_K = 2000;   // deep warm (candle-like)
const WHITE_MAX_K = 6500;   // cool daylight
function whiteRGB(kelvin) {
  const t = Math.min(WHITE_MAX_K, Math.max(WHITE_MIN_K, kelvin)) / 100;
  let r, g, b;
  if (t <= 66) { r = 255; g = 99.4708025861 * Math.log(t) - 161.1195681661; }
  else { r = 329.698727446 * Math.pow(t - 60, -0.1332047592); g = 288.1221695283 * Math.pow(t - 60, -0.0755148492); }
  if (t >= 66) b = 255; else if (t <= 19) b = 0; else b = 138.5177312231 * Math.log(t - 10) - 305.0447927307;
  // Display only (the firmware is unchanged): from 4500 K up, ease red and green down so the cool
  // end of the swatch and slider track gets progressively bluer, closer to how the lights look.
  let f = Math.min(1, Math.max(0, (t * 100 - 4500) / (WHITE_MAX_K - 4500)));
  f = f * f * (3 - 2 * f);
  r *= 1 - 0.55 * f; g *= 1 - 0.25 * f;
  const c = (v) => Math.round(Math.min(255, Math.max(0, v)));
  return [c(r), c(g), c(b)];
}
function whiteName(kelvin) { return kelvin < 3500 ? 'Warm white' : (kelvin < 5000 ? 'White' : 'Cool white'); }
