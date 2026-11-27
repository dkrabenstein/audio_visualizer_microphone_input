// jsdom tests: the API token handling in index.html.
// The board only accepts changes (POSTs) that carry "X-Api-Token". The page wraps fetch() to add the saved
// token, to ask for it in a dialog on a 401, and to pair from a #token=... link. Run against the real page.
const { loadPage, json } = require('./dom_harness.js');
const { suite } = require('./testkit.js');
const t = suite('page token');

const KEY = 'viz_api_token';
// A route that accepts exactly one token.
const guarded = good => req => (req.headers['x-api-token'] === good ? json({ ok: true }) : json({ error: 'missing or wrong API token' }, 401));
const dialog = p => p.document.getElementById('tok-dlg');
const dialogOpen = p => !!dialog(p) && dialog(p).hasAttribute('open');
// Type into the open dialog and press Save (ok) or Cancel, like a person would.
async function answer(p, value, how = 'ok') {
  dialog(p).querySelector('#tok-in').value = value;
  dialog(p).close(how);
  await p.flush();
}
const post = (p, url = '/api/thing', init = {}) => p.window.fetch(url, Object.assign({ method: 'POST' }, init));

(async () => {
  await t.test('the page loads without script errors, in both modes', async () => {
    for (const laptop of [false, true]) {
      const p = await loadPage({ laptop });
      t.eq(p.errors, [], 'no script errors on load (laptop=' + laptop + ')');
      p.close();
    }
  });

  await t.test('no saved token: a POST goes out without the header, and no dialog appears when the board accepts it', async () => {
    const p = await loadPage({ routes: { 'POST /api/thing': () => json({ ok: true }) } });
    p.forgetCalls();
    const r = await post(p);
    t.eq(r.status, 200, 'request goes through');
    t.eq(p.to('/api/thing', 'POST')[0].headers['x-api-token'], undefined, 'no header when there is no saved token');
    t.ok(!dialogOpen(p), 'no dialog');
    p.close();
  });

  await t.test('saved token is added to POSTs but never to GETs', async () => {
    const p = await loadPage({ storage: { [KEY]: 'secret123' }, routes: { 'POST /api/thing': guarded('secret123'), 'GET /api/thing': () => json({ ok: true }) } });
    p.forgetCalls();
    const r = await post(p);
    t.eq(r.status, 200, 'POST with the saved token is accepted');
    t.eq(p.to('/api/thing', 'POST')[0].headers['x-api-token'], 'secret123', 'POST carries X-Api-Token');
    await p.window.fetch('/api/thing');
    t.eq(p.to('/api/thing', 'GET')[0].headers['x-api-token'], undefined, 'GET does not carry the token');
    p.close();
  });

  await t.test('a 401 raises the dialog; Save retries with the token and remembers it', async () => {
    const p = await loadPage({ routes: { 'POST /api/thing': guarded('good') } });
    p.forgetCalls();
    const pending = post(p, '/api/thing', { signal: new p.window.AbortController().signal });
    await p.flush();
    t.ok(dialogOpen(p), 'dialog is shown after a 401');
    t.eq(dialog(p).querySelector('#tok-note').textContent, '', 'no complaint the first time');
    t.eq(dialog(p).querySelector('#tok-in').type, 'password', 'token field is hidden by default');
    await answer(p, '  good  ');
    const r = await pending;
    t.eq(r.status, 200, 'the retried request succeeds');
    const posts = p.to('/api/thing', 'POST');
    t.eq(posts.length, 2, 'exactly one retry');
    t.eq(posts[1].headers['x-api-token'], 'good', 'retry carries the entered token, trimmed');
    t.eq(posts[0].hasSignal && !posts[1].hasSignal, true, 'the caller\'s short-timeout abort signal is not reused on the retry');
    t.eq(p.storage(KEY), 'good', 'an accepted token is saved');
    t.ok(!dialogOpen(p), 'dialog closed');
    // and the next request needs no dialog
    p.forgetCalls();
    t.eq((await post(p)).status, 200, 'the next POST just works');
    t.eq(p.to('/api/thing').length, 1, 'with no retry');
    p.close();
  });

  await t.test('Cancel hands the 401 back to the caller and saves nothing', async () => {
    const p = await loadPage({ routes: { 'POST /api/thing': guarded('good') } });
    p.forgetCalls();
    const pending = post(p);
    await p.flush();
    await answer(p, 'whatever', 'cancel');
    t.eq((await pending).status, 401, 'caller sees the 401');
    t.eq(p.to('/api/thing').length, 1, 'no retry after cancel');
    t.eq(p.storage(KEY), null, 'nothing saved');
    p.close();
  });

  await t.test('a wrong token is not saved; the dialog asks again, up to three times', async () => {
    const p = await loadPage({ routes: { 'POST /api/thing': guarded('good') } });
    p.forgetCalls();
    const pending = post(p);
    const notes = [];
    for (let i = 0; i < 3; i++) {
      await p.flush();
      t.ok(dialogOpen(p), 'dialog is open for attempt ' + (i + 1));
      notes.push(dialog(p).querySelector('#tok-note').textContent);
      await answer(p, 'bad' + i);
    }
    t.eq((await pending).status, 401, 'after three wrong tries the caller sees the 401');
    t.eq(notes[0], '', 'first ask has no complaint');
    t.ok(/not accepted/i.test(notes[1]) && /not accepted/i.test(notes[2]), 'later asks say the token was not accepted');
    t.eq(p.storage(KEY), null, 'a rejected token is never kept');
    t.eq(p.to('/api/thing').length, 4, '1 original + 3 retries');
    p.close();
  });

  await t.test('a saved token the board no longer accepts is dropped and the person is asked again', async () => {
    const p = await loadPage({ storage: { [KEY]: 'old' }, routes: { 'POST /api/thing': guarded('new') } });
    const pending = post(p);
    await p.flush();
    t.eq(p.storage(KEY), null, 'the refused saved token is removed');
    t.ok(dialogOpen(p) && /saved token was not accepted/i.test(dialog(p).querySelector('#tok-note').textContent), 'dialog explains the saved token failed');
    await answer(p, 'new');
    t.eq((await pending).status, 200, 'works with the new token');
    t.eq(p.storage(KEY), 'new', 'new token saved');
    p.close();
  });

  await t.test('only one dialog at a time when several requests fail together', async () => {
    const p = await loadPage({ routes: { 'POST /api/a': guarded('good'), 'POST /api/b': guarded('good') } });
    const r1 = post(p, '/api/a'), r2 = post(p, '/api/b');
    await p.flush();
    t.eq(p.document.querySelectorAll('#tok-dlg').length, 1, 'a single dialog element');
    await answer(p, 'good');
    t.eq([(await r1).status, (await r2).status], [200, 200], 'both requests succeed after one answer');
    p.close();
  });

  await t.test('background requests never raise the dialog', async () => {
    const p = await loadPage({ routes: { 'POST /api/thing': guarded('good') } });
    t.eq((await post(p, '/api/thing', { background: true })).status, 401, 'the 401 is returned as is');
    t.ok(!dialogOpen(p), 'no dialog');
    p.close();
    const q = await loadPage({ storage: { [KEY]: 'stale' }, routes: { 'POST /api/thing': guarded('good') } });
    await post(q, '/api/thing', { background: true });
    t.eq(q.storage(KEY), null, 'a refused saved token is still cleared');
    t.ok(!dialogOpen(q), 'and still no dialog');
    q.close();
  });

  await t.test('the show-token checkbox reveals the field', async () => {
    const p = await loadPage({ routes: { 'POST /api/thing': guarded('good') } });
    const pending = post(p);
    await p.flush();
    const cb = dialog(p).querySelector('#tok-show');
    cb.checked = true; cb.dispatchEvent(new p.window.Event('change', { bubbles: true }));
    t.eq(dialog(p).querySelector('#tok-in').type, 'text', 'field shows the characters');
    await answer(p, 'x', 'cancel');
    await pending;
    p.close();
  });

  await t.test('laptop page: only requests for the board get the token, not the laptop server', async () => {
    const p = await loadPage({ laptop: true, storage: { [KEY]: 'tok' },
      routes: { 'GET /api/config': () => json({ host: 'visualizer-esp-32.local', ip: '10.0.0.5', source: 'mdns' }), 'POST /api/config': () => json({ ok: true }), 'POST /api/pattern': () => json({ ok: true }) } });
    p.forgetCalls();
    await p.window.fetch('/api/config', { method: 'POST', body: '{}' });
    await p.window.fetch('http://10.0.0.5/api/pattern', { method: 'POST', body: '{}' });
    t.eq(p.to('/api/config', 'POST')[0].headers['x-api-token'], undefined, 'POST to the laptop server carries no token');
    t.eq(p.to('/api/pattern', 'POST')[0].headers['x-api-token'], 'tok', 'POST to the board carries the token');
    p.close();
  });

  // ---- pairing link: http://board/#token=...
  // The pairing messages are <div>s the script adds straight to <body> with position:fixed.
  const toasts = p => [...p.document.querySelectorAll('body > div')].filter(d => /position:\s*fixed/.test(d.getAttribute('style') || '')).map(d => d.textContent).join(' | ');

  await t.test('pairing link: saves the token, clears the address bar, asks the board, confirms', async () => {
    const p = await loadPage({ url: 'http://board.test/#token=abc123', routes: { 'POST /api/auth': req => (req.headers['x-api-token'] === 'abc123' ? json({ ok: true }) : json({}, 401)) } });
    t.eq(p.storage(KEY), 'abc123', 'token saved');
    t.ok(!/token/.test(p.window.location.hash) && !/abc123/.test(p.window.location.href), 'token removed from the address bar');
    t.eq(p.to('/api/auth', 'POST')[0].headers['x-api-token'], 'abc123', 'the board is asked to check it');
    t.ok(/Paired\./.test(toasts(p)), 'success message shown');
    await p.tick(7000);
    t.ok(!/Paired\./.test(toasts(p)), 'the message goes away after 7 s');
    p.close();
  });

  await t.test('pairing link: a token the board refuses is taken back out', async () => {
    const p = await loadPage({ url: 'http://board.test/#token=wrong', routes: { 'POST /api/auth': () => json({}, 401) } });
    t.eq(p.storage(KEY), null, 'refused token removed');
    t.ok(/did not accept the token/.test(toasts(p)), 'error message shown');
    p.close();
  });

  await t.test('pairing link: other board answers', async () => {
    const e = await loadPage({ url: 'http://board.test/#token=abc', routes: { 'POST /api/auth': () => json({}, 500) } });
    t.ok(/answered with an error \(500\)/.test(toasts(e)), 'a 500 is reported');
    t.eq(e.storage(KEY), 'abc', 'the token is kept (not a refusal)');
    e.close();
    const n = await loadPage({ url: 'http://board.test/#token=abc', routes: { 'POST /api/auth': () => null } });
    t.ok(/did not answer just now/.test(toasts(n)), 'no answer is reported');
    t.eq(n.storage(KEY), 'abc', 'token kept; it is checked on the next change');
    n.close();
  });

  await t.test('pairing link: empty or badly encoded tokens are rejected without a request', async () => {
    for (const hash of ['#token=', '#token=%E0%A4%A']) {
      const p = await loadPage({ url: 'http://board.test/' + hash });
      t.ok(/no token in it/.test(toasts(p)), hash + ': message says there is no token');
      t.eq(p.storage(KEY), null, hash + ': nothing saved');
      t.eq(p.to('/api/auth').length, 0, hash + ': the board is not asked');
      p.close();
    }
  });

  await t.test('pairing link: other parts of the address survive, and no link means no message', async () => {
    const p = await loadPage({ url: 'http://board.test/#custom&token=zzz' });
    t.eq(p.storage(KEY), 'zzz', 'token found among other parameters');
    t.ok(p.document.getElementById('tab-custom').classList.contains('active'), 'the other part (#custom) still opens that tab');
    p.close();
    const q = await loadPage({ url: 'http://board.test/#patterns' });
    t.eq(q.to('/api/auth').length, 0, 'no /api/auth call without a token in the link');
    t.ok(!/Paired|Token saved|pairing link/.test(toasts(q)), 'no message');
    q.close();
  });

  await t.test('pairing link on the laptop page: saved, checked on first use', async () => {
    const p = await loadPage({ laptop: true, url: 'http://localhost:3000/#token=lap' });
    t.eq(p.storage(KEY), 'lap', 'token saved');
    t.eq(p.to('/api/auth').length, 0, 'the board\'s address is not known yet, so nothing is sent');
    t.ok(/Token saved in this browser/.test(toasts(p)), 'message shown');
    p.close();
  });

  t.done();
})();
