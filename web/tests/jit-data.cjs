/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const {chromium} = require('playwright');

async function execute(runtime, lines) {
  if (runtime.callMain([]) !== 0) throw Error('JIT data check did not start');
  const deadline = Date.now() + 90000;
  while (!lines.some(line => line.startsWith('{"programs":'))) {
    if (lines.some(line => /Aborted\(|JIT data check:/.test(line))) throw Error(lines.join('\n'));
    if (Date.now() > deadline) throw Error('JIT data check timed out: ' + lines.slice(-5).join('\n'));
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  return JSON.parse(lines.find(line => line.startsWith('{"programs":')));
}

function verify(report) {
  assert.equal(report.programs, 32); assert.equal(report.workerReads, 128);
  for (const field of ['globalAddresses', 'dataRelocations', 'functionRelocations',
    'nativeOwnerRetirement', 'independentOwners', 'outOfOrderRetirement',
    'concurrentInitialization']) assert.equal(report[field], true);
  assert.equal(report.variableLayouts, 8); assert.equal(report.dataAlignment, 64);
  assert(report.ownedBlocks > 0 && report.ownedBlocks <= 64);
  assert(report.ownedTableCapacity <= 64 * report.largestBlock);
}

async function main() {
  const directory = path.resolve(process.argv[2] || 'build/native/jit-data-check/wasm');
  const output = path.resolve(process.argv[3] || 'build/native/jit-data-check/evidence');
  fs.mkdirSync(output, {recursive: true});
  const lines = [], diagnostics = [];
  const create = require(path.join(directory, 'jit-data.js'));
  const runtime = await create({noInitialRun: true, print: text => lines.push(text),
    printErr: text => {lines.push(text); diagnostics.push(text);}});
  let node;
  try { node = await execute(runtime, lines); verify(node); }
  finally {
    fs.writeFileSync(path.join(output, 'node.log'), lines.join('\n') + '\n');
    runtime.PThread.terminateAllThreads();
  }
  if (process.env.WEB_JIT_NODE_ONLY === '1') { console.log(JSON.stringify(node)); return; }
  const server = http.createServer((request, response) => {
    response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    response.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    const name = new URL(request.url, 'http://localhost').pathname;
    if (name === '/') {
      response.setHeader('Content-Type', 'text/html');
      response.end('<!doctype html><meta charset="utf-8"><title>JIT data addresses</title>' +
        '<h1>LLVM JIT data and function addresses</h1><pre id="result">Loading actual installed LLVM libraries.</pre>' +
        '<script src="jit-data.js"></script>');
    } else if (/^\/jit-data\.(js|wasm)$/.test(name)) {
      response.setHeader('Content-Type', name.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
      fs.createReadStream(path.join(directory, path.basename(name))).pipe(response);
    } else response.writeHead(404).end();
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const browser = await chromium.launch({headless: true});
  const context = await browser.newContext({viewport: {width: 1100, height: 700}});
  const page = await context.newPage(), errors = [];
  page.on('pageerror', error => errors.push(error.stack || error.message));
  try {
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.evaluate(async () => {
      window.lines = [];
      window.runtime = await createJITDataCheck({noInitialRun: true,
        print: text => lines.push(text), printErr: text => lines.push(text)});
    });
    await context.setOffline(true);
    const result = await page.evaluate(async source => {
      const report = await (0, eval)('(' + source + ')')(runtime, lines);
      if (navigator.onLine) throw Error('Browser is not offline');
      document.getElementById('result').textContent = JSON.stringify(report, null, 2) + '\nOffline Chromium: passed';
      runtime.PThread.terminateAllThreads();
      return report;
    }, execute.toString());
    verify(result); assert.deepEqual(errors, []);
    await page.screenshot({path: path.join(output, 'jit-data.png')});
    const report = {node, browser: result, offline: true, browserErrors: errors,
      nodeDiagnostics: diagnostics, browserVersion: browser.version()};
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify(report));
  } finally {
    await context.close(); await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
}

if (require.main === module) main().catch(error => { console.error(error); process.exit(1); });
