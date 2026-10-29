/* SPDX-License-Identifier: GPL-2.0-or-later */
// Run the same release-mode dependency binaries in Chromium and under Node.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const crypto = require('node:crypto');
const {chromium} = require('playwright');
const directory = path.resolve(process.env.WEB_FFI_DIRECTORY || 'build/native/checks');
const output = path.resolve(process.env.WEB_FFI_OUTPUT || 'build/native/ffi-browser-evidence');
const entries = ['ffi-workers-check', 'ffi-high-address-check'];
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');

async function main() {
  const server = http.createServer((request, response) => {
    const url = new URL(request.url, 'http://localhost');
    response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    response.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    if (url.pathname === '/') {
      const entry = url.searchParams.get('entry');
      if (!entries.includes(entry)) { response.writeHead(400).end(); return; }
      response.setHeader('Content-Type', 'text/html');
      response.end(`<!doctype html><title>Original libffi worker checks</title><script>
        window.probeOutput=[];window.probeErrors=[];
        var Module={print(text){probeOutput.push(text);try{window.probeResult=JSON.parse(text)}catch{}},
          printErr(text){probeOutput.push(text)},onAbort(reason){probeErrors.push(String(reason))},
          onExit(status){window.probeExit=status}};
        </script><script src="${entry}.js"></script>`);
      return;
    }
    const match = /^\/(ffi-workers-check|ffi-high-address-check)\.(js|wasm)$/.exec(url.pathname);
    if (!match) { response.writeHead(404).end(); return; }
    const file = path.join(directory, match[1] + '.' + match[2]);
    if (!fs.existsSync(file)) { response.writeHead(404).end(); return; }
    response.setHeader('Content-Type', match[2] === 'js' ? 'text/javascript' : 'application/wasm');
    fs.createReadStream(file).pipe(response);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await chromium.launch({headless: true});
    fs.mkdirSync(output, {recursive: true});
    const results = {};
    for (const entry of entries) {
      const page = await browser.newPage(), errors = [];
      page.on('pageerror', error => errors.push(String(error)));
      await page.goto(`http://127.0.0.1:${server.address().port}/?entry=${entry}`);
      assert(await page.evaluate(() => crossOriginIsolated));
      await page.waitForFunction(() => window.probeExit === 0 || window.probeExit > 0 ||
        window.probeErrors.length, null, {timeout: 180000});
      const raw = await page.evaluate(() => ({exit: window.probeExit, result: window.probeResult,
        errors: window.probeErrors, output: window.probeOutput}));
      fs.writeFileSync(path.join(output, entry + '-raw.json'), JSON.stringify(raw, null, 2) + '\n');
      assert.equal(raw.exit, 0);
      assert.deepEqual(raw.errors, []);
      assert.deepEqual(errors, []);
      if (entry === 'ffi-workers-check') {
        assert.equal(raw.result.calls, 9341);
        assert.equal(raw.result.signatureReuseIterations, 1000);
        assert(raw.result.threadExitDuringCallback && raw.result.nestedRetirement &&
          raw.result.zeroExtraVariadicArguments && raw.result.releaseAssertionsDisabled);
      } else {
        assert.equal(raw.result.heapBytes, 2418016256);
        assert(raw.result.highMetadata && raw.result.highStack && raw.result.aggregateCallbacks &&
          raw.result.highClosureAllocation &&
          raw.result.sparseAllocation);
      }
      results[entry] = {result: raw.result, browserErrors: errors,
        wasmSha256: hash(fs.readFileSync(path.join(directory, entry + '.wasm'))),
        jsSha256: hash(fs.readFileSync(path.join(directory, entry + '.js')))};
      await page.close();
    }
    const report = {measuredAt: new Date().toISOString(), browserVersion: browser.version(),
      crossOriginIsolated: true, results};
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify(report));
  } finally {
    if (browser) await browser.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
}
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
