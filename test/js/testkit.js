// A tiny assertion helper shared by the page tests.
function suite(name) {
  let pass = 0, fail = 0, finished = false;
  // If a promise never settles, Node just runs out of work and exits with 0. That must not look like a pass.
  process.on('exit', () => {
    if (!finished) { console.log('FAIL: ' + name + ' ended before all tests finished (a promise never settled)'); process.exitCode = 1; }
  });
  const api = {
    ok(cond, msg) { if (cond) pass++; else { fail++; console.log('FAIL:', msg); } },
    eq(got, want, msg) {
      const same = JSON.stringify(got) === JSON.stringify(want);
      if (same) pass++; else { fail++; console.log('FAIL: ' + msg + ' (got ' + JSON.stringify(got) + ', want ' + JSON.stringify(want) + ')'); }
    },
    // Run an async test body; an exception counts as one failure instead of stopping the whole file.
    async test(label, body) {
      const started = Date.now();
      try { await body(); } catch (e) { fail++; console.log('FAIL: ' + label + ' threw ' + (e && e.stack ? e.stack.split('\n').slice(0, 3).join(' | ') : e)); }
      finally { if (process.env.TEST_TIMING) console.log('  ' + (Date.now() - started) + ' ms  ' + label); }
    },
    // For a file that cannot run in this setup (for example, it needs hardware in a particular state).
    skip(reason) {
      finished = true;
      console.log(name + ': SKIPPED, ' + reason);
      process.exit(0);
    },
    done() {
      finished = true;
      console.log(name + ': ' + pass + ' passed, ' + fail + ' failed');
      process.exit(fail ? 1 : 0);
    },
  };
  return api;
}
module.exports = { suite };
