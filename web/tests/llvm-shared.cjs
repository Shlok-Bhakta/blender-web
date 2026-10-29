// SPDX-License-Identifier: GPL-2.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const {chromium} = require('playwright');
const directory = path.resolve(process.argv[2]);
const output = path.resolve(process.argv[3]);

function verify(report) {
  assert.equal(report.llvmVersion, '20.1.8');
  assert.equal(report.compiledModules, 3);
  assert.equal(report.existingWorkers, 4);
  assert.equal(report.newWorkers, 3);
  assert.equal(report.atomicUpdates, 15360);
  assert(report.sharedMemoryExecution && report.simdExecution);
  assert.equal(report.reclamationVerified, false);
  assert(report.tableAfter >= report.tableBefore);
}

(async () => {
  const create = require(path.join(directory, 'llvm-compiler.js'));
  const lines = [], diagnostics = [];
  const node = await create({noInitialRun: true, print: line => lines.push(line),
    printErr: line => { diagnostics.push(line); console.error(line); }});
  const status = node.callMain([]);
  if (!lines.some(line => line.startsWith('{'))) console.error({status, lines, diagnostics});
  assert.equal(status, 0);
  const native = JSON.parse(lines.find(line => line.startsWith('{')));
  verify(native);
  node.PThread.terminateAllThreads();
  const server = http.createServer((request, response) => {
    const name = new URL(request.url, 'http://localhost').pathname;
    response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    response.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    if (name === '/') {
      response.setHeader('Content-Type', 'text/html');
      response.end('<!doctype html><title>Shared-memory LLVM execution</title><script src="llvm-compiler.js"></script>');
    } else if (['/llvm-compiler.js', '/llvm-compiler.wasm'].includes(name)) {
      response.setHeader('Content-Type', name.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
      fs.createReadStream(path.join(directory, name.slice(1))).pipe(response);
    } else response.writeHead(404).end();
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await chromium.launch({headless: true});
    const page = await browser.newPage(), errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.evaluate(async () => {
      window.lines = [];
      window.diagnostics = [];
      window.query = await createLLVMProbe({noInitialRun: true,
        print: line => lines.push(line), printErr: line => { diagnostics.push(line); console.error(line); }});
    });
    await page.context().setOffline(true);
    const proof = await page.evaluate(() => {
      if (navigator.onLine) throw new Error('LLVM shared-memory check requires offline Chromium');
      query.callMain([]);
      const result = JSON.parse(lines.find(line => line.startsWith('{')));
      query.PThread.terminateAllThreads();
      return {result, diagnostics, crossOriginIsolated, offline: !navigator.onLine};
    });
    verify(proof.result);
    assert.deepEqual(proof.result, native);
    assert.deepEqual(errors, []);
    assert(proof.offline && proof.crossOriginIsolated);
    fs.mkdirSync(output, {recursive: true});
    const report = {node: native, browser: proof, browserErrors: errors,
      browserVersion: browser.version(), applicationIntegrated: false};
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify(report));
  } finally {
    if (browser) await browser.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exit(1); });
