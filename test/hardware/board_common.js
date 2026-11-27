// Shared by the on-board tests in this folder: where the board is, the API token, and a small HTTP client.
//
//   VIZ_BOARD  address of the board (default: the upload_port IP in platformio.ini)
//
// The token is read from include/secrets.h and is never printed.
const fs = require('fs');
const http = require('http');
const path = require('path');

const root = path.join(__dirname, '..', '..');
const ini = fs.readFileSync(path.join(root, 'platformio.ini'), 'utf8');
const HOST = process.env.VIZ_BOARD || (/upload_port\s*=\s*(\S+)/.exec(ini) || [])[1];
const secrets = fs.readFileSync(path.join(root, 'include', 'secrets.h'), 'utf8');
const TOKEN = (/#define\s+API_TOKEN\s+"([^"]+)"/.exec(secrets) || [])[1];
const sleep = ms => new Promise(r => setTimeout(r, ms));

if (!HOST || !TOKEN) { console.log('FAIL: need the board address (VIZ_BOARD or upload_port) and API_TOKEN in include/secrets.h'); process.exit(1); }

// One request on a fresh connection (the board has few sockets). Answers { status, text, json, type }; status 0 = no answer.
//   token  value for X-Api-Token (omit for none)        body   object (sent as JSON) or string
//   form   true: send body as application/x-www-form-urlencoded instead of JSON
function request(method, urlPath, { token, body, headers, form } = {}) {
  return new Promise(resolve => {
    const h = Object.assign({}, headers || {});
    if (token !== undefined) h['X-Api-Token'] = token;
    let data;
    if (body !== undefined) {
      data = typeof body === 'string' ? body : JSON.stringify(body);
      h['Content-Type'] = form ? 'application/x-www-form-urlencoded' : 'application/json';
      h['Content-Length'] = Buffer.byteLength(data);
    }
    const req = http.request({ host: HOST, port: 80, path: urlPath, method, headers: h, agent: false, timeout: 8000 }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString();
        let json = null; try { json = JSON.parse(text); } catch (e) { /* not JSON */ }
        resolve({ status: res.statusCode, text, json, type: res.headers['content-type'] || '' });
      });
    });
    req.on('timeout', () => { req.destroy(); resolve({ status: 0, text: '', json: null, type: '' }); });
    req.on('error', () => resolve({ status: 0, text: '', json: null, type: '' }));
    if (data) req.write(data);
    req.end();
  });
}
const get = p => request('GET', p);
const post = (p, body, token = TOKEN) => request('POST', p, { token, body });                 // JSON body, with the token
const postForm = (p, body, token = TOKEN) => request('POST', p, { token, body, form: true }); // form body, with the token

module.exports = { HOST, TOKEN, root, sleep, request, get, post, postForm };
