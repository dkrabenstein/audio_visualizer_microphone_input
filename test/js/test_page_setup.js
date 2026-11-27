// jsdom tests: the Setup tab of index.html -- LED strips, microphone tuning, Wi-Fi, diagnostics.
const { loadPage, json } = require('./dom_harness.js');
const { suite } = require('./testkit.js');
const t = suite('page setup');

const openSetup = async (p, ms = 600) => {
  await p.click('#btn-setup');
  for (const d of p.document.querySelectorAll('#tab-setup details')) d.open = true;
  await p.tick(ms);
};
const change = async (p, el, value) => {
  if (el.type === 'checkbox') el.checked = value; else el.value = value;
  el.dispatchEvent(new p.window.Event('change', { bubbles: true }));
  await p.flush();
};

const STRIPS = { counts: [180, 100, 180, 101], reversed: [false, false, false, false], max: 300, defaults: [180, 100, 180, 101] };
const TUNING = { floors: [-78, -80, -84, -86, -85, -89, -92, -95], margin: 5, minCeil: -55, smoothness: 40,
                 raw: [-70, -90, -84, -86, -85, -89, -92, -95], calibrating: false };

(async () => {
  // ---------------------------------------------------------------- LED strips
  await t.test('Strips: the board\'s lengths and directions are shown', async () => {
    const p = await loadPage({ routes: { 'GET /api/strips': () => json(Object.assign({}, STRIPS, { counts: [60, 1, 1, 1], reversed: [false, true, false, false] })) } });
    await openSetup(p);
    const rows = [...p.document.querySelectorAll('#strip-rows .rangeRow')];
    t.eq(rows.map(r => r.querySelector('input[type=number]').value), ['60', '1', '1', '1'], 'counts (including the {60,1,1,1} development setup)');
    t.eq(rows.map(r => r.querySelector('input[type=checkbox]').checked), [false, true, false, false], 'reverse flags');
    t.eq(rows[0].querySelector('input[type=number]').max, '300', 'the limit comes from the board');
    p.close();
  });

  await t.test('Strips: changing a count sends all four counts and the flags', async () => {
    let seen = null;
    const p = await loadPage({ routes: { 'GET /api/strips': () => json(STRIPS), 'POST /api/strips': req => { seen = JSON.parse(req.body); return json({ ok: true }); } } });
    await openSetup(p);
    const nums = [...p.document.querySelectorAll('#strip-rows input[type=number]')];
    await change(p, nums[0], '60');
    t.eq(seen, { counts: [60, 100, 180, 101], reversed: [false, false, false, false] }, 'request body');
    t.eq(p.text('#strip-status'), 'applied (not saved yet)', 'status says it is live but not saved');
    await change(p, p.document.querySelectorAll('#strip-rows input[type=checkbox]')[2], true);
    t.eq(seen.reversed, [false, false, true, false], 'the reverse flag is sent');
    p.close();
  });

  await t.test('Strips: bad counts are caught in the page and never sent', async () => {
    const p = await loadPage({ routes: { 'GET /api/strips': () => json(STRIPS), 'POST /api/strips': () => json({ ok: true }) } });
    await openSetup(p);
    const n = p.document.querySelector('#strip-rows input[type=number]');
    for (const bad of ['0', '301', '1.5', '-4', '']) {
      p.forgetCalls();
      await change(p, n, bad);
      t.eq(p.to('/api/strips', 'POST').length, 0, JSON.stringify(bad) + ' is not sent');
      t.eq(p.text('#strip-status'), 'enter a whole number from 1 to 300', JSON.stringify(bad) + ' explains the range');
    }
    for (const good of ['1', '300']) {
      p.forgetCalls();
      await change(p, n, good);
      t.eq(p.to('/api/strips', 'POST').length, 1, good + ' is sent (the limits themselves are valid)');
    }
    p.close();
  });

  await t.test('Strips: a board refusal shows its message; an unreachable board is reported; Save and Reset call the board', async () => {
    const p = await loadPage({ routes: { 'GET /api/strips': () => json(STRIPS), 'POST /api/strips': () => json({ error: 'too many LEDs' }, 400),
                                         'POST /api/leds/save': () => json({ ok: true }), 'POST /api/leds/reset': () => json({ ok: true }) } });
    await openSetup(p);
    await change(p, p.document.querySelector('#strip-rows input[type=number]'), '50');
    t.eq(p.text('#strip-status'), 'too many LEDs', 'the board\'s error is shown');
    await p.click('#strip-save');
    t.eq(p.text('#strip-status'), 'saved to the board', 'saved');
    t.eq(p.to('/api/leds/save', 'POST').length, 1, 'save request sent');
    await p.click('#strip-reset');
    t.eq(p.text('#strip-status'), 'defaults restored', 'reset');
    p.close();
    const q = await loadPage({ routes: { 'GET /api/strips': () => json(STRIPS), 'POST /api/strips': () => null } });
    await openSetup(q);
    await change(q, q.document.querySelector('#strip-rows input[type=number]'), '50');
    t.eq(q.text('#strip-status'), 'lights not reachable', 'unreachable board');
    q.close();
  });

  // ---------------------------------------------------------------- microphone tuning
  await t.test('Mic tuning: the board\'s settings fill the controls, and the live level is shown per band', async () => {
    const p = await loadPage({ routes: { 'GET /api/tuning': () => json(TUNING) } });
    await openSetup(p);
    const sliders = [...p.document.querySelectorAll('#tune-rows input[type=range]')];
    t.eq(sliders.map(s => +s.value), TUNING.floors, 'eight floor sliders');
    t.eq([p.$('#tune-margin').value, p.$('#tune-ceil').value, p.$('#tune-smooth').value], ['5', '-55', '40'], 'margin, ceiling, smoothness');
    t.eq(p.text('#tune-smooth-val'), '40%', 'labels follow');
    const rows = [...p.document.querySelectorAll('#tune-rows .rangeRow')];
    t.eq(rows[0].children[3].textContent, 'now -70.0', 'live level for band 0');
    t.ok(/ok/.test(rows[0].children[3].style.color), 'a band above its floor + margin is highlighted');
    t.ok(/dim/.test(rows[1].children[3].style.color), 'a quiet band is not');
    p.close();
  });

  await t.test('Mic tuning: edits are sent after a short pause, as one request, with the numbers the board expects', async () => {
    let seen = [];
    const p = await loadPage({ routes: { 'GET /api/tuning': () => json(TUNING), 'POST /api/tuning': req => { seen.push(JSON.parse(req.body)); return json({ ok: true }); } } });
    await openSetup(p);
    const sliders = [...p.document.querySelectorAll('#tune-rows input[type=range]')];
    const input = el => el.dispatchEvent(new p.window.Event('input', { bubbles: true }));
    sliders[0].value = '-70'; input(sliders[0]);
    sliders[1].value = '-72'; input(sliders[1]);
    p.$('#tune-margin').value = '8'; input(p.$('#tune-margin'));
    await p.tick(50);
    t.eq(seen.length, 0, 'nothing is sent while edits keep coming');
    await p.tick(200);
    t.eq(seen.length, 1, 'one request after the pause');
    t.eq(seen[0], { floors: [-70, -72, -84, -86, -85, -89, -92, -95], margin: 8, minCeil: -55, smoothness: 40 }, 'body');
    t.eq(p.text('#tune-status'), 'applied (not saved yet)', 'status');
    p.close();
  });

  await t.test('Mic tuning: calibrate, save and reset call the board', async () => {
    const p = await loadPage({ routes: { 'GET /api/tuning': () => json(TUNING), 'POST /api/mic/calibrate': () => json({ ok: true }),
                                          'POST /api/mic/save': () => json({ ok: true }), 'POST /api/mic/reset': () => json({ ok: true }) } });
    await openSetup(p);
    await p.click('#tune-cal');
    t.eq(p.to('/api/mic/calibrate', 'POST').length, 1, 'calibrate sent');
    t.ok(/calibrating/.test(p.text('#tune-status')), 'status asks for a quiet room');
    await p.click('#tune-save');
    t.eq(p.text('#tune-status'), 'saved to the board', 'saved');
    await p.click('#tune-reset');
    t.eq(p.text('#tune-status'), 'defaults restored', 'reset');
    p.close();
    const q = await loadPage({ routes: { 'GET /api/tuning': () => json(TUNING), 'POST /api/mic/save': () => json({}, 500) } });
    await openSetup(q);
    await q.click('#tune-save');
    t.eq(q.text('#tune-status'), 'save failed', 'a failed save is reported');
    q.close();
  });

  // ---------------------------------------------------------------- Wi-Fi
  await t.test('Wi-Fi: signal wording thresholds', async () => {
    const p = await loadPage();
    const sig = r => p.window.vizSignal(r).word;
    t.eq([sig(-30), sig(-60), sig(-61), sig(-70), sig(-71), sig(-95)], ['good', 'good', 'fair', 'fair', 'weak', 'weak'], 'good to -60, fair to -70, weak below');
    p.close();
  });

  const WIFI = { connected: true, state: 'connected', ssid: 'HomeNet', ip: '192.168.1.82', rssi: -50, host: 'visualizer-esp-32.local', ap: false };

  await t.test('Wi-Fi: the connected line shows network, address, signal and name', async () => {
    const p = await loadPage({ routes: { 'GET /api/wifi': () => json(WIFI) } });
    await openSetup(p, 400);
    const text = p.text('#wifi-now');
    t.ok(text.includes('Connected to HomeNet') && text.includes('192.168.1.82') && text.includes('good connection') && text.includes('visualizer-esp-32.local'), 'line: ' + text);
    p.close();
  });

  await t.test('Wi-Fi: a hostile network name is shown as text, not run as HTML', async () => {
    const evil = '<img src=x onerror=alert(1)>';
    const p = await loadPage({ routes: { 'GET /api/wifi': () => json(Object.assign({}, WIFI, { ssid: evil })) } });
    await openSetup(p, 400);
    t.eq(p.$('#wifi-now').querySelector('img'), null, 'no element was created from the name');
    t.ok(p.text('#wifi-now').includes(evil), 'the characters are shown as they are');
    p.close();
  });

  await t.test('Wi-Fi: setup mode opens the Setup tab, scans, and fills the list with signal wording', async () => {
    const p = await loadPage({ routes: {
      'GET /api/wifi': () => json({ connected: false, state: 'ap', ap: true, ssid: '', apSsid: 'visualizer-esp-32-AB12' }),
      'GET /api/wifi/scan': () => json({ scanning: false, networks: [{ ssid: 'Cafe', rssi: -45, secure: true }, { ssid: 'Guest', rssi: -80, secure: false }] }) } });
    await p.tick(400);
    t.ok(p.$('#tab-setup').classList.contains('active'), 'the page jumps to Setup');
    t.ok(/No network saved/.test(p.text('#wifi-now')) && /visualizer-esp-32-AB12/.test(p.text('#wifi-now')), 'explains setup mode: ' + p.text('#wifi-now'));
    const opts = [...p.document.querySelectorAll('#wifi-list option')].map(o => o.textContent);
    t.eq(opts, ['choose a network (2 found)', 'Cafe · good connection', 'Guest (open) · weak connection'], 'list entries');
    const list = p.$('#wifi-list'); list.value = 'Cafe'; list.dispatchEvent(new p.window.Event('change', { bubbles: true }));
    t.eq(p.$('#wifi-ssid').value, 'Cafe', 'choosing one fills the name box');
    p.close();
  });

  await t.test('Wi-Fi: Connect checks the input before sending anything', async () => {
    const p = await loadPage({ routes: { 'GET /api/wifi': () => json(WIFI), 'POST /api/wifi': () => json({ ok: true }) } });
    await openSetup(p, 400);
    p.forgetCalls();
    p.$('#wifi-ssid').value = '';
    await p.click('#wifi-connect');
    t.eq(p.text('#wifi-status'), 'enter or choose a network name', 'empty name');
    p.$('#wifi-ssid').value = 'Home'; p.$('#wifi-pass').value = 'short';
    await p.click('#wifi-connect');
    t.eq(p.text('#wifi-status'), 'a Wi-Fi password is 8 to 63 characters', 'short password');
    p.$('#wifi-pass').value = 'x'.repeat(64);
    await p.click('#wifi-connect');
    t.eq(p.text('#wifi-status'), 'a Wi-Fi password is 8 to 63 characters', 'long password');
    t.eq(p.to('/api/wifi', 'POST').length, 0, 'nothing was sent');
    p.close();
  });

  await t.test('Wi-Fi: Connect sends the details, then reports the result the board gives', async () => {
    let body = null, state = 'trying';
    const p = await loadPage({ routes: {
      'GET /api/wifi': () => json(Object.assign({}, WIFI, state === 'trying' ? { state: 'trying', connected: false } : {})),
      'POST /api/wifi': req => { body = JSON.parse(req.body); return json({ ok: true }); } } });
    await openSetup(p, 400);
    p.$('#wifi-ssid').value = '  Home  '; p.$('#wifi-pass').value = 'password123';
    await p.click('#wifi-connect');
    t.eq(body, { ssid: 'Home', password: 'password123' }, 'name is trimmed, password sent as typed');
    t.eq(p.text('#wifi-status'), 'Testing "Home"...', 'status while testing');
    t.ok(p.$('#wifi-connect').disabled, 'button disabled while testing');
    state = 'connected';
    await p.tick(1600);
    t.ok(/Connected to Home/.test(p.text('#wifi-status')) || /Connected to HomeNet/.test(p.text('#wifi-status')), 'success reported: ' + p.text('#wifi-status'));
    t.eq(p.$('#wifi-pass').value, '', 'the password is cleared from the box');
    t.ok(!p.$('#wifi-connect').disabled, 'button usable again');
    p.close();
  });

  await t.test('Wi-Fi: a refusal or an unreachable board is reported and the button comes back', async () => {
    const p = await loadPage({ routes: { 'GET /api/wifi': () => json(WIFI), 'POST /api/wifi': () => json({ error: 'bad ssid' }, 400) } });
    await openSetup(p, 400);
    p.$('#wifi-ssid').value = 'Home';
    await p.click('#wifi-connect');
    t.eq(p.text('#wifi-status'), 'bad ssid', 'board message');
    t.ok(!p.$('#wifi-connect').disabled, 'button usable');
    p.close();
    const q = await loadPage({ routes: { 'GET /api/wifi': () => json(WIFI), 'POST /api/wifi': () => null } });
    await openSetup(q, 400);
    q.$('#wifi-ssid').value = 'Home';
    await q.click('#wifi-connect');
    t.eq(q.text('#wifi-status'), 'board not reachable', 'no answer');
    q.close();
  });

  await t.test('Wi-Fi: Forget asks first, then tells the board', async () => {
    const p = await loadPage({ routes: { 'GET /api/wifi': () => json(WIFI), 'POST /api/wifi/forget': () => json({ ok: true }) } });
    await openSetup(p, 400);
    p.window.confirm = () => false;
    await p.click('#wifi-forget');
    t.eq(p.to('/api/wifi/forget').length, 0, 'declining the question sends nothing');
    p.window.confirm = () => true;
    await p.click('#wifi-forget');
    t.eq(p.to('/api/wifi/forget', 'POST').length, 1, 'confirming sends it');
    t.ok(/Forgotten/.test(p.text('#wifi-status')), 'status');
    p.close();
  });

  await t.test('Wi-Fi: show-password checkbox toggles the field', async () => {
    const p = await loadPage();
    const cb = p.$('#wifi-show');
    cb.checked = true; cb.dispatchEvent(new p.window.Event('change', { bubbles: true }));
    t.eq(p.$('#wifi-pass').type, 'text', 'visible');
    cb.checked = false; cb.dispatchEvent(new p.window.Event('change', { bubbles: true }));
    t.eq(p.$('#wifi-pass').type, 'password', 'hidden again');
    p.close();
  });

  // ---------------------------------------------------------------- diagnostics
  const diag = p => [...p.document.querySelectorAll('#diag-rows > div')].map(d => [d.children[0].textContent, d.children[1].textContent, d.children[1].style.color]);

  await t.test('Diagnostics: uptime, restart reason, memory, signal and refused connections', async () => {
    const p = await loadPage({ routes: { 'GET /api/status': () => json({ uptimeS: 90061, resetReason: 'power on', abnormalReset: false,
      freeHeap: 204800, minFreeHeap: 153600, wifiConnected: true, rssi: -65, refused: 3 }) } });
    await openSetup(p, 500);
    const rows = Object.fromEntries(diag(p).map(r => [r[0], r[1]]));
    t.eq(rows['uptime'], '1 d 1 h 1 min', 'uptime in days/hours/minutes');
    t.eq(rows['last restart'], 'power on', 'restart reason');
    t.eq(rows['free memory'], '200 kB (lowest 150 kB)', 'memory in kB');
    t.eq(rows['Wi-Fi signal'], '-65 dBm (fair)', 'signal with the same wording as the Wi-Fi panel');
    t.eq(rows['refused connections'], '3 since boot', 'refused connections');
    p.close();
  });

  await t.test('Diagnostics: short uptimes, abnormal restarts in red, and no Wi-Fi', async () => {
    const p = await loadPage({ routes: { 'GET /api/status': () => json({ uptimeS: 45, resetReason: 'panic', abnormalReset: true,
      freeHeap: 1024, minFreeHeap: 1024, wifiConnected: false, rssi: 0, refused: 0 }) } });
    await openSetup(p, 500);
    const rows = Object.fromEntries(diag(p).map(r => [r[0], r]));
    t.eq(rows['uptime'][1], '45 s', 'under a minute is shown in seconds');
    t.ok(/err/.test(rows['last restart'][2]), 'a crash restart is red');
    t.ok(!/err/.test(rows['uptime'][2]), 'normal rows are not');
    t.eq(rows['Wi-Fi signal'][1], 'not connected', 'no Wi-Fi');
    p.close();
    for (const [secs, want] of [[3700, '1 h 1 min'], [59, '59 s'], [60, '1 min'], [86400, '1 d 0 h 0 min']]) {
      const q = await loadPage({ routes: { 'GET /api/status': () => json({ uptimeS: secs, resetReason: 'x', abnormalReset: false, freeHeap: 1, minFreeHeap: 1, wifiConnected: false, refused: 0 }) } });
      await openSetup(q, 500);
      t.eq(diag(q)[0][1], want, secs + ' s shown as ' + want);
      q.close();
    }
  });

  await t.test('Diagnostics: a hostile restart reason is shown as text; an unreachable board is reported', async () => {
    const evil = '<script>window.hacked=1</script>';
    const p = await loadPage({ routes: { 'GET /api/status': () => json({ uptimeS: 1, resetReason: evil, abnormalReset: false, freeHeap: 1, minFreeHeap: 1, wifiConnected: false, refused: 0 }) } });
    await openSetup(p, 500);
    t.eq(p.$('#diag-rows').querySelector('script'), null, 'no script element created');
    t.eq(p.window.hacked, undefined, 'and nothing ran');
    p.close();
    const q = await loadPage({ routes: { 'GET /api/status': () => null } });
    await openSetup(q, 500);
    t.eq(q.text('#diag-rows'), 'board not reachable', 'message');
    q.close();
  });

  await t.test('Diagnostics: the board is not polled while another tab is showing', async () => {
    const p = await loadPage({ routes: { 'GET /api/status': () => json({ uptimeS: 1, resetReason: 'x', abnormalReset: false, freeHeap: 1, minFreeHeap: 1, wifiConnected: false, refused: 0 }) } });
    p.forgetCalls();
    await p.tick(10000);
    t.eq(p.to('/api/status').length, 0, 'no /api/status requests from the Home tab (it would load the board for nothing)');
    p.close();
  });

  t.done();
})();
