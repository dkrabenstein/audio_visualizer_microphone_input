// Builds a small C++ helper with g++ for the differential tests.
//
// On Windows, the first run of an .exe Windows Defender has not seen before is held up by a scan of
// about 25 seconds. A normal link puts a timestamp in the file, so every rebuild is "new" and pays that
// again. --no-insert-timestamp makes the output byte-identical for identical sources, so only the first
// run after a source change is slow; later runs hit the scan cache and start in about 0.2 s.
const { spawnSync } = require('child_process');

function buildCpp(sources, out, includeDir) {
  const args = ['-std=gnu++17', '-O1', '-I', includeDir, ...sources, '-o', out];
  if (process.platform === 'win32') args.push('-Wl,--no-insert-timestamp');   // PE-only option; GNU ld for ELF rejects it
  // True 32-bit float arithmetic, like the ESP32. A 32-bit MinGW defaults to x87 (80-bit intermediates), whose
  // results differ from the board in the last bits. Harmless on x86-64 Linux, where SSE is already the default.
  if (process.platform === 'win32') args.push('-msse2', '-mfpmath=sse');
  const r = spawnSync('g++', args, { encoding: 'utf8' });
  return { ok: !r.error && r.status === 0, message: r.error ? r.error.message : r.stderr };
}

module.exports = { buildCpp };
