// Loads the browser scripts in data/www/ for the tests, without changing them.
//
// They are plain <script> files (top-level const/function, no module.exports), so each one is run in
// a vm context and the names the tests need are returned. `names` is a list of top-level identifiers.
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const WWW = path.join(__dirname, '..', '..', 'data', 'www');

function loadScript(file, names, sandbox) {
  const full = path.join(WWW, file);
  const src = fs.readFileSync(full, 'utf8');
  return vm.runInNewContext(src + '\n;({ ' + names.join(', ') + ' });', sandbox || {}, { filename: full });
}

module.exports = { WWW, loadScript };

// Returns the source text of `function name(...) { ... }` that starts at the beginning of a line in
// `src` (a .html or .cpp file), found by matching braces. Lets a test run one function out of a big
// file without loading the rest of it. `prefix` is what precedes the name, e.g. 'function ' or
// 'static uint32_t '.
function extractFunction(src, name, prefix) {
  const start = src.search(new RegExp('^' + (prefix || 'function ') + name + '\\s*\\(', 'm'));
  if (start < 0) throw new Error('function ' + name + ' not found');
  const open = src.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < src.length; i++) {
    if (src[i] === '{') depth++;
    else if (src[i] === '}' && --depth === 0) return src.slice(start, i + 1);
  }
  throw new Error('unbalanced braces in ' + name);
}

module.exports.extractFunction = extractFunction;
