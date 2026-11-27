// core.js -- the page's foundation, loaded first.
//   - the API token: wraps fetch() so changes to the board carry the token
//   - background-tab idle: a hidden page stops talking to the board
//   - the Custom tab: the formula editor, compile/send, and helpers shared by the other files
//     (espUrl, slotHash, presetsOnBoard)
// Plain script (no modules): everything here is global, and compiler.js / presets.js / fastled.js
// must be loaded before it. Keep the order in index.html: core.js, tabs.js, setup.js.

// ---- API token ----
// The board only accepts changes (POST requests) that carry the shared token from its secrets.h, in
// an "X-Api-Token" header. Rather than touch every fetch() in this page, fetch is wrapped once here:
// it adds the saved token to POSTs meant for the board, and if the board answers 401 it asks for the
// token in a small dialog (once per browser, kept in localStorage) and sends the request again.
// A request the page sends on its own (not because of something the person did) is marked
// { background: true }. Those never raise the dialog: without a token they just get their 401 back,
// so a first-time visitor is not met by a token prompt before they have touched anything.
(function () {
  var KEY = 'viz_api_token';
  var realFetch = window.fetch.bind(window);
  var asking = null;                       // one dialog at a time, however many requests fail together

  function saved() { try { return localStorage.getItem(KEY) || ''; } catch (e) { return ''; } }
  function save(t) { try { if (t) localStorage.setItem(KEY, t); else localStorage.removeItem(KEY); } catch (e) {} }

  // On the board every request is for the board. On the laptop page, device requests use an absolute
  // URL (espUrl) and anything relative goes to the laptop's own server, which must not get the token.
  function isForBoard(url) {
    if (window.VIZ_LAPTOP === true) return /^https?:\/\//i.test(url);
    return true;
  }

  // `retry` drops the caller's abort signal: callers set short timeouts meant for a slow network, but
  // a retry follows a dialog the person took their time over, and an already-fired signal would
  // make the retry fail before it left.
  function withToken(init, token, retry) {
    var o = Object.assign({}, init || {});
    if (retry) delete o.signal;
    var h = new Headers(o.headers || {});
    if (token) h.set('X-Api-Token', token);
    o.headers = h;
    return o;
  }

  // The dialog is built on first use. Resolves to the entered token, or '' if cancelled.
  function showDialog(note) {
    return new Promise(function (resolve) {
      var dlg = document.getElementById('tok-dlg');
      if (!dlg) {
        dlg = document.createElement('dialog');
        dlg.id = 'tok-dlg';
        dlg.style.cssText = 'background:var(--panel,#14161d);color:var(--fg,#e6e6e6);border:1px solid var(--line,#2a2f3d);' +
          'border-radius:8px;padding:18px;max-width:min(92vw,420px);width:100%';
        dlg.innerHTML =
          '<form method="dialog">' +
          '<label style="display:block;font-size:15px;text-transform:uppercase;letter-spacing:.08em;color:var(--acc,#60a5fa);margin-bottom:8px">API token needed</label>' +
          '<div style="font-size:14px;line-height:1.4;margin-bottom:10px">This board only accepts changes from devices that know its token. ' +
          'Enter the <b>API_TOKEN</b> value from <code>include/secrets.h</code>. It is saved in this browser, so you only do this once.</div>' +
          '<div id="tok-note" style="font-size:13px;color:var(--err,#f87171);min-height:18px;margin-bottom:6px"></div>' +
          '<input id="tok-in" type="password" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="API token" ' +
          'style="width:100%;box-sizing:border-box;background:#0b0c10;border:1px solid var(--line,#2a2f3d);color:var(--fg,#e6e6e6);padding:10px;border-radius:5px;font:inherit">' +
          '<label style="display:block;font-size:13px;color:var(--dim,#9aa0b0);margin:8px 0 14px;text-transform:none;letter-spacing:0">' +
          '<input id="tok-show" type="checkbox"> show token</label>' +
          '<div style="display:flex;gap:8px;justify-content:flex-end">' +
          '<button value="cancel" id="tok-cancel">Cancel</button>' +
          '<button value="ok" id="tok-ok" class="primary">Save</button></div></form>';
        document.body.appendChild(dlg);
        dlg.querySelector('#tok-show').addEventListener('change', function (e) {
          dlg.querySelector('#tok-in').type = e.target.checked ? 'text' : 'password';
        });
      }
      var input = dlg.querySelector('#tok-in');
      input.value = '';
      input.type = 'password';
      dlg.querySelector('#tok-show').checked = false;
      dlg.querySelector('#tok-note').textContent = note || '';
      dlg.returnValue = '';
      dlg.onclose = function () {
        resolve(dlg.returnValue === 'ok' ? input.value.trim() : '');
        input.value = '';
      };
      dlg.querySelector('#tok-cancel').formNoValidate = true;
      if (typeof dlg.showModal === 'function') { dlg.showModal(); input.focus(); }
      else {                                // very old browser: fall back to the built-in prompt
        var t = window.prompt('Enter the API_TOKEN value from include/secrets.h:', '');
        resolve((t || '').trim());
      }
    });
  }

  function ask(note) {
    if (!asking) asking = showDialog(note).finally(function () { asking = null; });
    return asking;
  }

  window.fetch = async function (input, init) {
    var url = (typeof input === 'string') ? input : (input && input.url) || '';
    var method = ((init && init.method) || (input && input.method) || 'GET').toUpperCase();
    if (method === 'GET' || method === 'HEAD' || method === 'OPTIONS' || !isForBoard(url)) {
      return realFetch(input, init);
    }
    var sent = saved();
    var res = await realFetch(input, withToken(init, sent));
    if (init && init.background === true) {
      if (res.status === 401 && sent) save('');   // a refused token is wrong; the next real action asks again
      return res;
    }
    var note = '';
    if (res.status === 401 && sent) {      // the board refused the token we had: it is wrong, so drop it
      save('');
      note = 'The saved token was not accepted (did secrets.h change?). Enter it again.';
    }
    for (var tries = 0; res.status === 401 && tries < 3; tries++) {   // missing or wrong token
      var t = await ask(note);
      if (!t) return res;                  // cancelled: let the caller see the 401
      res = await realFetch(input, withToken(init, t, true));
      if (res.status === 401) { save(''); note = 'That token was not accepted. Check it and try again.'; }
      else save(t);                        // only keep a token the board accepted
    }
    return res;
  };

  // ---- Pairing link ----
  // Opening  http://<board>/#token=<API_TOKEN>  saves the token in this browser, so it never has to
  // be typed. Everything after the # stays in the browser: it is not sent to the board, so it does
  // not appear in any request or log. The token is removed from the address bar straight away, so it
  // is not left in the history, a bookmark of the page, or a screenshot.
  (function pair() {
    var parts = location.hash.replace(/^#/, '').split('&'), tok = '', rest = [], found = false;
    parts.forEach(function (p) {
      if (p.indexOf('token=') === 0) {
        found = true;
        try { tok = decodeURIComponent(p.slice(6)); } catch (e) { tok = ''; }
      } else if (p) rest.push(p);
    });
    if (!found) return;
    try { history.replaceState(null, '', location.pathname + location.search + (rest.length ? '#' + rest.join('&') : '')); } catch (e) {}

    function toast(msg, bad) {
      var d = document.createElement('div');
      d.textContent = msg;
      d.style.cssText = 'position:fixed;left:50%;bottom:20px;transform:translateX(-50%);z-index:9999;' +
        'max-width:min(92vw,420px);padding:12px 16px;border-radius:8px;font-size:14px;line-height:1.4;' +
        'background:var(--panel,#14161d);color:var(--fg,#e6e6e6);border:1px solid ' + (bad ? 'var(--err,#f87171)' : 'var(--ok,#4ade80)');
      document.body.appendChild(d);
      setTimeout(function () { d.remove(); }, 7000);
    }

    if (!tok) { toast('The pairing link has no token in it.', true); return; }
    // Saved straight away, before the board has answered: the page sends its own requests the moment
    // it loads (it uploads its patterns), and they must already carry the token or they get a 401 and
    // raise the token dialog. If the board then refuses the token, it is taken back out.
    save(tok);
    if (window.VIZ_LAPTOP === true) {      // the board's address is not known yet: it is checked on first use
      toast('Token saved in this browser.');
      return;
    }
    // Ask the board whether it accepts the token.
    realFetch('/api/auth', { method: 'POST', headers: { 'X-Api-Token': tok } }).then(function (r) {
      if (r.ok) toast('Paired. This browser can now change the lights.');
      else if (r.status === 401) { save(''); toast('The board did not accept the token in that link. Check you used the current one.', true); }
      else toast('The board answered with an error (' + r.status + '). Try the link again.', true);
    }).catch(function () {
      toast('Token saved, but the board did not answer just now. It is checked on your next change.', true);
    });
  })();
})();

// Background pages release the board. Every open page costs the ESP32 TCP connections and CPU, and
// enough of them made the board reboot. A page that has been hidden for IDLE_MS stops making any
// request at all (fetch is rejected up front, so no connection is opened), then picks up again the
// moment the tab is shown. A web page can't close its own tab unless a script opened it, so this
// also tries window.close() (works for script-opened windows, silently ignored otherwise).
(function () {
  const IDLE_MS = 10 * 60 * 1000;
  let asleep = false, timer = null;
  const realFetch = window.fetch.bind(window);
  window.vizAsleep = () => asleep;
  window.fetch = function (...a) {
    return asleep ? Promise.reject(new DOMException('page idle', 'AbortError')) : realFetch(...a);
  };
  document.addEventListener('visibilitychange', () => {
    clearTimeout(timer);
    if (document.hidden) {
      timer = setTimeout(() => { asleep = true; try { window.close(); } catch (e) {} }, IDLE_MS);
    } else if (asleep) {
      asleep = false;   // pollers resume on their next tick and resync the UI
    }
  });
})();

const $ = s => document.querySelector(s);
let code = null;

function build() {
  const src = $('#expr').value;
  try {
    code = compile(src);
    $('#status').className = 'ok';
    $('#status').textContent =
      '\u2713 ' + code.length + ' ops, ' + (code.length * 8) + ' bytes';
  } catch (e) {
    code = null;
    $('#status').className = 'err';
    $('#status').textContent = '\u2717 ' + e.message +
      (e.pos !== undefined ? '  (col ' + (e.pos + 1) + ')' : '');
  }
}

// The Node server resolves the hostname (mDNS) and reports the IP. Device
// requests use that IP, so the browser doesn't repeat the mDNS lookup.
let espIp = null;
// Laptop mode (window.VIZ_LAPTOP, set by the Node server's /host.js): the page is served by the PC,
// so device requests go to the board's address. On the board there is no espUrl at all: every
// other script in this page treats "no espUrl" as "served by the board" and uses relative paths.
const ON_LAPTOP = window.VIZ_LAPTOP === true;
var espUrl;
if (ON_LAPTOP) espUrl = () => 'http://' + (espIp || $('#esp_ip').value);
const apiBase = () => (typeof espUrl === 'function' ? espUrl() : '');
if (ON_LAPTOP) $('#conn-panel').style.display = '';

const DEFAULT_HOST = 'visualizer-esp-32.local';
let lastIp = null;

function showTarget(j) {
  espIp = j.ip;
  const el = $('#target');
  if (!j.ip) {
    el.className = '';
    el.textContent = 'searching for ESP32\u2026 (' + j.host +
      (j.error ? ': ' + j.error : '') + ')';
  } else {
    el.className = 'ok';
    el.textContent = '\u2713 ESP32 found at ' + j.ip +
      (j.source === 'beacon' ? ' (auto-detected by beacon)'
       : j.source === 'manual' ? ' (manual address)'
       : ' (' + j.host + ')');
  }
  // A board that moved to a new address has no patterns yet: send them again.
  if (j.ip && lastIp && j.ip !== lastIp) { uploadPresets(); poll(); }
  if (j.ip) lastIp = j.ip;
}

// Ask the Node server what it currently knows. The beacon can change the
// address at any time (DHCP), so this runs every few seconds.
async function refreshTarget() {
  try {
    const j = await (await fetch('/api/config')).json();
    if (document.activeElement !== $('#esp_ip')) $('#esp_ip').value = j.host;
    showTarget(j);
    return j;
  } catch (e) {
    $('#target').className = '';
    $('#target').textContent = 'dev server unreachable';
    return null;
  }
}

// Point the Node server's UDP stream at a new host or IP.
async function setTarget(host) {
  $('#target').className = '';
  $('#target').textContent = 'resolving ' + host + '\u2026';
  try {
    const j = await (await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ host })
    })).json();
    lastIp = null;      // handled by the explicit upload below
    showTarget(j);
  } catch (e) {
    $('#target').textContent = 'dev server unreachable';
  }
  uploadPresets();  // a new target has no presets yet
}

$('#save_ip').onclick = () => setTarget($('#esp_ip').value.trim());
$('#auto_ip').onclick = () => { $('#esp_ip').value = DEFAULT_HOST; setTarget(DEFAULT_HOST); };

// Start with whatever the Node server is currently streaming to. Its first
// lookup may still be running, so retry briefly until an IP comes back.
async function loadTarget(tries) {
  const j = await refreshTarget();
  if (j && !j.ip && tries > 0) return setTimeout(() => loadTarget(tries - 1), 1000);
  uploadPresets();
}
if (ON_LAPTOP) setInterval(refreshTarget, 3000);

// --- Preview using simulated audio synced from the PC server ---
const cv = $('#cv'), cx = cv.getContext('2d');
const N = 150, W = cv.width / N;
let t0 = performance.now();
let liveAudio = { bands: [0,0,0,0,0,0,0,0], level: 0, bass: 0, mid: 0, treb: 0 };

// Only the Custom tab's preview canvas uses liveAudio, so ask for it only while that tab is showing
// (and the browser tab is visible), 10 times a second, one request at a time. It used to poll 20 times a
// second on every tab, which is enough extra traffic to make the board's LED output stall.
let liveBusy = false;
setInterval(async () => {
  if (liveBusy || document.hidden || !$('#tab-custom').classList.contains('active')) return;
  liveBusy = true;
  const ctl = new AbortController(), to = setTimeout(() => ctl.abort(), 1200);
  try {
    const res = await fetch('/api/audio', { signal: ctl.signal });
    liveAudio = await res.json();
  } catch (e) {}
  clearTimeout(to);
  liveBusy = false;
}, 100);

function frame() {
  const now = performance.now();
  const t = (now - t0) / 1000;
  cx.fillStyle = '#000'; 
  cx.fillRect(0, 0, cv.width, cv.height);

  if (code) {
    const ctx = {
      t,
      i: 0,
      bands: liveAudio.bands,
      bass: liveAudio.bass,
      mid: liveAudio.mid,
      treb: liveAudio.treb,
      level: liveAudio.level
    };

    for (let p = 0; p < N; p++) {
      ctx.x = p / (N - 1);
      let v = evalCode(code, ctx);
      v = v - Math.floor(v);
      cx.fillStyle = fastledCss(v);
      cx.fillRect(p * W, 0, W + 1, cv.height);
    }
  }
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

// --- Send bytecode directly to ESP32 HTTP Server ---
$('#send').onclick = async () => {
  if (!code) { $('#status').textContent = '\u2717 fix the expression first'; return; }
  const slot = ADVANCED_SLOT;
  const name = 'advanced';

  try {
    const r = await fetch(apiBase() + '/api/pattern', {
      method: 'POST', 
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(toPayload(name, slot, code))
    });
    const j = await r.json();
    if (!r.ok) throw new Error(j.error || r.status);
    
    await fetch(apiBase() + '/api/select', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'slot=' + slot 
    });

    $('#status').className = 'ok';
    $('#status').textContent =
      '\u2713 uploaded: ' + j.ops + ' ops, depth ' + j.depth;
    poll();
  } catch (e) {
    $('#status').className = 'err';
    $('#status').textContent = '\u2717 device: ' + e.message;
  }
};

let presetHealAt = 0;
async function poll() {
  try {
    const j = await (await fetch(apiBase() + '/api/slots')).json();
    document.dispatchEvent(new CustomEvent('devstate', { detail: j }));
    // If the board is missing a preset (a fresh or erased board, or presets.js changed), send them
    // again. The cooldown stops a failing upload from re-triggering itself through the poll() at the
    // end of uploadPresets().
    if (j.slots && !presetsOnBoard(j.slots) && Date.now() - presetHealAt > 10000) {
      presetHealAt = Date.now();
      uploadPresets();
    }
  } catch (e) {
    document.dispatchEvent(new CustomEvent('devoffline'));  // the preview still works
  }
}

$('#refresh').onclick = poll;
// Preset links: load an expression into the editor (does not upload it).
PRESETS.forEach((p, n) => {
  const el = document.createElement('span');
  el.className = 'ex';
  el.textContent = p.name;
  el.title = p.expr;
  el.onclick = () => {
    $('#expr').value = p.expr;
    build();
  };
  $('#examples').appendChild(el);
});

// Fingerprint of a compiled program, matching programHash() in the firmware (FNV-1a over each op's
// code byte and its argument as a little-endian 32-bit float). /api/slots reports it per slot.
function slotHash(code) {
  let h = 2166136261 >>> 0;
  const f = new DataView(new ArrayBuffer(4));
  const eat = b => { h = Math.imul((h ^ b) >>> 0, 16777619) >>> 0; };
  for (const op of code) {
    eat(op[0] & 255);
    f.setFloat32(0, (op.length > 1 ? op[1] : 0) + 0, true);   // "+ 0" turns -0 into 0, as JSON does
    for (let k = 0; k < 4; k++) eat(f.getUint8(k));
  }
  return h.toString(16).padStart(8, '0');
}

// True when every preset slot on the board already holds that exact preset: same name and the same
// program (compared by fingerprint, so an edit that keeps the name and length is still noticed). The
// board keeps its slots in flash, so after the first upload this is normally true and a page load
// needs no POSTs at all. A board whose firmware predates the fingerprint is compared by name and
// op count only.
function presetsOnBoard(slots) {
  return PRESETS.every((p, n) => {
    const s = (slots || []).find(x => x.i === PRESET_FIRST_SLOT + n);
    if (!s || !s.used || s.name !== p.name) return false;
    const code = compile(p.expr);
    return s.hash !== undefined ? s.hash === slotHash(code) : s.ops === code.length;
  });
}

// Upload the presets the board is missing, each to its own slot (PRESET_FIRST_SLOT onward). Only
// /api/pattern is called, never /api/select, so the running pattern is not interrupted. These are the
// page's own requests, so they are sent as background and never raise the token dialog.
let presetsBusy = false;
async function uploadPresets() {
  if (presetsBusy) return;   // overlapping uploads would interleave in the board's request buffer
  presetsBusy = true;
  try {
    try {
      const j = await (await fetch(apiBase() + '/api/slots')).json();
      if (presetsOnBoard(j.slots)) {
        $('#status').className = 'ok';
        $('#status').textContent = '✓ patterns already on the board';
        presetsBusy = false;
        poll();
        return;
      }
    } catch (e) { /* cannot tell what the board holds: upload them to be safe */ }
    for (let n = 0; n < PRESETS.length; n++) {
      const p = PRESETS[n], slot = PRESET_FIRST_SLOT + n;
      const r = await fetch(apiBase() + '/api/pattern', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(toPayload(p.name, slot, compile(p.expr))),
        background: true
      });
      if (r.status === 401) {
        const e = new Error('no API token in this browser yet');
        e.needsToken = true;
        throw e;
      }
      if (!r.ok) throw new Error(p.name + ': ' + ((await r.json()).error || r.status));
    }
    $('#status').className = 'ok';
    $('#status').textContent = '✓ patterns loaded';
  } catch (e) {
    if (e.needsToken) {
      // Try again in a minute: by then the person may have entered the token for something else.
      presetHealAt = Date.now() + 50000;
      $('#status').className = '';
      $('#status').textContent = 'patterns not loaded yet: this browser has no API token (it is asked for when you change something)';
    } else {
      $('#status').className = 'err';
      $('#status').textContent = '✗ preset upload: ' + e.message;
    }
  }
  presetsBusy = false;
  poll();
}

$('#expr').addEventListener('input', build);

build();

// First contact with the board. Held back until every script has run: tabs.js and setup.js listen for the
// 'devstate' event this triggers, and if the board answered while they were still downloading they would
// miss it (the sliders would show the wrong brightness until the next poll).
// On the board there is no target discovery step, so load the presets directly. On the laptop, find the
// board through the PC server first (see loadTarget above).
function whenAllScriptsRun(fn) {
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', fn);
  else fn();
}
whenAllScriptsRun(() => { if (ON_LAPTOP) loadTarget(5); else uploadPresets(); });
