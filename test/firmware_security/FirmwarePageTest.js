// Exercise the shipped firmware page against a fake transport. Cryptographic
// and flash behavior are tested separately by the native production-code tests.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const root = path.resolve(__dirname, '../..');
const html = fs.readFileSync(path.join(root, 'web/pages/firmware.html'), 'utf8');
const serverSource = fs.readFileSync(path.join(root, 'src/network/CrossPointWebServer.cpp'), 'utf8');
const routes = new Map([...serverSource.matchAll(/server->on\(\s*"([^"]+)"\s*,\s*HTTP_(GET|POST)\s*,\s*\[this\]\s*\{\s*(\w+)\(/g)]
  .map(match => [`${match[2]} ${match[1]}`, match[3]]));
assert.equal(routes.get('POST /api/firmware/manual-download'), 'handleFirmwareManualDownload');
assert.equal(routes.get('GET /api/firmware/source'), 'handleFirmwareSource');
assert.equal(routes.get('POST /api/firmware/source'), 'handleFirmwareSource');
function requireRoute(method, url) {
  assert.ok(routes.has(`${method} ${url.split('?')[0]}`), `Unregistered firmware endpoint: ${method} ${url}`);
}
const elements = new Map();
const requests = [], xhrs = [], confirmations = [];
let state = { state: 'idle', device: 'x4-pro', version: '1.8.0', candidateSha256: 'a'.repeat(64) };
let installFails = false;
function element(id) {
  assert.ok(html.includes(`id="${id}"`), `Missing real HTML element ${id}`);
  if (!elements.has(id)) elements.set(id, {
    value: id === 'firmware-mode' ? 'signed' : '', files: [], style: {}, textContent: '', hidden: false,
    listeners: {}, addEventListener(event, callback) { this.listeners[event] = callback; }
  });
  return elements.get(id);
}
const response = (data, status = 200) => ({ ok: status < 400, status, json: async () => ({ ...data }) });
class FakeFormData { append(name, value) { this[name] = value; } }
class FakeXHR {
  constructor() { this.headers = {}; this.upload = {}; }
  open(method, url) { requireRoute(method, url); this.method = method; this.url = url; }
  setRequestHeader(name, value) { this.headers[name] = value; }
  send() {
    xhrs.push(this);
    assert.equal(this.headers['X-Inkademic-Firmware'], '1');
    if (this.url.includes('/upload?')) {
      const query = new URL(this.url, 'http://reader').searchParams;
      state = { ...state, state: query.get('mode') === 'manual' ? 'ready' : 'awaiting_signature',
        mode: query.get('mode'), received: Number(query.get('total')), total: Number(query.get('total')), size: Number(query.get('total')) };
    } else state = { ...state, state: 'ready', mode: 'signed', signatureVerified: true };
    this.status = 200; this.responseText = JSON.stringify(state); this.onload();
  }
}
const context = vm.createContext({
  document: { getElementById: element }, URL, URLSearchParams, FormData: FakeFormData, XMLHttpRequest: FakeXHR,
  window: { crypto: { randomUUID: () => 'test-session' }, confirm(message) { confirmations.push(message); return true; } },
  setInterval() {}, Date, Math,
  fetch: async (url, options = {}) => {
    requireRoute(options.method || 'GET', url);
    requests.push({ url, options });
    if (url.includes('/status')) return response(state);
    if (url === '/api/firmware/source') return response(options.method === 'POST'
      ? { mode: options.body.get('mode'), url: options.body.get('url') } : { mode: 'official' });
    if (url === '/api/firmware/install') return installFails ? response({ error: 'Image changed' }, 409) : response({ state: 'install_requested' }, 202);
    if (url === '/api/firmware/manual-download') return response({ state: 'uploading', mode: 'manual' }, 202);
    throw new Error(`Unexpected endpoint ${url}`);
  }
});
vm.runInContext(fs.readFileSync(path.join(root, 'web/pages/firmware.js'), 'utf8'), context);
const click = id => element(id).listeners.click();
const settle = () => new Promise(resolve => setImmediate(resolve));
(async () => {
  await settle();
  element('firmware-file').files = [{ name: 'other.bin', size: 65536, slice() { return {}; } }];
  await click('firmware-upload-btn');
  assert.equal(xhrs.length, 0); // signed mode never silently downgrades to manual
  assert.match(element('firmware-status').textContent, /matching Ed25519/);
  element('firmware-mode').value = 'manual'; element('firmware-mode').listeners.change();
  assert.equal(element('firmware-signature-group').hidden, true);
  await click('firmware-upload-btn');
  assert.equal(xhrs.length, 1); assert.ok(xhrs[0].url.endsWith('mode=manual'));
  assert.match(element('firmware-status').textContent, /origin not verified/i);
  installFails = true; await click('firmware-install-btn');
  assert.match(element('firmware-status').textContent, /Image changed/);
  assert.match(confirmations.at(-1), /manually selected/);
  const install = requests.findLast(r => r.url === '/api/firmware/install');
  assert.equal(install.options.body.get('mode'), 'manual');
  assert.equal(install.options.body.get('sha256'), 'a'.repeat(64));
  installFails = false;
  state = { ...state, state: 'idle', mode: 'signed' };
  element('firmware-mode').value = 'signed'; element('firmware-mode').listeners.change();
  element('firmware-signature-file').files = [{ name: 'release.sig', size: 64 }];
  await click('firmware-upload-btn');
  assert.equal(xhrs.length, 3); assert.ok(xhrs[1].url.endsWith('mode=signed'));
  assert.equal(xhrs[2].url, '/api/firmware/signature');
  assert.match(element('firmware-status').textContent, /Signed image/);
  element('firmware-url').value = 'https://example.com/other.bin';
  await click('firmware-manual-download-btn');
  let sent = requests.findLast(r => r.url === '/api/firmware/manual-download');
  assert.equal(sent.options.body.get('mode'), 'manual');
  assert.equal(sent.options.body.get('url'), 'https://example.com/other.bin');
  await click('firmware-source-save-btn');
  assert.match(element('firmware-source-status').textContent, /manual source/);
  await click('firmware-source-official-btn');
  assert.match(element('firmware-source-status').textContent, /official signed/);
  for (const request of requests.filter(r => r.options.method === 'POST'))
    assert.equal(request.options.headers['X-Inkademic-Firmware'], '1');
  console.log('Firmware page: signed/manual uploads, manual OTA, source switching and HTTP error reporting passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
