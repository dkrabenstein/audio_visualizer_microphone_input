// jsdom tests: the Patterns, Basic (colour wheel, white light) and Home tabs of index.html, and the
// power / brightness controls they share.
const { loadPage, json } = require('./dom_harness.js');
const { loadScript } = require('./load_web.js');
const { compile, evalCode } = require('./load_compiler.js');
const { suite } = require('./testkit.js');
const t = suite('page tabs');

const { PRESETS, BASIC_SLOT, ADVANCED_SLOT } = loadScript('presets.js', ['PRESETS', 'BASIC_SLOT', 'ADVANCED_SLOT']);
const { rainbowRGB, fastledCss } = loadScript('fastled.js', ['rainbowRGB', 'fastledCss']);

const mouse = (p, type, x, y) => p.window.MouseEvent && new p.window.MouseEvent(type, { clientX: x, clientY: y, bubbles: true });

(async () => {
  // ---------------------------------------------------------------- Patterns tab
  await t.test('Patterns: one card per preset, with the right slot and a preview that matches the lights', async () => {
    const p = await loadPage();
    const cards = [...p.document.querySelectorAll('#cards .card')];
    t.eq(cards.map(c => c.dataset.slot), PRESETS.map((_, n) => String(n)), 'cards are in slot order');
    t.ok(cards.every(c => c.querySelector('.title').textContent.length > 0 && c.querySelector('.blurb').textContent.length > 0), 'each card has a title and a description');
    t.eq(cards[0].querySelector('.title').textContent, 'Audio Reactive', 'friendly title for audio_reactive');

    // The preview is the preset evaluated at a fixed moment of music, colour by colour, as the lights would show it.
    const bands = [0.8, 0.6, 0.5, 0.4, 0.3, 0.35, 0.25, 0.15];
    const sample = { t: 3, i: 0, bands, bass: (bands[0] + bands[1]) / 2, mid: (bands[2] + bands[3] + bands[4]) / 3,
                     treb: (bands[5] + bands[6] + bands[7]) / 3, level: bands.reduce((a, b) => a + b) / 8 };
    let allMatch = true;
    PRESETS.forEach((preset, n) => {
      const rects = cards[n].querySelector('canvas').getContext('2d').rects;
      const strips = rects.slice(1);                      // the first rect is the black background
      const code = compile(preset.expr);
      if (strips.length !== 120) allMatch = false;
      strips.forEach((r, k) => {
        sample.x = k / 119;
        const v = evalCode(code, sample);
        if (r.style.replace(/\s/g, '') !== fastledCss(v - Math.floor(v)).replace(/\s/g, '')) allMatch = false;
      });
    });
    t.ok(allMatch, 'all 7 previews are 120 colour steps identical to compiler.js + fastled.js output');
    p.close();
  });

  await t.test('Patterns: clicking a card selects that slot and marks it active', async () => {
    const p = await loadPage();
    p.forgetCalls();
    await p.click('#cards .card:nth-child(3)');
    t.eq(p.to('/api/select', 'POST')[0].body, 'slot=2', 'slot 2 is selected');
    t.eq(p.text('#pat-status'), '✓ now showing: Spectrum Hue Shift', 'status names the pattern');
    t.eq(p.$('#pat-status').className, 'ok', 'styled as ok');
    t.eq([...p.document.querySelectorAll('#cards .card.active')].map(c => c.dataset.slot), ['2'], 'only that card is marked');
    p.close();
  });

  await t.test('Patterns: after a board reboot ("invalid slot") the presets are re-sent and the choice retried once', async () => {
    let selects = 0;
    const p = await loadPage({ routes: { 'POST /api/select': () => (++selects === 1 ? json({ error: 'invalid slot' }, 400) : json({ ok: true })) } });
    p.forgetCalls();
    await p.click('#cards .card:nth-child(2)');
    await p.flush(20);
    t.eq(p.to('/api/select', 'POST').length, 2, 'selected twice');
    t.eq(p.text('#pat-status'), '✓ now showing: Dominant Frequency', 'and it worked the second time');
    p.close();
  });

  await t.test('Patterns: another failure is reported, not retried', async () => {
    const p = await loadPage({ routes: { 'POST /api/select': () => json({ error: 'busy' }, 500) } });
    p.forgetCalls();
    await p.click('#cards .card:nth-child(1)');
    t.eq(p.to('/api/select', 'POST').length, 1, 'only one attempt');
    t.eq(p.text('#pat-status'), '✗ could not switch pattern: busy', 'error shown');
    t.eq(p.$('#pat-status').className, 'err', 'styled as an error');
    p.close();
  });

  await t.test('Patterns: the card for the pattern the board is running is highlighted', async () => {
    const p = await loadPage();
    await p.dispatch('devstate', { active: 4 });
    t.eq([...p.document.querySelectorAll('#cards .card.active')].map(c => c.dataset.slot), ['4'], 'slot 4 highlighted');
    await p.dispatch('devstate', { active: 1 });
    t.eq([...p.document.querySelectorAll('#cards .card.active')].map(c => c.dataset.slot), ['1'], 'and it moves');
    p.close();
  });

  // ---------------------------------------------------------------- Basic tab: colour wheel
  await t.test('Basic: the colour wheel is a ring of rainbow colours with a transparent middle', async () => {
    const p = await loadPage();
    const img = p.canvas('wheel').lastImage;
    const size = img.width, at = (x, y) => { const k = (y * size + x) * 4; return [...img.data.slice(k, k + 4)]; };
    t.eq(at(size >> 1, size >> 1)[3], 0, 'the centre is transparent');
    t.eq(at(2, 2)[3], 0, 'the corner is transparent');
    const top = at(size >> 1, Math.round(size * 0.5 * 0.16));   // just inside the outer edge, straight up
    t.eq(top[3], 255, 'the ring is opaque');
    t.eq(top.slice(0, 3), rainbowRGB(0), 'straight up is hue 0 (red), like the lights');
    const right = at(Math.round(size * (0.5 + 0.42)), size >> 1);   // a quarter turn clockwise
    t.eq(right.slice(0, 3), rainbowRGB(63), 'a quarter turn round is hue 63');
    p.close();
  });

  await t.test('Basic: tapping the ring sends that hue as a constant pattern to the Basic slot, then selects it', async () => {
    const p = await loadPage();
    p.forgetCalls();
    const wheel = p.$('#wheel');
    wheel.dispatchEvent(mouse(p, 'pointerdown', 140, 140 - 0.85 * 140));   // straight up, on the ring
    await p.flush();
    const up = p.to('/api/pattern', 'POST')[0];
    t.ok(up, 'a pattern was uploaded');
    const body = JSON.parse(up.body);
    t.eq([body.slot, body.name], [BASIC_SLOT, 'basic'], 'to the Basic slot');
    t.eq(body.code, compile(String((0 + 0.5) / 255)), 'the program is the constant (hue+0.5)/255');
    t.eq(p.to('/api/select', 'POST')[0].body, 'slot=' + BASIC_SLOT, 'and then selected');
    t.eq(p.text('#basic-status'), '✓ lights set to this colour', 'status says so');
    t.eq(p.window.basicHue, 0, 'the page remembers the hue');
    t.eq(p.storage('viz.basicHue'), '0', 'and stores it for next time');
    t.eq(p.$('#wheel-swatch').style.background.replace(/\s/g, ''), 'rgb(' + rainbowRGB(0).join(',') + ')', 'the swatch shows the colour');
    p.close();
  });

  await t.test('Basic: taps off the ring (middle, outside) do nothing', async () => {
    const p = await loadPage();
    p.forgetCalls();
    p.$('#wheel').dispatchEvent(mouse(p, 'pointerdown', 140, 140));
    p.$('#wheel').dispatchEvent(mouse(p, 'pointerdown', 1, 1));
    await p.flush();
    t.eq(p.calls.length, 0, 'no requests');
    p.close();
  });

  await t.test('Basic: a right-hand tap is hue 63; dragging sends only the latest colour while one is in flight', async () => {
    let release;
    const gate = new Promise(r => { release = r; });
    const p = await loadPage({ routes: { 'POST /api/pattern': async () => { await gate; return json({ ok: true }); } } });
    p.forgetCalls();
    const wheel = p.$('#wheel');
    wheel.dispatchEvent(mouse(p, 'pointerdown', 140 + 0.85 * 140, 140));          // hue 63
    wheel.dispatchEvent(mouse(p, 'pointermove', 140 + 0.85 * 140 * 0.7, 140 + 0.85 * 140 * 0.7));   // 135 degrees: later colours while waiting
    wheel.dispatchEvent(mouse(p, 'pointermove', 140, 140 + 0.85 * 140));          // straight down: the newest
    await p.flush();
    t.eq(p.to('/api/pattern', 'POST').length, 1, 'only one request in flight while waiting');
    release();
    await p.flush(20);
    const sent = p.to('/api/pattern', 'POST').map(c => JSON.parse(c.body).code[0][1]);
    t.eq(sent.length, 2, 'then exactly one more, for the newest colour (the middle one was dropped)');
    t.eq(sent[0], (63 + 0.5) / 255, 'first request is hue 63');
    t.eq(sent[1], (127 + 0.5) / 255, 'second is the straight-down hue (127), the latest');
    wheel.dispatchEvent(mouse(p, 'pointerup', 0, 0));
    p.forgetCalls();
    wheel.dispatchEvent(mouse(p, 'pointermove', 140, 140 - 0.85 * 140));
    await p.flush();
    t.eq(p.calls.length, 0, 'after the finger lifts, moving does nothing');
    p.close();
  });

  await t.test('Basic: a board error is reported', async () => {
    const p = await loadPage({ routes: { 'POST /api/pattern': () => json({ error: 'full' }, 400) } });
    p.$('#wheel').dispatchEvent(mouse(p, 'pointerdown', 140, 140 - 0.85 * 140));
    await p.flush();
    t.eq(p.text('#basic-status'), '✗ could not reach the lights: full', 'message');
    t.eq(p.$('#basic-status').className, 'err', 'styled as an error');
    p.close();
  });

  // ---------------------------------------------------------------- Basic tab: white light
  const slider = '#white-temp';
  await t.test('White: moving the slider sends the temperature and updates the swatch and readout', async () => {
    const p = await loadPage({ routes: { 'POST /api/white': () => json({ ok: true }) } });
    p.forgetCalls();
    await p.type(slider, '3000');
    t.eq(p.to('/api/white', 'POST')[0].body, 'kelvin=3000', 'kelvin sent');
    t.eq(p.text('#white-readout'), 'Warm white · 3000 K', 'readout');
    t.eq(p.text('#white-status'), '✓ lights set to warm white (3000 K)', 'status');
    await p.type(slider, '6000');
    t.eq(p.text('#white-readout'), 'Cool white · 6000 K', 'readout follows');
    p.close();
  });

  await t.test('White: the same temperature is not sent twice, and a board without white support says so', async () => {
    const p = await loadPage({ routes: { 'POST /api/white': () => json({ ok: true }) } });
    await p.type(slider, '4000');
    p.forgetCalls();
    await p.type(slider, '4000');
    t.eq(p.to('/api/white').length, 0, 'unchanged value is not resent');
    p.close();
    const q = await loadPage();   // default routes have no /api/white: the page gets a 404
    await q.type(slider, '3000');
    t.eq(q.text('#white-status'), '✗ could not set white: the board needs a firmware update for white', 'a 404 means old firmware');
    t.ok(/white is off/.test(q.text('#white-readout')), 'the readout goes back to "off"');
    q.close();
  });

  await t.test('White: the slider follows what the board is really showing', async () => {
    const p = await loadPage();
    await p.dispatch('devstate', { active: 8, white: 5600 });
    t.eq(p.$(slider).value, '5600', 'slider moves to 5600');
    t.eq(p.text('#white-readout'), 'Cool white · 5600 K', 'readout');
    await p.dispatch('devstate', { active: 0, white: null });
    t.ok(/white is off/.test(p.text('#white-readout')), 'choosing a pattern switches white off in the page too');
    p.close();
  });

  // ---------------------------------------------------------------- Home tab
  await t.test('Home: the "now playing" card names what the board is running', async () => {
    const p = await loadPage();
    const name = () => p.text('#now-name');
    await p.dispatch('devstate', { active: 0 });
    t.eq(name(), 'Audio Reactive', 'a preset');
    await p.dispatch('devstate', { active: 6 });
    t.eq(name(), 'Bass Bloom', 'another preset');
    await p.dispatch('devstate', { active: ADVANCED_SLOT });
    t.eq(name(), 'Custom formula', 'the Custom slot');
    t.eq(p.$('#nowcard').dataset.go, 'custom', 'clicking goes to the Custom tab');
    await p.dispatch('devstate', { active: BASIC_SLOT });
    t.eq(name(), 'Single colour', 'the Basic slot');
    await p.dispatch('devstate', { active: BASIC_SLOT, white: 2700 });
    t.eq(name(), 'Warm white (2700 K)', 'white light');
    t.eq(p.$('#nowcard').dataset.go, 'basic', 'clicking goes to the Basic tab');
    t.eq(p.$('#now-dot').className, 'dot on', 'green dot while online');
    await p.dispatch('devoffline');
    t.eq(name(), 'Lights not reachable', 'offline text');
    t.eq(p.$('#now-dot').className, 'dot off', 'and a red dot');
    p.close();
  });

  await t.test('Home: the live preview draws colours from the running pattern', async () => {
    const p = await loadPage();
    await p.dispatch('devstate', { active: 0 });
    await p.tick(100);      // a few animation frames
    const strip = p.canvas('now-strip').rects;
    t.ok(strip.length >= 80, 'the strip was painted (' + strip.length + ' rects)');
    t.ok(strip.every(r => /^rgb\(\d+, ?\d+, ?\d+\)$/.test(r.style) || r.style === '#000000' || r.style === '#000'), 'every fill is a valid colour');
    p.close();
  });

  // ---------------------------------------------------------------- power and brightness
  await t.test('Power: every switch stays in step, and the board is told', async () => {
    const p = await loadPage();
    const ids = ['pwr', 'pwr-basic', 'pwr-pat', 'pwr-adv'];
    t.ok(ids.every(id => p.$('#' + id).getAttribute('aria-pressed') === 'true' && p.$('#' + id).textContent === 'Lights on'), 'all show "Lights on" (the board reported on)');
    p.forgetCalls();
    await p.click('#pwr-pat');
    t.ok(ids.every(id => p.$('#' + id).getAttribute('aria-pressed') === 'false' && p.$('#' + id).textContent === 'Lights off'), 'all four flip together');
    t.eq(p.to('/api/power', 'POST')[0].body, 'on=0', 'the board is told off');
    await p.click('#pwr');
    t.eq(p.to('/api/power', 'POST').map(c => c.body), ['on=0', 'on=1'], 'and on again');
    p.close();
  });

  await t.test('Power: while one request is in flight, only the newest state is sent', async () => {
    let release;
    const gate = new Promise(r => { release = r; });
    const p = await loadPage({ routes: { 'POST /api/power': async () => { await gate; return json({ ok: true }); } } });
    p.forgetCalls();
    await p.click('#pwr');     // off
    await p.click('#pwr');     // on
    await p.click('#pwr');     // off
    await p.click('#pwr');     // on (newest)
    t.eq(p.to('/api/power').length, 1, 'one request while waiting');
    release();
    await p.flush(20);
    t.eq(p.to('/api/power').map(c => c.body), ['on=0', 'on=1'], 'then only the latest state');
    p.close();
  });

  await t.test('Brightness: all sliders move together, the board gets the value, the latest wins', async () => {
    let release;
    const gate = new Promise(r => { release = r; });
    const p = await loadPage({ routes: { 'POST /api/brightness': async () => { await gate; return json({ ok: true }); } } });
    t.eq(p.$('#home-bri').value, '80', 'sliders show the board\'s brightness (80) after load');
    t.eq(p.text('#bri-val'), '80%', 'and their labels');
    p.forgetCalls();
    await p.type('#home-bri', '30');
    await p.type('#pat-bri', '45');
    await p.type('#adv-bri', '60');
    t.ok(['bri', 'home-bri', 'pat-bri', 'adv-bri'].every(id => p.$('#' + id).value === '60' && p.text('#' + id + '-val') === '60%'), 'all four sliders and labels show 60%');
    release();
    await p.flush(20);
    t.eq(p.to('/api/brightness').map(c => c.body), ['value=30', 'value=60'], 'first value, then only the newest');
    p.close();
  });

  t.done();
})();
