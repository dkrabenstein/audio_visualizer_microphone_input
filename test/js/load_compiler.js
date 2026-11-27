// Loads data/www/compiler.js for the tests.
//
// compiler.js is a plain browser script (it defines globals and has no module.exports), so it is
// evaluated in a vm context instead of being require()d. That way the web file stays exactly as the
// browser loads it, and the tests exercise the real thing.
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const file = path.join(__dirname, '..', '..', 'data', 'www', 'compiler.js');
const src = fs.readFileSync(file, 'utf8');

// The trailing expression is the script's completion value, which runInNewContext returns.
module.exports = vm.runInNewContext(
  src + '\n;({ compile, evalCode, CompileError, toPayload, OP, ABI, MAX_OPS, STACK_MAX, sSin, sCos });',
  {}, { filename: file });
