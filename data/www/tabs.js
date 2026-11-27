// tabs.js -- tab switching and the Patterns, Basic (colour wheel, white light) and Home tabs.
// Loaded after core.js, which it relies on (espUrl, $, ...).

// --- tabs -------------------------------------------------------------
(function () {
  const tabs = ['home', 'basic', 'patterns', 'custom', 'setup'];
  function showTab(name) {
    if (name === 'advanced') name = 'custom';   // old bookmarks
    if (!tabs.includes(name)) name = 'home';
    tabs.forEach(function (t) {
      document.getElementById('tab-' + t).classList.toggle('active', t === name);
      document.getElementById('btn-' + t).classList.toggle('active', t === name);
    });
    try { history.replaceState(null, '', '#' + name); } catch (e) {}
  }
  document.querySelectorAll('.tabbar button').forEach(function (b) {
    b.onclick = function () { showTab(b.dataset.tab); };
  });
  document.querySelectorAll('[data-go]').forEach(function (b) {
    b.onclick = function () { showTab(b.dataset.go); };
  });
  showTab(location.hash.slice(1) || 'home');
})();

// --- patterns tab -----------------------------------------------------------
(function () {
  // Friendly text for the presets (keyed by preset name in presets.js).
  var INFO = {
    audio_reactive: ['Audio Reactive',
      'The default pattern: a slowly drifting rainbow along the strip whose colours swing with the music. Bass and overall loudness both shift the colours.'],
    dominant_frequency: ['Dominant Frequency',
      'One colour for the whole strip that follows the pitch: deep sounds are red, high sounds move round the colour wheel.'],
    spectrum_hue_shift: ['Spectrum Hue Shift',
      'A rainbow along the strip. Bass, mid and treble each push the colour in their own part of the strip.'],
    level_line: ['Level Line',
      'A bar that grows from the start of the strip with the bass. Blue-to-green inside the bar, red outside.'],
    level_line_2: ['Level Line 2',
      'The same bar with a different colour scheme.'],
    spectrum_bars: ['Spectrum Bars',
      'Position along the strip is pitch. Each region changes colour as its part of the spectrum gets louder.'],
    bass_bloom: ['Bass Bloom',
      'Kick drums make the middle of the strip bloom into a new colour while the ends stay put.']
  };

  // A fixed, representative moment of music so every image is comparable.
  var bands = [0.8, 0.6, 0.5, 0.4, 0.3, 0.35, 0.25, 0.15];
  var sample = {
    t: 3, i: 0, bands: bands,
    bass: (bands[0] + bands[1]) / 2,
    mid: (bands[2] + bands[3] + bands[4]) / 3,
    treb: (bands[5] + bands[6] + bands[7]) / 3,
    level: bands.reduce(function (a, b) { return a + b; }) / bands.length
  };

  function paint(canvas, expr) {
    var cx = canvas.getContext('2d'), N = 120, w = canvas.width / N;
    cx.fillStyle = '#000'; cx.fillRect(0, 0, canvas.width, canvas.height);
    var code;
    try { code = compile(expr); } catch (e) { return; }
    for (var p = 0; p < N; p++) {
      sample.x = p / (N - 1);
      var v = evalCode(code, sample);
      v = v - Math.floor(v);
      cx.fillStyle = fastledCss(v);
      cx.fillRect(p * w, 0, w + 1, canvas.height);
    }
  }

  function status(ok, msg) {
    var el = document.getElementById('pat-status');
    el.className = ok ? 'ok' : 'err';
    el.textContent = msg;
  }

  function markActive(slot) {
    document.querySelectorAll('.card').forEach(function (c) {
      c.classList.toggle('active', +c.dataset.slot === slot);
    });
  }

  async function choose(n) {
    var slot = PRESET_FIRST_SLOT + n;
    var title = (INFO[PRESETS[n].name] || [PRESETS[n].name])[0];
    // The laptop page talks to the ESP32 directly (espUrl); the board's own page uses its own address.
    var base = (typeof espUrl === 'function') ? espUrl() : '';
    // One select attempt; resolves to { ok, error } instead of throwing on an HTTP error.
    async function trySelect() {
      var r = await fetch(base + '/api/select', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'slot=' + slot });
      var j = {};
      try { j = await r.json(); } catch (e) {}
      return { ok: r.ok, error: j.error || r.status };
    }
    try {
      var res = await trySelect();
      // Presets live in the board's RAM, so a reboot or reflash empties them while this page
      // stays open. The board then rejects the slot as invalid: re-upload the presets, retry once.
      if (!res.ok && /invalid slot/i.test(String(res.error)) && typeof uploadPresets === 'function') {
        status(true, 'loading patterns onto the lights\u2026');
        await uploadPresets();
        res = await trySelect();
      }
      if (!res.ok) throw new Error(res.error);
      markActive(slot);
      status(true, '\u2713 now showing: ' + title);
    } catch (e) {
      status(false, '\u2717 could not switch pattern: ' + e.message);
    }
  }

  var box = document.getElementById('cards');
  PRESETS.forEach(function (p, n) {
    var info = INFO[p.name] || [p.name, ''];
    var card = document.createElement('div');
    card.className = 'card';
    card.dataset.slot = PRESET_FIRST_SLOT + n;
    var cv = document.createElement('canvas');
    cv.width = 360; cv.height = 64;
    var title = document.createElement('div');
    title.className = 'title'; title.textContent = info[0];
    var blurb = document.createElement('div');
    blurb.className = 'blurb'; blurb.textContent = info[1];
    card.appendChild(cv); card.appendChild(title); card.appendChild(blurb);
    card.onclick = function () { choose(n); };
    box.appendChild(card);
    paint(cv, p.expr);
  });

  // Highlight the pattern that is running whenever the device state refreshes.
  document.addEventListener('devstate', function (e) { markActive(e.detail.active); });

})();

// --- basic tab --------------------------------------------------------------
(function () {
  var cv = document.getElementById('wheel');
  var size = cv.width, c = size / 2, outer = c, inner = c * 0.68;
  var cx = cv.getContext('2d');
  var img = cx.createImageData(size, size);
  for (var py = 0; py < size; py++) {
    for (var px = 0; px < size; px++) {
      var dx = px + 0.5 - c, dy = py + 0.5 - c;
      var d = Math.sqrt(dx * dx + dy * dy);
      var k = (py * size + px) * 4;
      if (d > outer || d < inner) { img.data[k + 3] = 0; continue; }
      var deg = (Math.atan2(dy, dx) * 180 / Math.PI + 90 + 360) % 360;  // 0 at top, clockwise
      var rgb = rainbowRGB(Math.min(254, Math.floor(deg / 360 * 255)));
      img.data[k] = rgb[0]; img.data[k + 1] = rgb[1]; img.data[k + 2] = rgb[2];
      // soften the two edges of the ring
      img.data[k + 3] = Math.round(255 * Math.min(1, outer - d, d - inner));
    }
  }
  cx.putImageData(img, 0, 0);

  var marker = document.getElementById('wheel-marker');
  var swatch = document.getElementById('wheel-swatch');
  var statusEl = document.getElementById('basic-status');

  function status(cls, msg) { statusEl.className = cls; statusEl.textContent = msg; }

  function show(h) {
    var rgb = rainbowRGB(h);
    var col = 'rgb(' + rgb[0] + ',' + rgb[1] + ',' + rgb[2] + ')';
    swatch.style.background = col;
    marker.style.background = col;
    var css = cv.getBoundingClientRect().width || 280;
    var rad = css / 2 * (1 + 0.68) / 2;                       // middle of the ring
    var ang = ((h + 0.5) / 255 * 360 - 90) * Math.PI / 180;
    marker.style.left = (css / 2 + rad * Math.cos(ang)) + 'px';
    marker.style.top = (css / 2 + rad * Math.sin(ang)) + 'px';
    marker.style.display = 'block';
  }

  // Send "constant hue" to its own slot and switch to it. Only the newest
  // colour matters while dragging, so requests never pile up.
  var busy = false, pending = null;
  async function apply(h) {
    if (busy) { pending = h; return; }
    busy = true;
    // The board does (uint8)(fract(v) * 255), so it can show hues 0..254; the +0.5
    // keeps float rounding from landing one step low.
    var expr = String((h + 0.5) / 255);
    var base = (typeof espUrl === 'function') ? espUrl() : '';
    try {
      var r = await fetch(base + '/api/pattern', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(toPayload('basic', BASIC_SLOT, compile(expr)))
      });
      if (!r.ok) throw new Error((await r.json()).error || r.status);
      var r2 = await fetch(base + '/api/select', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'slot=' + BASIC_SLOT });
      if (!r2.ok) throw new Error((await r2.json()).error || r2.status);
      status('ok', '\u2713 lights set to this colour');
    } catch (e) {
      status('err', '\u2717 could not reach the lights: ' + e.message);
    }
    busy = false;
    if (pending !== null) { var n = pending; pending = null; apply(n); }
  }

  function hueAt(ev) {
    var rect = cv.getBoundingClientRect();
    var dx = ev.clientX - rect.left - rect.width / 2;
    var dy = ev.clientY - rect.top - rect.height / 2;
    var d = Math.sqrt(dx * dx + dy * dy) / (rect.width / 2);
    if (d < 0.68 - 0.04 || d > 1.04) return null;             // not on the ring
    var deg = (Math.atan2(dy, dx) * 180 / Math.PI + 90 + 360) % 360;
    return Math.min(254, Math.floor(deg / 360 * 255));
  }

  var dragging = false, last = -1;
  function pick(ev) {
    var h = hueAt(ev);
    if (h === null || h === last) return;
    last = h; show(h); apply(h);
    window.basicHue = h;
    try { localStorage.setItem('viz.basicHue', String(h)); } catch (err) {}
  }
  cv.addEventListener('pointerdown', function (ev) {
    if (hueAt(ev) === null) return;
    dragging = true; cv.setPointerCapture(ev.pointerId); pick(ev);
  });
  cv.addEventListener('pointermove', function (ev) { if (dragging) pick(ev); });
  cv.addEventListener('pointerup', function () { dragging = false; });
  cv.addEventListener('pointercancel', function () { dragging = false; });

  // Brightness: the control is here, the lights don't use it yet.
  var bri = document.getElementById('bri'), briVal = document.getElementById('bri-val');
  bri.addEventListener('input', function () { briVal.textContent = bri.value + '%'; });
})();

// --- home page --------------------------------------------------------------
(function () {
  var el = function (id) { return document.getElementById(id); };
  var home = el('tab-home');
  var isHome = function () { return home.classList.contains('active') && !document.hidden; };

  function title(name) {
    return name.replace(/_/g, ' ').replace(/\b\w/g, function (c) { return c.toUpperCase(); });
  }

  // ---- compiled expressions for the little previews ----
  var presetCode = PRESETS.map(function (p) { try { return compile(p.expr); } catch (e) { return null; } });
  var defaultCode = compile('0.3*x + 0.03*t + bass + 1.5*level');
  if (typeof window.basicHue !== 'number') {
    try {
      var saved = parseInt(localStorage.getItem('viz.basicHue'), 10);
      if (saved >= 0 && saved <= 254) window.basicHue = saved;
    } catch (e) {}
  }

  // ---- audio feed (for the meter and so previews react to real sound) ----
  var audio = null, live = false, fetching = false;
  function synth(t) {            // gentle fake audio when nothing is reachable
    var b = [];
    for (var k = 0; k < 8; k++) b.push(0.5 + 0.5 * Math.sin(t * (1.3 + k * 0.4) + k));
    return { bands: b, bass: (b[0] + b[1]) / 2, mid: (b[2] + b[3] + b[4]) / 3,
             treb: (b[5] + b[6] + b[7]) / 3,
             level: b.reduce(function (a, c) { return a + c; }) / 8 };
  }
  async function getJson(url) {
    var ctl = new AbortController();
    var to = setTimeout(function () { ctl.abort(); }, 1200);
    try {
      var r = await fetch(url, { signal: ctl.signal });
      if (!r.ok) throw new Error(r.status);
      return await r.json();
    } finally { clearTimeout(to); }
  }
  async function fetchAudio() {
    if (fetching || !isHome()) return;
    fetching = true;
    var onLaptop = (typeof espUrl === 'function');
    var base = onLaptop ? espUrl() : '';
    try {
      audio = await getJson(base + '/api/audio');
      live = true;
      el('meter-src').textContent = '';   // normal case: no subtitle (problems are still reported below)
    } catch (e) {
      live = false;
      var done = false;
      if (onLaptop) {              // the PC server also streams simulated audio
        try {
          audio = await getJson('/api/audio');
          live = true; done = true;
          el('meter-src').textContent = 'simulated by the PC (lights not reachable)';
        } catch (e2) {}
      }
      if (!done) el('meter-src').textContent = 'no signal \u2014 lights not reachable';
    }
    fetching = false;
  }
  setInterval(fetchAudio, 150);

  // ---- device state (running pattern, online or not) ----
  var dev = { online: null, active: null, white: null };
  var polling = false;
  document.addEventListener('devstate', function (e) {
    dev.online = true; dev.active = e.detail.active; dev.white = e.detail.white || null; renderNow();
  });
  document.addEventListener('devoffline', function () { dev.online = false; renderNow(); });
  setInterval(function () {
    if (!isHome() || polling) return;
    polling = true;
    Promise.resolve(poll()).then(function () { polling = false; }, function () { polling = false; });
  }, 3000);

  var nowCard = el('nowcard');
  function presetIndex(slot) {
    var n = slot - PRESET_FIRST_SLOT;
    return (n >= 0 && n < PRESETS.length) ? n : -1;
  }
  function renderNow() {
    var name, sub, go = 'patterns', dot = '';
    if (dev.online === null) {
      name = 'checking the lights\u2026'; sub = '';
    } else if (!dev.online) {
      name = 'Lights not reachable'; dot = 'off';
      sub = 'check that the board is powered and on your network';
    } else {
      dot = 'on'; sub = '';
      var a = dev.active, n = presetIndex(a);
      if (dev.white) { name = whiteName(dev.white) + ' (' + dev.white + ' K)'; go = 'basic'; }
      else if (n >= 0) name = title(PRESETS[n].name);
      else if (a === BASIC_SLOT) { name = 'Single colour'; go = 'basic'; }
      else if (a === ADVANCED_SLOT) { name = 'Custom formula'; go = 'custom'; }
      else name = '';
    }
    el('now-name').textContent = name;
    el('now-sub').textContent = sub;
    el('now-dot').className = 'dot' + (dot ? ' ' + dot : '');
    nowCard.dataset.go = go;
  }
  renderNow();

  // ---- drawing helpers ----
  function paintStrip(cv, c, t, a) {
    var g = cv.getContext('2d'), n = 80, w = cv.width / n;
    g.fillStyle = '#000'; g.fillRect(0, 0, cv.width, cv.height);
    if (!c) return;
    var ctx = { t: t, i: 0, bands: a.bands, bass: a.bass, mid: a.mid, treb: a.treb, level: a.level };
    for (var p = 0; p < n; p++) {
      ctx.x = p / (n - 1);
      g.fillStyle = fastledCss(evalCode(c, ctx));
      g.fillRect(p * w, 0, w + 1, cv.height);
    }
  }
  function paintSolidRGB(cv, c) {
    var g = cv.getContext('2d');
    g.fillStyle = 'rgb(' + c[0] + ',' + c[1] + ',' + c[2] + ')';
    g.fillRect(0, 0, cv.width, cv.height);
  }
  function paintSolid(cv, h) {
    var g = cv.getContext('2d'), c = rainbowRGB(h);
    g.fillStyle = 'rgb(' + c[0] + ',' + c[1] + ',' + c[2] + ')';
    g.fillRect(0, 0, cv.width, cv.height);
  }
  function drawBar(t) {
    var cv = el('home-bar'), g = cv.getContext('2d'), n = 150, w = cv.width / n;
    for (var p = 0; p < n; p++) {
      g.fillStyle = fastledCss(p / (n - 1) * 0.6 + t * 0.04);
      g.fillRect(p * w, 0, w + 1, cv.height);
    }
  }
  var shown = [0, 0, 0, 0, 0, 0, 0, 0];
  function drawMeter(a) {
    var cv = el('meter'), g = cv.getContext('2d'), W = cv.width, H = cv.height;
    g.fillStyle = '#000'; g.fillRect(0, 0, W, H);
    var n = 8, gap = 10, bw = (W - gap * (n + 1)) / n;
    for (var i = 0; i < n; i++) {
      var target = live ? Math.max(0, Math.min(1, a.bands[i] || 0)) : 0;
      shown[i] += (target - shown[i]) * (target > shown[i] ? 0.5 : 0.15);   // fast up, slow down
      var h = shown[i] * (H - 8);
      g.fillStyle = fastledCss(i / 9);
      g.fillRect(gap + i * (bw + gap), H - 4 - h, bw, Math.max(2, h));
    }
  }

  function nowCode() {
    if (!dev.online) return null;
    var a = dev.active, n = presetIndex(a);
    if (n >= 0) return presetCode[n];
    if (a === ADVANCED_SLOT) return (typeof code !== 'undefined') ? code : null;
    if (a === BASIC_SLOT || dev.white) return null;   // drawn as a solid colour
    return defaultCode;
  }

  function frame(now) {
    if (isHome()) {
      var t = now / 1000;
      var a = (live && audio) ? audio : synth(t);

      drawBar(t);

      var strip = el('now-strip'), hue = window.basicHue;
      if (dev.online && dev.white) paintSolidRGB(strip, whiteRGB(dev.white));
      else if (dev.online && dev.active === BASIC_SLOT && typeof hue === 'number') paintSolid(strip, hue);
      else paintStrip(strip, nowCode(), t, a);

      var sw = el('prev-basic');
      if (typeof hue === 'number') {
        var c = rainbowRGB(hue);
        sw.style.background = 'rgb(' + c[0] + ',' + c[1] + ',' + c[2] + ')';
        sw.style.borderStyle = 'solid';
      } else { sw.style.background = 'transparent'; sw.style.borderStyle = 'dashed'; }

      var pi = (dev.online && presetIndex(dev.active) >= 0)
        ? presetIndex(dev.active)
        : Math.floor(t / 3) % PRESETS.length;     // otherwise flip through them
      paintStrip(el('prev-patterns'), presetCode[pi], t, a);
      paintStrip(el('prev-custom'), (typeof code !== 'undefined') ? code : null, t, a);

      drawMeter(a);
    }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);

  // ---- quick controls (UI only for now) ----
  var switches = ['pwr', 'pwr-basic', 'pwr-pat', 'pwr-adv'];
  var lastPwrInput = 0, pwrBusy = false, pwrPending = null;

  function setSwitches(on) {
    switches.forEach(function (o) {
      el(o).setAttribute('aria-pressed', on ? 'true' : 'false');
      el(o).textContent = on ? 'Lights on' : 'Lights off';
    });
  }
  async function sendPower(on) {                  // latest value wins, like brightness
    if (pwrBusy) { pwrPending = on; return; }
    pwrBusy = true;
    var base = (typeof espUrl === 'function') ? espUrl() : '';
    var ctl = new AbortController();
    var to = setTimeout(function () { ctl.abort(); }, 1500);
    try {
      await fetch(base + '/api/power', {
        method: 'POST', signal: ctl.signal,
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'on=' + (on ? '1' : '0') });
    } catch (e) {}
    clearTimeout(to);
    pwrBusy = false;
    if (pwrPending !== null) { var n = pwrPending; pwrPending = null; sendPower(n); }
  }
  switches.forEach(function (id) {                // keep every on/off switch in step
    el(id).addEventListener('click', function () {
      var on = el(id).getAttribute('aria-pressed') !== 'true';
      lastPwrInput = Date.now();
      setSwitches(on);
      sendPower(on);
    });
  });
  // Show the board's real state (e.g. after a reload), unless the user just clicked.
  document.addEventListener('devstate', function (e) {
    if (typeof e.detail.on === 'boolean' && Date.now() - lastPwrInput > 2000) setSwitches(e.detail.on);
  });
  var sliders = ['bri', 'home-bri', 'pat-bri', 'adv-bri'];
  var lastBriInput = 0, briBusy = false, briPending = null;

  function setSliders(v) {
    sliders.forEach(function (o) { el(o).value = v; el(o + '-val').textContent = v + '%'; });
  }
  // Latest value wins: while a request is in flight only the newest value is kept.
  async function sendBrightness(v) {
    if (briBusy) { briPending = v; return; }
    briBusy = true;
    var base = (typeof espUrl === 'function') ? espUrl() : '';
    var ctl = new AbortController();
    var to = setTimeout(function () { ctl.abort(); }, 1500);
    try {
      await fetch(base + '/api/brightness', {
        method: 'POST', signal: ctl.signal,
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'value=' + v });
    } catch (e) {}
    clearTimeout(to);
    briBusy = false;
    if (briPending !== null) { var n = briPending; briPending = null; sendBrightness(n); }
  }
  sliders.forEach(function (id) {                 // keep every brightness slider in step
    el(id).addEventListener('input', function () {
      var v = el(id).value;
      lastBriInput = Date.now();
      setSliders(v);
      sendBrightness(v);
    });
  });
  // Show the board's real brightness (e.g. after a reload), unless the user is mid-change.
  document.addEventListener('devstate', function (e) {
    var b = e.detail.brightness;
    if (typeof b === 'number' && Date.now() - lastBriInput > 2000) setSliders(b);
  });
})();

// ---- white light (Basic tab): warm <-> cool slider ----
(function () {
  var slider = document.getElementById('white-temp');
  var swatch = document.getElementById('white-swatch');
  var readout = document.getElementById('white-readout');
  var status = document.getElementById('white-status');
  var lastInput = 0, busy = false, pending = null, active = false, lastSent = null;

  function base() { return (typeof espUrl === 'function') ? espUrl() : ''; }
  function say(msg, bad) { status.textContent = msg; status.style.color = bad ? 'var(--err)' : 'var(--ok)'; }

  // Track: warm -> cool, painted with the same colour function the lights use.
  var stops = [];
  for (var k = WHITE_MIN_K; k <= WHITE_MAX_K; k += 500) stops.push('rgb(' + whiteRGB(k).join(',') + ')');
  stops.push('rgb(' + whiteRGB(WHITE_MAX_K).join(',') + ')');
  slider.style.background = 'linear-gradient(to right,' + stops.join(',') + ')';

  // Show the colour for temperature k. `on` = the lights are really showing it.
  function show(k, on) {
    swatch.style.background = 'rgb(' + whiteRGB(k).join(',') + ')';
    swatch.style.opacity = on ? '1' : '0.35';
    slider.classList.toggle('off', !on);
    readout.textContent = on ? whiteName(k) + ' · ' + k + ' K'
                             : 'white is off — move or tap the slider to turn it on';
  }

  // Latest value wins: only the newest temperature is sent if the board is slow.
  async function send(k) {
    if (busy) { pending = k; return; }               // a request is in flight: remember only the newest value
    if (k === lastSent && active) return;            // the board already shows this
    lastSent = k;                                    // (set only when really sending)
    busy = true;
    var ctl = new AbortController(), to = setTimeout(function () { ctl.abort(); }, 3000);
    try {
      var r = await fetch(base() + '/api/white', { method: 'POST', signal: ctl.signal,
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'kelvin=' + k });
      var j = {}; try { j = await r.json(); } catch (e) {}
      if (r.status === 404) throw new Error('the board needs a firmware update for white');
      if (!r.ok) throw new Error(j.error || r.status);
      active = true;
      say('✓ lights set to ' + whiteName(k).toLowerCase() + ' (' + k + ' K)');
      if (typeof poll === 'function') poll();
    } catch (e) {
      active = false; lastSent = null;
      show(+slider.value, false);
      say('✗ could not set white: ' + (e.name === 'AbortError' ? 'the lights did not answer' : e.message), true);
    }
    clearTimeout(to);
    busy = false;
    if (pending !== null) { var n = pending; pending = null; send(n); }
  }

  slider.addEventListener('input', function () {
    lastInput = Date.now();
    var k = +slider.value;
    show(k, true);
    send(k);
  });
  // A plain tap on the thumb (no movement) should still turn the white on.
  slider.addEventListener('pointerup', function () { lastInput = Date.now(); if (!active) { show(+slider.value, true); send(+slider.value); } });
  slider.addEventListener('keyup', function () { if (!active) { show(+slider.value, true); send(+slider.value); } });

  // Keep the slider in step with what the board is really showing: choosing a colour, a pattern
  // or a custom formula switches the white off on the board.
  function sync(white) {
    if (Date.now() - lastInput < 2500 || busy) return;   // don't fight a drag in progress
    if (white) { active = true; lastSent = white; slider.value = white; show(+slider.value, true); }
    else {
      active = false; lastSent = null; show(+slider.value, false);
      if (status.textContent.indexOf('set to') > 0) status.textContent = '';
    }
  }
  document.addEventListener('devstate', function (e) { sync(e.detail.white || null); });
  async function refresh() {
    var tab = document.getElementById('tab-basic');
    if (!tab || !tab.classList.contains('active') || busy) return;
    var ctl = new AbortController(), to = setTimeout(function () { ctl.abort(); }, 1500);
    try { var j = await (await fetch(base() + '/api/slots', { signal: ctl.signal })).json(); sync(j.white || null); }
    catch (e) { /* offline: keep what is shown */ }
    clearTimeout(to);
  }
  setInterval(refresh, 3000);
  document.querySelectorAll('[data-tab="basic"], [data-go="basic"]').forEach(function (b) {
    b.addEventListener('click', function () { setTimeout(refresh, 0); });
  });
  show(+slider.value, false);
  setTimeout(refresh, 600);
})();
