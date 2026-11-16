// presets.js -- the five built-in microphone patterns, uploaded to slots 1-5
// whenever the page loads (slot 0 is the firmware's own default).
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
    // Classic level meter along each strip. The bar grows from x=0 with
    // loudness; inside the bar the colour runs green -> red toward the tip,
    // outside it is blue. sat(...*40) turns "x < level" into a sharp edge.
    name: 'vu_meter',
    expr: '0.66 + sat((sat(level*3) - x)*40) * (-0.33 - 0.33*x)'
  },
  {
    // Position along the strip = frequency (bass at x=0, treble at x=1).
    // Four bands are blended with triangular weights, and each spot's colour
    // goes blue -> red as its frequency gets louder: a colour equaliser.
    name: 'spectrum_bars',
    expr: '0.66 - 0.66*sat(1.5*(' +
          'band(0)*sat(1-abs(x*3-0)) + ' +
          'band(2)*sat(1-abs(x*3-1)) + ' +
          'band(5)*sat(1-abs(x*3-2)) + ' +
          'band(7)*sat(1-abs(x*3-3))))'
  },
  {
    // Kick drum bloom: the strip is teal until bass hits, then the middle of
    // every strip flushes toward red/pink while the ends stay teal.
    name: 'bass_bloom',
    expr: '0.5 + 0.45*sat(bass*2.5)*tri(x)'
  },
  {
    // Gold base colour. Treble (hats, sibilance) adds a fast ripple that
    // sweeps along the strip and swings the colour from red to green.
    name: 'treble_shimmer',
    expr: '0.15 + sat(treb*3)*0.25*sin(tau*x*12 - t*14)'
  },
  {
    // Each physical strip listens to something different: strip 0 = bass,
    // 1 = mids, 2 = treble, 3 = overall level. idx i picks the feature via
    // sat(1-abs(i-n)), which is 1 for strip n and 0 for the others.
    name: 'strip_per_band',
    expr: '0.66 - 0.6*sat(2*(' +
          'bass*sat(1-abs(i-0)) + ' +
          'mid*sat(1-abs(i-1)) + ' +
          'treb*sat(1-abs(i-2)) + ' +
          'level*sat(1-abs(i-3)))) + x*0.06'
  }
];

const PRESET_FIRST_SLOT = 1;
