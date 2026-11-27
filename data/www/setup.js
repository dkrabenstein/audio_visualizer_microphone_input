// setup.js -- the Setup tab: microphone tuning, LED strips, Wi-Fi, diagnostics and the live sound bars.
// Loaded after core.js, which it relies on (espUrl, ...).

// ---- microphone tuning (Setup tab) ----
(function () {
  var box = document.getElementById('tune-box');
  if (!box) return;
  var NAMES = ['47–94 Hz', '141–234 Hz', '234–422 Hz', '422–750 Hz',
               '750 Hz–1.4k', '1.4–2.8k', '2.8–6.1k', '6.1–14k'];
  var rows = document.getElementById('tune-rows');
  var fl = [], nowEl = [], valEl = [];
  for (var i = 0; i < 8; i++) {
    var r = document.createElement('div');
    r.className = 'rangeRow';
    r.innerHTML = '<span style="min-width:120px">band ' + i + ' <span style="color:var(--dim);font-size:12px">' +
      NAMES[i] + '</span></span><input type="range" min="-110" max="-40" step="0.5" value="-80">' +
      '<span style="min-width:64px;text-align:right"></span><span style="min-width:84px;text-align:right;font-size:12px"></span>';
    rows.appendChild(r);
    fl.push(r.children[1]); valEl.push(r.children[2]); nowEl.push(r.children[3]);
  }
  var margin = document.getElementById('tune-margin'), ceil = document.getElementById('tune-ceil'),
      smooth = document.getElementById('tune-smooth'), status = document.getElementById('tune-status');
  var lastEdit = 0, sending = false, pending = false, loaded = false;
  function base() { return (typeof espUrl === 'function') ? espUrl() : ''; }
  function say(m, bad) { status.textContent = m; status.style.color = bad ? 'var(--err)' : 'var(--dim)'; }
  function labels() {
    for (var i = 0; i < 8; i++) valEl[i].textContent = (+fl[i].value).toFixed(1) + ' dB';
    document.getElementById('tune-margin-val').textContent = margin.value + ' dB';
    document.getElementById('tune-ceil-val').textContent = ceil.value + ' dB';
    document.getElementById('tune-smooth-val').textContent = smooth.value + '%';
  }
  async function push() {                       // latest value wins
    if (sending) { pending = true; return; }
    sending = true;
    var body = { floors: fl.map(function (s) { return +s.value; }),
                 margin: +margin.value, minCeil: +ceil.value, smoothness: +smooth.value };
    try {
      var r = await fetch(base() + '/api/tuning', { method: 'POST',
        headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
      say(r.ok ? 'applied (not saved yet)' : 'board rejected the change', !r.ok);
    } catch (e) { say('lights not reachable', true); }
    sending = false;
    if (pending) { pending = false; push(); }
  }
  var timer = null;
  function edited() { lastEdit = Date.now(); labels(); clearTimeout(timer); timer = setTimeout(push, 120); }
  fl.concat([margin, ceil, smooth]).forEach(function (s) { s.addEventListener('input', edited); });

  async function post(path) { return fetch(base() + path, { method: 'POST' }); }
  document.getElementById('tune-cal').addEventListener('click', async function () {
    try { await post('/api/mic/calibrate'); say('calibrating… keep the room quiet for 3 seconds'); lastEdit = 0; loaded = false; }
    catch (e) { say('lights not reachable', true); }
  });
  document.getElementById('tune-save').addEventListener('click', async function () {
    try { var r = await post('/api/mic/save'); say(r.ok ? 'saved to the board' : 'save failed', !r.ok); }
    catch (e) { say('lights not reachable', true); }
  });
  document.getElementById('tune-reset').addEventListener('click', async function () {
    try { await post('/api/mic/reset'); loaded = false; say('defaults restored'); }
    catch (e) { say('lights not reachable', true); }
  });

  var busy = false;
  async function refresh() {
    if (busy) return;
    var tab = document.getElementById('tab-setup');
    if (document.hidden || (tab && !tab.classList.contains('active'))) return;
    var fold = box.closest('details');
    if (fold && !fold.open) return;     // collapsed
    busy = true;
    var ctl = new AbortController(), to = setTimeout(function () { ctl.abort(); }, 1200);
    try {
      var j = await (await fetch(base() + '/api/tuning', { signal: ctl.signal })).json();
      var editing = Date.now() - lastEdit < 2000;
      if ((!editing && !j.calibrating) || !loaded) {
        for (var i = 0; i < 8; i++) fl[i].value = j.floors[i];
        margin.value = j.margin; ceil.value = j.minCeil; smooth.value = j.smoothness;
        labels();
        loaded = !j.calibrating;
      }
      for (var k = 0; k < 8; k++) {
        var above = j.raw[k] - (+fl[k].value + +margin.value);
        nowEl[k].textContent = 'now ' + j.raw[k].toFixed(1);
        nowEl[k].style.color = above > 0 ? 'var(--ok)' : 'var(--dim)';
      }
      if (j.calibrating) say('calibrating… keep the room quiet');
      else if (status.textContent.indexOf('calibrating') === 0) say('calibrated — press Save to board to keep it');
    } catch (e) { /* offline: leave the last values */ }
    clearTimeout(to);
    busy = false;
  }
  setInterval(refresh, 400);
  // The tuning box is not currently inside a <details>, so only attach the listener if it is.
  var tuneFold = box.closest('details');
  if (tuneFold) tuneFold.addEventListener('toggle', function () { loaded = false; refresh(); });
  // Re-read the board's values whenever the Setup tab is opened.
  document.querySelectorAll('[data-tab="setup"], [data-go="setup"]').forEach(function (b) {
    b.addEventListener('click', function () { loaded = false; setTimeout(refresh, 0); });
  });
  labels();
})();

// ---- LED strips (Setup tab) ----
(function () {
  var rowsBox = document.getElementById('strip-rows');
  if (!rowsBox) return;
  var N = 4, max = 300;
  var cnt = [], rev = [];
  for (var i = 0; i < N; i++) {
    var r = document.createElement('div');
    r.className = 'rangeRow';
    r.style.marginBottom = '8px';
    r.innerHTML = '<span style="min-width:70px">strip ' + i + '</span>' +
      '<input type="number" min="1" max="300" step="1" value="100" aria-label="LEDs on strip ' + i + '" ' +
      'style="width:88px;background:#0b0c10;border:1px solid var(--line);color:var(--fg);padding:6px 8px;border-radius:5px;font:inherit">' +
      '<label style="display:flex;align-items:center;gap:6px;margin:0;text-transform:none;letter-spacing:0;font-size:12px;color:var(--fg);cursor:pointer">' +
      '<input type="checkbox" style="accent-color:var(--acc)"> reverse orientation</label>';
    rowsBox.appendChild(r);
    cnt.push(r.children[1]); rev.push(r.children[2].querySelector('input'));
  }
  var status = document.getElementById('strip-status');
  var lastEdit = 0, sending = false, pending = false, defaults = null;

  function base() { return (typeof espUrl === 'function') ? espUrl() : ''; }
  function say(m, bad) { status.textContent = m; status.style.color = bad ? 'var(--err)' : 'var(--dim)'; }

  function fill(j) {
    max = j.max || max; defaults = j.defaults;
    for (var i = 0; i < N; i++) {
      cnt[i].max = max;
      cnt[i].value = j.counts[i];
      rev[i].checked = !!j.reversed[i];
    }
  }

  async function push() {                       // latest value wins
    if (sending) { pending = true; return; }
    sending = true;
    var body = { counts: cnt.map(function (e) { return parseInt(e.value, 10); }),
                 reversed: rev.map(function (e) { return e.checked; }) };
    try {
      var r = await fetch(base() + '/api/strips', { method: 'POST',
        headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
      if (r.ok) say('applied (not saved yet)');
      else { var j = {}; try { j = await r.json(); } catch (e) {} say(j.error || 'board rejected the change', true); }
    } catch (e) { say('lights not reachable', true); }
    sending = false;
    if (pending) { pending = false; push(); }
  }

  function edited(input) {
    lastEdit = Date.now();
    if (input.type === 'number') {              // only send a sensible whole number
      var v = Number(input.value);
      if (!(v >= 1 && v <= max) || Math.floor(v) !== v) { say('enter a whole number from 1 to ' + max, true); return; }
    }
    push();
  }
  cnt.concat(rev).forEach(function (e) { e.addEventListener('change', function () { edited(e); }); });

  async function post(path) { return fetch(base() + path, { method: 'POST' }); }
  document.getElementById('strip-save').addEventListener('click', async function () {
    try { var r = await post('/api/leds/save'); say(r.ok ? 'saved to the board' : 'save failed', !r.ok); }
    catch (e) { say('lights not reachable', true); }
  });
  document.getElementById('strip-reset').addEventListener('click', async function () {
    try { await post('/api/leds/reset'); lastEdit = 0; say('defaults restored'); refresh(); }
    catch (e) { say('lights not reachable', true); }
  });

  var busy = false;
  async function refresh() {
    if (busy) return;
    var tab = document.getElementById('tab-setup');
    if (tab && !tab.classList.contains('active')) return;
    var fold = rowsBox.closest('details');
    if (fold && !fold.open) return;     // collapsed
    if (Date.now() - lastEdit < 3000 || sending) return;     // don't overwrite what is being typed
    busy = true;
    var ctl = new AbortController(), to = setTimeout(function () { ctl.abort(); }, 1200);
    try {
      var j = await (await fetch(base() + '/api/strips', { signal: ctl.signal })).json();
      if (cnt.indexOf(document.activeElement) < 0) fill(j);   // not while a count is being typed
    } catch (e) { /* offline: keep what is shown */ }
    clearTimeout(to);
    busy = false;
  }
  setInterval(refresh, 2000);
  rowsBox.closest('details').addEventListener('toggle', function () { lastEdit = 0; refresh(); });
  document.querySelectorAll('[data-tab="setup"], [data-go="setup"]').forEach(function (b) {
    b.addEventListener('click', function () { lastEdit = 0; setTimeout(refresh, 0); });
  });
  setTimeout(refresh, 500);
})();

// ---- Wi-Fi network (Setup tab) ----
// Talks to /api/wifi. If the board cannot join its network it opens its own "visualizer-esp-32-XXXX"
// access point and serves this same page, and this panel is where the network gets chosen.
(function () {
  var $ = function (id) { return document.getElementById(id); };
  var now = $('wifi-now'), status = $('wifi-status'), list = $('wifi-list'), ssid = $('wifi-ssid'),
      pass = $('wifi-pass'), fold = $('wifi-fold'), connectBtn = $('wifi-connect');
  function base() { return (typeof espUrl === 'function') ? espUrl() : ''; }
  function say(m, bad) { status.textContent = m; status.style.color = bad ? 'var(--err)' : 'var(--dim)'; }
  function esc(t) { var d = document.createElement('div'); d.textContent = t; return d.innerHTML; }
  var jumped = false, applying = false, lastAp = false;

  // Signal strength in words and colour: the one place the thresholds live. rssi is in dBm
  // (closer to 0 is stronger). The Diagnostics panel uses the same wording.
  window.vizSignal = function (rssi) {
    if (rssi >= -60) return { word: 'good', text: 'good connection', color: 'var(--ok)' };
    if (rssi >= -70) return { word: 'fair', text: 'fair connection', color: 'var(--warn)' };
    return { word: 'weak', text: 'weak connection', color: 'var(--err)' };
  };

  async function getJson(path, ms) {
    var ctl = new AbortController(), to = setTimeout(function () { ctl.abort(); }, ms || 2500);
    try { return await (await fetch(base() + path, { signal: ctl.signal })).json(); }
    finally { clearTimeout(to); }
  }

  function show(j) {
    var t;
    lastAp = !!j.ap;
    if (j.state === 'trying') t = 'Trying the new network&hellip;';
    else if (j.connected) t = 'Connected to <b>' + esc(j.ssid) + '</b> &middot; ' + esc(j.ip) +
        ' &middot; ' + (function (q) { return '<span style="white-space:nowrap;color:' + q.color + '">' + q.text + '</span>'; })(window.vizSignal(j.rssi)) +
        ' &middot; <b>' + esc(j.host) + '</b>';
    else if (j.ap) t = (j.ssid ? 'Cannot reach <b>' + esc(j.ssid) + '</b>. ' : 'No network saved. ') +
        'Setup mode: this board is hosting <b>' + esc(j.apSsid) + '</b>. Choose a network below.';
    else t = 'Connecting to <b>' + esc(j.ssid) + '</b>&hellip;';
    now.innerHTML = t;
    if (j.error && !applying) say(j.error, true);
    // First time we see setup mode: bring the panel to the front. The page is probably open on a
    // phone that just joined the board's own network.
    if (j.ap && !jumped) {
      jumped = true;
      try { $('btn-setup').click(); } catch (e) {}
      fold.open = true;
      scan();
    }
    if (!j.ap) jumped = false;
    connectBtn.disabled = j.state === 'trying';
  }

  async function refresh() {
    if (document.hidden) return;
    var tab = $('tab-setup');
    if (!applying && tab && !tab.classList.contains('active') && jumped) return;
    try { show(await getJson('/api/wifi')); }
    catch (e) { if (!applying) now.textContent = 'board not reachable'; }
  }

  var scanning = false;
  async function scan() {
    if (scanning) return;
    scanning = true; $('wifi-scan').disabled = true;
    list.innerHTML = '<option value="">scanning&hellip;</option>';
    try {
      for (var i = 0; i < 30; i++) {                       // up to ~30 s (the board retries interrupted scans)
        var j = await getJson('/api/wifi/scan', 4000);
        if (!j.scanning) {
          list.innerHTML = '<option value="">' + (j.networks.length ? 'choose a network (' + j.networks.length + ' found)' : 'none found') + '</option>';
          j.networks.forEach(function (n) {
            var o = document.createElement('option');
            o.value = n.ssid;
            var q = window.vizSignal(n.rssi);
            o.textContent = n.ssid + (n.secure ? '' : ' (open)') + ' · ' + q.text;
            o.style.color = q.color;   // desktop browsers colour the entries; some phone pickers ignore it
            list.appendChild(o);
          });
          scanning = false; $('wifi-scan').disabled = false;
          return;
        }
        await new Promise(function (r) { setTimeout(r, 1000); });
      }
      list.innerHTML = '<option value="">scan timed out</option>';
    } catch (e) { list.innerHTML = '<option value="">scan failed</option>'; }
    scanning = false; $('wifi-scan').disabled = false;
  }

  $('wifi-scan').addEventListener('click', scan);
  list.addEventListener('change', function () { if (list.value) { ssid.value = list.value; pass.focus(); } });
  $('wifi-show').addEventListener('change', function (e) { pass.type = e.target.checked ? 'text' : 'password'; });

  connectBtn.addEventListener('click', async function () {
    var name = ssid.value.trim();
    if (!name) { say('enter or choose a network name', true); return; }
    if (pass.value && (pass.value.length < 8 || pass.value.length > 63)) {
      say('a Wi-Fi password is 8 to 63 characters', true); return;
    }
    applying = true; connectBtn.disabled = true;
    say('Testing "' + name + '"...');
    try {
      var r = await fetch(base() + '/api/wifi', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                                   body: JSON.stringify({ ssid: name, password: pass.value }) });
      if (!r.ok) {
        var e = {}; try { e = await r.json(); } catch (x) {}
        say(e.error || 'board rejected it', true); applying = false; connectBtn.disabled = false; return;
      }
    } catch (e) { say('board not reachable', true); applying = false; connectBtn.disabled = false; return; }

    // Poll for the result. If the board joins the network, its setup AP closes and this page loses
    // the connection: that silence is the success case when we came in through the AP.
    var started = Date.now(), misses = 0, viaAp = lastAp;
    while (Date.now() - started < 40000) {
      await new Promise(function (r) { setTimeout(r, 1500); });
      var j = null;
      try { j = await getJson('/api/wifi', 2000); misses = 0; } catch (e) { misses++; }
      if (j) {
        show(j);
        if (j.state === 'connected') { say('Connected to ' + j.ssid + '. Open http://' + j.host + ' on that network.'); pass.value = ''; break; }
        if (j.state !== 'trying') { say(j.error || 'could not join that network', true); break; }
        say('Testing "' + name + '"...');
      } else if (misses >= 3 && viaAp) {
        say('The board left its setup network, so it probably joined "' + name + '". Reconnect this device to that network, then open http://visualizer-esp-32.local (or the IP shown on the serial monitor).');
        break;
      }
    }
    if (!status.textContent || status.textContent.indexOf('Testing') === 0) {
      say('No answer from the board. If it joined the new network, open http://visualizer-esp-32.local on that network.', true);
    }
    applying = false; connectBtn.disabled = false;
  });

  $('wifi-forget').addEventListener('click', async function () {
    if (!confirm('Forget the saved network? The board will open its setup network, and you will have to join it to choose a new one.')) return;
    try {
      await fetch(base() + '/api/wifi/forget', { method: 'POST' });
      say('Forgotten. Join the setup network (visualizer-esp-32-XXXX) to choose a new one.');
    } catch (e) { say('board not reachable', true); }
  });

  setInterval(refresh, 3000);
  setTimeout(refresh, 300);
})();

// ---- Diagnostics (Setup tab) ----
// Reads /api/status every few seconds while the Setup tab is open.
(function () {
  var box = document.getElementById('diag-rows');
  if (!box) return;
  function base() { return (typeof espUrl === 'function') ? espUrl() : ''; }
  function esc(t) { var d = document.createElement('div'); d.textContent = t; return d.innerHTML; }
  function up(s) {
    var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    if (!d && !h && !m) return s + ' s';   // under a minute: just the seconds
    return (d ? d + ' d ' : '') + (d || h ? h + ' h ' : '') + m + ' min';
  }
  function row(k, v, bad) {
    return '<div style="display:flex;justify-content:space-between;gap:12px"><span>' + k + '</span>' +
      '<span style="color:' + (bad ? 'var(--err)' : 'var(--fg)') + '">' + v + '</span></div>';
  }
  function signal(r) {
    return r + ' dBm (' + window.vizSignal(r).word + ')';
  }
  async function refresh() {
    if (document.hidden) return;
    var tab = document.getElementById('tab-setup');
    if (tab && !tab.classList.contains('active')) return;
    var ctl = new AbortController(), to = setTimeout(function () { ctl.abort(); }, 2500);
    try {
      var j = await (await fetch(base() + '/api/status', { signal: ctl.signal })).json();
      box.innerHTML =
        row('uptime', up(j.uptimeS)) +
        row('last restart', esc(j.resetReason), j.abnormalReset) +
        row('free memory', Math.round(j.freeHeap / 1024) + ' kB (lowest ' + Math.round(j.minFreeHeap / 1024) + ' kB)') +
        row('Wi-Fi signal', j.wifiConnected ? signal(j.rssi) : 'not connected') +
        row('refused connections', j.refused + ' since boot');
    } catch (e) { box.textContent = 'board not reachable'; }
    finally { clearTimeout(to); }
  }
  setInterval(refresh, 3000);
  setTimeout(refresh, 400);
  document.querySelectorAll('[data-tab="setup"], [data-go="setup"]').forEach(function (b) {
    b.addEventListener('click', function () { setTimeout(refresh, 50); });
  });
})();

// ---- live sound bars (Setup tab) ----
// The same 0..1 band values the lights react to, so tuning changes show up here at once.
(function () {
  var cv = document.getElementById('setup-meter');
  if (!cv) return;
  var g = cv.getContext('2d'), W = cv.width, H = cv.height;
  var src = document.getElementById('setup-meter-src');
  var shown = [0, 0, 0, 0, 0, 0, 0, 0], audio = null, live = false, fetching = false;

  function base() { return (typeof espUrl === 'function') ? espUrl() : ''; }
  function active() {
    var tab = document.getElementById('tab-setup');
    return !document.hidden && (!tab || tab.classList.contains('active'));
  }
  async function fetchAudio() {
    if (fetching || !active()) return;
    fetching = true;
    var ctl = new AbortController(), to = setTimeout(function () { ctl.abort(); }, 1200);
    try {
      var r = await fetch(base() + '/api/audio', { signal: ctl.signal });
      if (!r.ok) throw new Error(r.status);
      audio = await r.json(); live = true; src.textContent = '';
    } catch (e) { live = false; src.textContent = 'no signal — lights not reachable'; }
    clearTimeout(to);
    fetching = false;
  }
  setInterval(fetchAudio, 150);

  function draw() {
    if (active()) {
      g.fillStyle = '#000'; g.fillRect(0, 0, W, H);
      var n = 8, gap = 14, bw = (W - gap * (n + 1)) / n, top = 8, base0 = H - 22;
      g.font = '14px ui-monospace, monospace'; g.textAlign = 'center';
      for (var i = 0; i < n; i++) {
        var target = (live && audio) ? Math.max(0, Math.min(1, audio.bands[i] || 0)) : 0;
        shown[i] += (target - shown[i]) * (target > shown[i] ? 0.5 : 0.15);   // fast up, slow down
        var h = shown[i] * (base0 - top), x = gap + i * (bw + gap);
        g.fillStyle = (typeof fastledCss === 'function') ? fastledCss(i / 9) : '#60a5fa';
        g.fillRect(x, base0 - h, bw, Math.max(2, h));
        g.fillStyle = '#8b90a4';
        g.fillText('band ' + i, x + bw / 2, H - 6);
      }
    }
    requestAnimationFrame(draw);
  }
  requestAnimationFrame(draw);
})();
