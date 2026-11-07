// Deeper node check: loopback HTTP server and fetch client, zlib streams, fs.watch, timers under load,
// and node:test. Prints one line per check; exits 1 if any failed.
const http = require('http');
const zlib = require('zlib');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { pipeline } = require('stream/promises');
const { Readable } = require('stream');

const results = [];
const check = (name, ok, detail) => { results.push(ok); console.log(`${name.padEnd(12)} ${ok ? 'ok' : 'FAIL'} (${detail})`); };
const withTimeout = (promise, ms, label) => Promise.race([promise,
  new Promise((_, reject) => setTimeout(() => reject(new Error(`${label} timed out`)), ms))]);

async function httpCheck() {
  const server = http.createServer((req, res) => {
    let body = '';
    req.on('data', (c) => { body += c; });
    req.on('end', () => { res.setHeader('content-type', 'application/json'); res.end(JSON.stringify({ echo: body, url: req.url })); });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();
  const replies = await Promise.all([1, 2, 3].map((n) =>
    fetch(`http://127.0.0.1:${port}/r${n}`, { method: 'POST', body: `n=${n}` }).then((r) => r.json())));
  server.close();
  return replies.map((r) => `${r.url}:${r.echo}`).join(',');
}

async function zlibCheck() {
  const input = Buffer.alloc(1 << 20, 'abcdefgh');
  const gz = zlib.gzipSync(input, { level: 9 });
  const back = zlib.gunzipSync(gz);
  const chunks = [];
  await pipeline(Readable.from([input]), zlib.createBrotliCompress(), zlib.createBrotliDecompress(),
    async function* (source) { for await (const c of source) chunks.push(c); });
  return { ok: back.equals(input) && Buffer.concat(chunks).equals(input), detail: `gzip ${gz.length} bytes` };
}

async function watchCheck() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'alchemy-watch-'));
  const seen = new Promise((resolve) => {
    const watcher = fs.watch(dir, (event, name) => { if (name === 'touched.txt') { watcher.close(); resolve(event); } });
  });
  setTimeout(() => fs.writeFileSync(path.join(dir, 'touched.txt'), 'x'), 200);
  let event;
  try { event = await withTimeout(seen, 10000, 'fs.watch'); } catch (e) { event = `no event: ${e.message}`; }
  // Removal is checked on its own: unlink then rmdir of the watched directory must leave nothing behind.
  let removal = 'ok';
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) {
    removal = `${e.code}; left: ${fs.existsSync(dir) ? fs.readdirSync(dir).join(',') || '(empty)' : '(gone)'}`;
  }
  return { event, removal };
}

async function testRunnerCheck() {
  const { run } = require('node:test');
  const file = path.join(os.tmpdir(), `alchemy-node-test-${process.pid}.js`);
  fs.writeFileSync(file, "const test = require('node:test'); const assert = require('assert');\n" +
    "test('adds', () => assert.strictEqual(1 + 1, 2));\ntest('async', async () => { await new Promise(r => setTimeout(r, 20)); });\n");
  let pass = 0, fail = 0;
  for await (const event of run({ files: [file] })) {
    if (event.type === 'test:pass' && event.data.nesting === 0) pass++;
    if (event.type === 'test:fail') fail++;
  }
  fs.unlinkSync(file);
  return { pass, fail };
}

(async () => {
  try { const r = await withTimeout(httpCheck(), 30000, 'http'); check('http-fetch', r === '/r1:n=1,/r2:n=2,/r3:n=3', r); }
  catch (e) { check('http-fetch', false, e.message); }
  try { const r = await zlibCheck(); check('zlib', r.ok, r.detail); } catch (e) { check('zlib', false, e.message); }
  try {
    const r = await watchCheck();
    check('fs-watch', r.event === 'rename' || r.event === 'change', r.event);
    check('rm-watched', r.removal === 'ok', r.removal);
  } catch (e) { check('fs-watch', false, e.message); }
  try { const t0 = Date.now(); await new Promise((r) => setTimeout(r, 100)); const dt = Date.now() - t0; check('timer', dt >= 95 && dt < 2000, `${dt} ms`); }
  catch (e) { check('timer', false, e.message); }
  try { const r = await withTimeout(testRunnerCheck(), 120000, 'node:test'); check('node-test', r.pass === 2 && r.fail === 0, `${r.pass} pass, ${r.fail} fail`); }
  catch (e) { check('node-test', false, e.message); }
  process.exit(results.every(Boolean) ? 0 : 1);
})();
