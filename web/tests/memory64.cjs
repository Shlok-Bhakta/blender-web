/* SPDX-License-Identifier: GPL-2.0-or-later */
// Repository browser workflow for the isolated, non-Blender memory64 probe.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const {chromium} = require('playwright');

async function main() {
  const directory = path.resolve(process.argv[2] || 'build/native/memory64');
  const server = http.createServer((request, response) => {
    response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    response.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    if (request.url === '/') {
      response.setHeader('Content-Type', 'text/html');
      response.end(`<script>globalThis.probeMessages=[];var Module={print:s=>probeMessages.push(s),printErr:s=>console.error(s)};</script><script src="probe.js"></script>`);
      return;
    }
    const name = request.url.slice(1);
    if (!['probe.js', 'probe.wasm'].includes(name)) { response.writeHead(404).end(); return; }
    response.setHeader('Content-Type', name.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
    fs.createReadStream(path.join(directory, name)).pipe(response);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await chromium.launch({headless:true});
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    page.on('console', message => { if (message.type() === 'error') errors.push(message.text()); });
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.waitForFunction(() => probeMessages.some(line => line.startsWith('MEMORY64_PROBE:')), null, {timeout:120000});
    const result = await page.evaluate(() => ({isolated:crossOriginIsolated,
      result:JSON.parse(probeMessages.find(line=>line.startsWith('MEMORY64_PROBE:')).split(':').slice(1).join(':'))}));
    assert.equal(result.isolated, true);
    assert.equal(result.result.pointerBytes, 8);
    assert(result.result.allocationBytes > 2**32);
    assert(result.result.heapBytes > 2**32);
    assert(result.result.highAddress > 2**32);
    assert.equal(result.result.crossThreadReadWrite, true);
    assert.deepEqual(errors, []);
    const report = {...JSON.parse(fs.readFileSync(path.join(directory, 'build.json'))),
      browserVersion:browser.version(), browserFlags:[], browser:result, browserErrors:errors};
    fs.writeFileSync(path.join(directory, 'acceptance.json'), JSON.stringify(report, null, 2)+'\n');
    console.log(JSON.stringify(report));
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
}
main().catch(error => { console.error(error); process.exitCode=1; });
