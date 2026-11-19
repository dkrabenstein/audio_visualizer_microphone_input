// presets.js -- the built-in microphone patterns, uploaded to slots 1-6
// whenever the page loads (slot 0 is the firmware's own default:
// 0.3*x + 0.03*t + bass + 1.5*level).
//
// These are the tested equations from README.md.
//
// The firmware turns each expression's result into a HUE only (fract(v) ->
// CHSV hue, fixed saturation and brightness), so every pattern here is a
// different way of mapping sound -> colour:
//   hue 0.00 red, 0.16 yellow, 0.33 green, 0.50 cyan, 0.66 blue, 0.83 magenta
//
// Audio inputs are all 0..1: level, bass, mid, treb and band(0..7).
//
// Keep this file identical in data/www/ and server/public/.

const PRESETS = [
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

const PRESET_FIRST_SLOT = 1;
