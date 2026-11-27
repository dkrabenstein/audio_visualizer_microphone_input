// jsdom tests: the core of index.html -- tabs, the idle sleep, the Custom tab's compile/send,
// the preset upload on load, and laptop mode (the ESP32 connection panel).
const { loadPage, json, boardSlots } = require('./dom_harness.js');
const { loadScript } = require('./load_web.js');
const { compile } = require('./load_compiler.js');
const { suite } = require('./testkit.js');
const t = suite('page core');

const { PRESETS, ADVANCED_SLOT } = loadScript('presets.js', ['PRESETS', 'ADVANCED_SLOT']);

// A board that remembers the patterns uploaded to it, so the page sees them on its next /api/slots.
function statefulBoard(initial = []) {
  const have = new Map(initial.map(s => [s.i, s]));
  const slotsNow = boardSlots();
  return {
    posted: [],
    routes: null,
    init(self) {
      self.routes = {
        'GET /api/slots': () => json({ slots: [...have.values()], active: 0, on: true, brightness: 80 }),
        'POST /api/pattern': req => {
          const b = JSON.parse(req.body);
          self.posted.push({ slot: b.slot, name: b.name, abi: b.abi, code: b.code, background: req.background });
          have.set(b.slot, slotsNow.find(s => s.i === b.slot) || { i: b.slot, used: true, name: b.name, ops: b.code.length });
          return json({ ok: true, ops: b.code.length, depth: 3 });
        },
      };
      return self;
    },
  };
}
const newBoard = initial => { const b = statefulBoard(initial); return b.init(b); };

(async () => {
  // ---------------------------------------------------------------- tabs
  await t.test('tabs: Home first, buttons switch, address bar follows, old names work', async () => {
    const p = await loadPage();
    const active = () => ['home', 'basic', 'patterns', 'custom', 'setup'].filter(n => p.$('#tab-' + n).classList.contains('active'));
    t.eq(active(), ['home'], 'Home is showing at first');
    await p.click('#btn-patterns');
    t.eq(active(), ['patterns'], 'clicking Patterns shows only Patterns');
    t.ok(p.$('#btn-patterns').classList.contains('active') && !p.$('#btn-home').classList.contains('active'), 'the tab button follows');
    t.eq(p.window.location.hash, '#patterns', 'the address bar records the tab');
    await p.click('#btn-setup');
    t.eq(active(), ['setup'], 'Setup shows');
    p.close();
    for (const [hash, want] of [['#custom', 'custom'], ['#advanced', 'custom'], ['#nonsense', 'home'], ['', 'home']]) {
      const q = await loadPage({ url: 'http://board.test/' + hash });
      t.ok(q.$('#tab-' + want).classList.contains('active'), 'opening ' + (hash || 'with no hash') + ' shows ' + want);
      q.close();
    }
  });

  // ---------------------------------------------------------------- idle sleep
  await t.test('a page hidden for 10 minutes stops talking to the board, and wakes when shown', async () => {
    const p = await loadPage({ noAnimation: true });
    const IDLE = 10 * 60 * 1000;
    t.eq(p.window.vizAsleep(), false, 'awake at first');
    p.setHidden(true);
    await p.tick(IDLE - 1000);
    t.eq(p.window.vizAsleep(), false, 'still awake just before the limit');
    await p.tick(1000);
    t.eq(p.window.vizAsleep(), true, 'asleep after 10 minutes hidden');
    t.eq(p.closeCalls, 1, 'it also tries to close its own tab');
    p.forgetCalls();
    let rejected = null;
    try { await p.window.fetch('/api/slots'); } catch (e) { rejected = e; }
    t.ok(rejected && rejected.name === 'AbortError', 'fetch is refused up front while asleep');
    t.eq(p.calls.length, 0, 'no request reaches the network');
    p.setHidden(false);
    t.eq(p.window.vizAsleep(), false, 'awake again once shown');
    t.eq((await p.window.fetch('/api/slots')).status, 200, 'requests work again');
    p.close();
  });

  await t.test('showing the page again before the limit cancels the countdown', async () => {
    const p = await loadPage({ noAnimation: true });
    p.setHidden(true);
    await p.tick(5 * 60 * 1000);
    p.setHidden(false);
    await p.tick(11 * 60 * 1000);
    t.eq(p.window.vizAsleep(), false, 'never fell asleep');
    t.eq(p.closeCalls, 0, 'never tried to close');
    p.close();
  });

  // ---------------------------------------------------------------- Custom tab: compile + send
  await t.test('Custom tab: the status line reports ops and bytes, or the error with its column', async () => {
    const p = await loadPage();
    await p.type('#expr', '1+2');
    t.eq(p.text('#status'), '✓ 3 ops, 24 bytes', 'a valid expression');
    t.eq(p.$('#status').className, 'ok', 'styled as ok');
    await p.type('#expr', '1+');
    t.ok(p.text('#status').startsWith('✗ ') && /\(col \d+\)/.test(p.text('#status')), 'a broken expression shows the error and column: ' + p.text('#status'));
    t.eq(p.$('#status').className, 'err', 'styled as an error');
    await p.type('#expr', 'nope(1)');
    t.ok(/✗/.test(p.text('#status')), 'an unknown function is an error');
    p.close();
  });

  await t.test('Custom tab: the example links load a preset into the editor', async () => {
    const p = await loadPage();
    const links = [...p.document.querySelectorAll('#examples .ex')];
    t.eq(links.map(l => l.textContent), PRESETS.map(x => x.name), 'one link per preset, in order');
    links[3].dispatchEvent(new p.window.MouseEvent('click', { bubbles: true }));
    t.eq(p.$('#expr').value, PRESETS[3].expr, 'the expression appears in the editor');
    t.eq(p.text('#status').startsWith('✓'), true, 'and it is compiled');
    p.close();
  });

  await t.test('Custom tab: Send uploads to the Custom slot, selects it, and reports ops and depth', async () => {
    const p = await loadPage();
    await p.type('#expr', '0.5+0.5*sin(tau*x)');
    p.forgetCalls();
    await p.click('#send');
    const up = p.to('/api/pattern', 'POST')[0], sel = p.to('/api/select', 'POST')[0];
    t.ok(up && sel, 'both requests were made');
    const body = JSON.parse(up.body);
    t.eq([body.slot, body.name, body.abi], [ADVANCED_SLOT, 'advanced', 1], 'goes to the Custom slot as "advanced", ABI 1');
    t.eq(body.code, compile('0.5+0.5*sin(tau*x)'), 'the bytecode is what compiler.js produces');
    t.eq(sel.body, 'slot=' + ADVANCED_SLOT, 'then that slot is selected');
    t.eq(p.text('#status'), '✓ uploaded: ' + body.code.length + ' ops, depth 3', 'status shows the board\'s answer');
    p.close();
  });

  await t.test('Custom tab: Send refuses a broken expression and shows a board error', async () => {
    const p = await loadPage();
    await p.type('#expr', '1+');
    p.forgetCalls();
    await p.click('#send');
    t.eq(p.to('/api/pattern').length, 0, 'nothing is sent for a broken expression');
    t.eq(p.text('#status'), '✗ fix the expression first', 'the person is told');
    p.close();
    const q = await loadPage({ routes: { 'POST /api/pattern': () => json({ error: 'program rejected' }, 400) } });
    await q.type('#expr', '1+2');
    await q.click('#send');
    t.eq(q.text('#status'), '✗ device: program rejected', 'the board\'s error text is shown');
    t.eq(q.to('/api/select').length, 0, 'and the slot is not selected');
    q.close();
  });

  // ---------------------------------------------------------------- preset upload on load
  await t.test('presets already on the board: nothing is uploaded', async () => {
    const p = await loadPage();
    t.eq(p.to('/api/pattern', 'POST').length, 0, 'no uploads');
    t.eq(p.text('#status'), '✓ patterns already on the board', 'status says so');
    p.close();
  });

  await t.test('empty board: all presets go to slots 0..6, as background requests, and /api/select is never called', async () => {
    const b = newBoard();
    const p = await loadPage({ routes: b.routes });
    t.eq(b.posted.map(x => x.slot), PRESETS.map((_, n) => n), 'one upload per preset, to its own slot, in order');
    t.eq(b.posted.map(x => x.name), PRESETS.map(x => x.name), 'with the preset names');
    t.ok(b.posted.every(x => x.background), 'all marked as background (so they never raise the token dialog)');
    t.ok(b.posted.every((x, n) => JSON.stringify(x.code) === JSON.stringify(compile(PRESETS[n].expr)) && x.abi === 1), 'with the compiled code');
    t.eq(p.to('/api/select').length, 0, 'the running pattern is not interrupted');
    t.eq(p.text('#status'), '✓ patterns loaded', 'status says loaded');
    p.close();
  });

  await t.test('one preset different on the board: the presets are re-uploaded', async () => {
    const slots = boardSlots(); slots[3].hash = '00000000';
    const b = newBoard(slots);
    const p = await loadPage({ routes: b.routes });
    t.ok(b.posted.length >= PRESETS.length, 'uploaded again (' + b.posted.length + ' requests)');
    p.close();
  });

  await t.test('no API token yet: the first refused upload stops the rest and explains, with no dialog', async () => {
    let n = 0;
    const p = await loadPage({ routes: { 'GET /api/slots': () => json({ slots: [] }), 'POST /api/pattern': () => { n++; return json({ error: 'missing' }, 401); } } });
    t.eq(n, 1, 'stops after the first 401');
    t.ok(/no API token/.test(p.text('#status')), 'status explains: ' + p.text('#status'));
    t.ok(!p.document.getElementById('tok-dlg'), 'no token dialog was raised');
    p.close();
  });

  await t.test('a board error during upload is shown with the preset name', async () => {
    const p = await loadPage({ routes: { 'GET /api/slots': () => json({ slots: [] }),
      'POST /api/pattern': req => (JSON.parse(req.body).slot === 2 ? json({ error: 'too long' }, 400) : json({ ok: true })) } });
    t.eq(p.text('#status'), '✗ preset upload: ' + PRESETS[2].name + ': too long', 'names the failing preset');
    t.eq(p.$('#status').className, 'err', 'styled as an error');
    p.close();
  });

  await t.test('board unreachable: the page still loads and the preview keeps working', async () => {
    const p = await loadPage({ routes: { 'GET /api/slots': () => null, 'POST /api/pattern': () => null } });
    t.eq(p.errors, [], 'no script errors');
    await p.type('#expr', 'x');
    t.ok(p.text('#status').startsWith('✓'), 'the editor still compiles');
    p.close();
  });

  // ---------------------------------------------------------------- laptop mode
  await t.test('board page: the ESP32 connection panel stays hidden, requests are relative', async () => {
    const p = await loadPage();
    t.eq(p.$('#conn-panel').style.display, 'none', 'panel hidden');
    t.ok(p.calls.every(c => c.origin === 'http://board.test'), 'every request goes to the page\'s own address');
    p.close();
  });

  const cfg = (ip, source = 'mdns', extra = {}) => ({ 'GET /api/config': () => json(Object.assign({ host: 'visualizer-esp-32.local', ip, source }, extra)) });

  await t.test('laptop page: shows the panel, finds the board, and sends device requests straight to its IP', async () => {
    const p = await loadPage({ laptop: true, routes: cfg('192.168.1.77') });
    t.ok(p.$('#conn-panel').style.display !== 'none', 'panel visible');
    t.eq(p.text('#target'), '✓ ESP32 found at 192.168.1.77 (visualizer-esp-32.local)', 'target line');
    t.eq(p.$('#target').className, 'ok', 'styled as found');
    t.eq(p.$('#esp_ip').value, 'visualizer-esp-32.local', 'the host box shows the configured host');
    const slots = p.to('/api/slots');
    t.ok(slots.length > 0 && slots.every(c => c.origin === 'http://192.168.1.77'), 'device requests go to http://192.168.1.77, not to the laptop server');
    t.ok(p.to('/api/config').every(c => c.origin === 'http://board.test'), 'config requests stay on the laptop server');
    p.close();
  });

  await t.test('laptop page: target line wording for each way the address was found', async () => {
    for (const [source, want] of [['beacon', ' (auto-detected by beacon)'], ['manual', ' (manual address)'], ['mdns', ' (visualizer-esp-32.local)']]) {
      const p = await loadPage({ laptop: true, routes: cfg('10.0.0.9', source) });
      t.eq(p.text('#target'), '✓ ESP32 found at 10.0.0.9' + want, 'source ' + source);
      p.close();
    }
  });

  await t.test('laptop page: while the board is not found it says so, and keeps looking', async () => {
    let answers = 0;
    const p = await loadPage({ laptop: true, routes: { 'GET /api/config': () => { answers++; return json({ host: 'visualizer-esp-32.local', ip: answers > 2 ? '10.0.0.9' : null, source: 'none', error: 'ENOTFOUND' }); } } });
    t.eq(p.text('#target'), 'searching for ESP32… (visualizer-esp-32.local: ENOTFOUND)', 'searching text includes the error');
    await p.tick(2000);
    t.ok(/ESP32 found at 10\.0\.0\.9/.test(p.text('#target')), 'found on a later try');
    p.close();
  });

  await t.test('laptop page: the dev server being down is reported', async () => {
    const p = await loadPage({ laptop: true, routes: { 'GET /api/config': () => null } });
    t.eq(p.text('#target'), 'dev server unreachable', 'message');
    p.close();
  });

  await t.test('laptop page: Save posts the typed host to the laptop server and uploads presets to the new board', async () => {
    const b = newBoard();
    let seen = null;
    const p = await loadPage({ laptop: true, routes: Object.assign({}, b.routes, cfg('192.168.1.77'),
      { 'POST /api/config': req => { seen = JSON.parse(req.body); return json({ ok: true, host: seen.host, ip: '192.168.1.99', source: 'manual' }); } }) });
    p.forgetCalls();
    p.$('#esp_ip').value = '  192.168.1.99  ';
    await p.click('#save_ip');
    t.eq(seen, { host: '192.168.1.99' }, 'the host is trimmed and posted as JSON');
    t.eq(p.to('/api/config', 'POST')[0].origin, 'http://board.test', 'to the laptop server');
    t.eq(p.text('#target'), '✓ ESP32 found at 192.168.1.99 (manual address)', 'the target line updates');
    t.ok(p.calls.some(c => c.origin === 'http://192.168.1.99' && c.url === '/api/slots'), 'and the page talks to the new address');
    p.$('#esp_ip').value = 'something';
    await p.click('#auto_ip');
    t.eq(seen, { host: 'visualizer-esp-32.local' }, 'the Auto button goes back to the default name');
    p.close();
  });

  await t.test('laptop page: a board that moves to a new address gets its presets again', async () => {
    let ip = '192.168.1.77';
    const b = newBoard(boardSlots());
    const p = await loadPage({ laptop: true, routes: Object.assign({}, b.routes, { 'GET /api/config': () => json({ host: 'h', ip, source: 'beacon' }) }) });
    p.forgetCalls();
    ip = '192.168.1.88';
    await p.tick(3000);
    t.eq(p.text('#target'), '✓ ESP32 found at 192.168.1.88 (auto-detected by beacon)', 'new address shown');
    t.ok(p.calls.some(c => c.origin === 'http://192.168.1.88' && c.url === '/api/slots'), 'the page checks the board at its new address');
    p.close();
  });

  t.done();
})();
