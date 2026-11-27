// Runs every JS test: node test/js/run_all.js   (or: npm test)
//
// The files are independent (each starts its own server / fake browser on its own ports and temp
// folders), so they run side by side. Output is collected per file and printed in the list's order so
// the log reads the same every time. Exit code is 1 if any file failed.
const { spawn } = require('child_process');
const os = require('os');
const path = require('path');

const FILES = ['test_compiler.js', 'test_differential.js', 'test_firmware_sync.js', 'test_presets.js',
               'test_fastled.js', 'test_index_html.js', 'test_server.js',
               'test_page_token.js', 'test_page_core.js', 'test_page_tabs.js', 'test_page_setup.js'];
const PARALLEL = Math.max(1, Math.min(FILES.length, os.cpus().length - 1));

function runOne(file) {
  return new Promise(resolve => {
    const child = spawn(process.execPath, [path.join(__dirname, file)], { stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    child.stdout.on('data', d => { out += d; });
    child.stderr.on('data', d => { out += d; });
    child.on('close', code => resolve({ file, code, out }));
  });
}

(async () => {
  const wall = Date.now();
  const results = new Array(FILES.length);
  let next = 0;
  await Promise.all(Array.from({ length: PARALLEL }, async () => {
    while (next < FILES.length) { const k = next++; results[k] = await runOne(FILES[k]); }
  }));
  let failed = 0;
  for (const r of results) {
    process.stdout.write(r.out.endsWith('\n') || !r.out ? r.out : r.out + '\n');
    if (r.code !== 0) { failed++; console.log('  ^ ' + r.file + ' FAILED (exit ' + r.code + ')'); }
  }
  console.log('\n' + (FILES.length - failed) + '/' + FILES.length + ' files passed in ' + ((Date.now() - wall) / 1000).toFixed(1) + ' s (' + PARALLEL + ' at a time)');
  process.exit(failed ? 1 : 0);
})();
