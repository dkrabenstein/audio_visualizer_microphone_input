// dom_harness.js -- loads the real data/www/index.html in jsdom, with everything a browser would
// normally supply (network, canvas, timers, dialogs) replaced by controllable fakes:
//
//   fetch      a recorder + router. Every request is logged in page.calls; `routes` decides the answer.
//   timers     a virtual clock. Nothing runs by itself; page.tick(ms) advances time and runs what is due,
//              so a 10-minute idle timeout takes no time and the page's pollers never fire unexpectedly.
//   canvas     a recording 2D context. page.canvas(id).rects lists every fillRect with its fillStyle.
//   <dialog>   showModal()/close() (jsdom does not implement them), so the token dialog can be driven.
//
// The page itself is not modified: its own scripts are loaded from data/www exactly as served.
const fs = require('fs');
const path = require('path');
const { JSDOM, VirtualConsole, requestInterceptor } = require('jsdom');
const { WWW, loadScript, extractFunction } = require('./load_web.js');

const LAPTOP_HOST_JS = 'window.VIZ_LAPTOP = true;\n';   // what server/server.js answers for /host.js

// Serves the page's own files from data/www (and /host.js the way the laptop server or the board does).
function diskInterceptor(laptop) {
  return requestInterceptor(request => {
    const p = decodeURIComponent(new URL(request.url).pathname);
    const js = { 'Content-Type': 'application/javascript' };
    if (p === '/host.js') return laptop ? new Response(LAPTOP_HOST_JS, { headers: js }) : new Response('', { status: 404 });   // the board has no /host.js
    const file = path.join(WWW, p);
    if (!file.startsWith(WWW + path.sep) || !fs.existsSync(file)) return new Response('', { status: 404 });
    return new Response(fs.readFileSync(file), { headers: js });
  });
}

const flush = async (n = 8) => { for (let i = 0; i < n; i++) await new Promise(r => setImmediate(r)); };

// The slot list a board would report once the page's presets are on it (see presetsOnBoard in core.js).
function boardSlots() {
  const { PRESETS, PRESET_FIRST_SLOT } = loadScript('presets.js', ['PRESETS', 'PRESET_FIRST_SLOT']);
  const { compile } = require('./load_compiler.js');
  const slotHash = new Function(extractFunction(fs.readFileSync(path.join(WWW, 'core.js'), 'utf8'), 'slotHash') + '; return slotHash;')();
  return PRESETS.map((p, n) => { const c = compile(p.expr); return { i: PRESET_FIRST_SLOT + n, used: true, name: p.name, ops: c.length, hash: slotHash(c) }; });
}

// Answer helpers for routes.
const json = (obj, status = 200) => ({ status, body: JSON.stringify(obj), type: 'application/json' });

function defaultRoutes() {
  return {
    'GET /api/slots': () => json({ slots: boardSlots(), active: 0, on: true, brightness: 80 }),
    'POST /api/pattern': (req) => { const b = JSON.parse(req.body); return json({ ok: true, ops: b.code.length, depth: 3 }); },
    'POST /api/select': () => json({ ok: true }),
    'POST /api/power': () => json({ ok: true }),
    'POST /api/brightness': () => json({ ok: true }),
    'POST /api/auth': () => json({ ok: true }),
  };
}

/**
 * options:
 *   laptop    true = page served by the laptop server (/host.js sets VIZ_LAPTOP)
 *   url       address the page is "served from" (default http://board.test/); may carry a #hash
 *   routes    { 'METHOD /path': (req) => answer }, merged over the defaults. An answer is json(...) or
 *             { status, body, type } or null for "connection failed". Unlisted requests get 404.
 *   storage   { key: value } put in localStorage before the page starts
 *   noAnimation  true = requestAnimationFrame never fires (keeps long clock jumps cheap)
 */
async function loadPage(options = {}) {
  const url = options.url || 'http://board.test/';
  const routes = Object.assign(defaultRoutes(), options.routes || {});
  const calls = [];
  const errors = [];
  const timers = new Map();
  let nextId = 1, now = 0, hidden = false, closeCalls = 0;

  const vc = new VirtualConsole();
  vc.on('jsdomError', e => {
    if (e.type === 'resource-loading' && /host\.js/.test(String(e.url || e.message))) return;   // expected on the board
    errors.push(e.type + ': ' + (e.cause && e.cause.stack ? e.cause.stack.split('\n').slice(0, 2).join(' ') : e.message));
  });
  vc.on('error', m => errors.push(String(m)));

  const html = fs.readFileSync(path.join(WWW, 'index.html'), 'utf8');
  const dom = new JSDOM(html, {
    url, runScripts: 'dangerously', resources: { interceptors: [diskInterceptor(options.laptop)] }, pretendToBeVisual: false, virtualConsole: vc,
    beforeParse(window) {
      if (options.storage) for (const [k, v] of Object.entries(options.storage)) window.localStorage.setItem(k, v);

      // ---- fetch
      window.Headers = Headers; window.Request = Request; window.Response = Response;
      window.fetch = async (input, init = {}) => {
        const u = new URL(typeof input === 'string' ? input : input.url, url);
        const method = String(init.method || 'GET').toUpperCase();
        const headers = {};
        for (const [k, v] of new Headers(init.headers || {})) headers[k.toLowerCase()] = v;
        const req = { url: u.pathname + u.search, origin: u.origin, href: u.href, method, headers, body: init.body === undefined ? undefined : String(init.body),
                      background: init.background === true, hasSignal: !!init.signal };
        calls.push(req);
        const handler = routes[method + ' ' + u.pathname];
        let ans = handler ? await handler(req, calls) : json({ error: 'not found' }, 404);
        if (ans === null) throw new TypeError('Failed to fetch');
        return new Response(ans.body === undefined ? '' : ans.body, { status: ans.status || 200, headers: { 'content-type': ans.type || 'application/json' } });
      };

      // ---- virtual clock
      window.setTimeout = (fn, ms = 0, ...args) => { const id = nextId++; timers.set(id, { fn, at: now + Math.max(0, +ms || 0), args, every: null }); return id; };
      window.setInterval = (fn, ms = 0, ...args) => { const id = nextId++; const e = Math.max(1, +ms || 1); timers.set(id, { fn, at: now + e, args, every: e }); return id; };
      window.clearTimeout = window.clearInterval = id => { timers.delete(id); };
      window.requestAnimationFrame = fn => options.noAnimation ? 0 : window.setTimeout(() => fn(now), 16);
      window.cancelAnimationFrame = id => timers.delete(id);

      // ---- window.close would tear the test page down
      window.__realClose = window.close.bind(window);
      Object.defineProperty(window, 'close', { configurable: true, value: () => { closeCalls++; } });

      window.confirm = () => true;   // tests that care replace it

      // ---- document.hidden is switchable
      Object.defineProperty(window.document, 'hidden', { configurable: true, get: () => hidden });

      // ---- canvas
      window.HTMLCanvasElement.prototype.getContext = function () {
        if (this._ctx) return this._ctx;
        const rects = [];
        const ctx = { rects, fillStyle: '#000', canvas: this,
          fillRect(x, y, w, h) { if (rects.length > 4000) rects.splice(0, 2000); rects.push({ style: this.fillStyle, x, y, w, h }); },
          createImageData(w, h) { return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }; },
          putImageData(img) { this.lastImage = img; }, clearRect() {}, beginPath() {}, arc() {}, fill() {}, stroke() {}, moveTo() {}, lineTo() {},
          save() {}, restore() {}, fillText() {}, strokeRect() {}, setTransform() {}, scale() {}, translate() {} };
        this._ctx = ctx;
        return ctx;
      };
      window.HTMLCanvasElement.prototype.getBoundingClientRect = function () { return { left: 0, top: 0, width: 280, height: 280, right: 280, bottom: 280 }; };
      window.HTMLCanvasElement.prototype.setPointerCapture = function () {};

      // ---- <dialog>
      const D = window.HTMLDialogElement.prototype;
      D.showModal = function () { this.setAttribute('open', ''); };
      D.close = function (value) {
        if (value !== undefined) this.returnValue = value;
        this.removeAttribute('open');
        this.dispatchEvent(new window.Event('close'));
      };
    },
  });
  const window = dom.window;
  await new Promise(r => window.document.readyState === 'complete' ? r() : window.addEventListener('load', r));
  await flush();

  const page = {
    window, document: window.document, calls, errors,
    get closeCalls() { return closeCalls; },
    $: s => window.document.querySelector(s),
    canvas: id => window.document.getElementById(id).getContext('2d'),
    text: s => window.document.querySelector(s).textContent,
    flush,
    // Calls made so far to an API path (optionally one method).
    to: (p, method) => calls.filter(c => c.url.split('?')[0] === p && (!method || c.method === method)),
    forgetCalls: () => { calls.length = 0; },
    storage: k => window.localStorage.getItem(k),
    setHidden(h) { hidden = h; window.document.dispatchEvent(new window.Event('visibilitychange')); },
    async tick(ms) {
      const end = now + ms;
      for (;;) {
        let best = null, bestId = null;
        for (const [id, t] of timers) if (t.at <= end && (!best || t.at < best.at)) { best = t; bestId = id; }
        if (!best) break;
        now = best.at;
        if (best.every) best.at = now + best.every; else timers.delete(bestId);
        try { best.fn(...best.args); } catch (e) { errors.push('timer: ' + e.message); }
        await flush(2);
      }
      now = end;
      await flush();
    },
    dispatch(name, detail) { window.document.dispatchEvent(new window.CustomEvent(name, { detail })); return flush(); },
    // Fire an input event after setting a value, like typing.
    async type(sel, value) {
      const el = window.document.querySelector(sel);
      el.value = value;
      el.dispatchEvent(new window.Event('input', { bubbles: true }));
      await flush();
    },
    async click(sel) {
      window.document.querySelector(sel).dispatchEvent(new window.MouseEvent('click', { bubbles: true }));
      await flush();
    },
    close() { window.__realClose(); },
  };
  return page;
}

module.exports = { loadPage, json, flush, boardSlots };
