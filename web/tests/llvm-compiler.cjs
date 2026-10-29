/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const crypto = require('node:crypto');
const {chromium} = require('playwright');
const {checkGenerated} = require('./llvm-generated.cjs');
const directory = path.resolve(process.argv[2]);
const output = path.resolve(process.argv[3]);
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');

(async () => {
  const server = http.createServer((req, res) => {
    const name = new URL(req.url, 'http://localhost').pathname;
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    if (name === '/') {
      res.setHeader('Content-Type', 'text/html');
      res.end('<!doctype html><title>Original LLVM compiler probe</title><script src="llvm-compiler.js"></script>');
      return;
    }
    if (!['/llvm-compiler.js', '/llvm-compiler.wasm'].includes(name)) {
      res.writeHead(404).end(); return;
    }
    res.setHeader('Content-Type', name.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
    fs.createReadStream(path.join(directory, name.slice(1))).pipe(res);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await chromium.launch({headless: true});
    const page = await browser.newPage(), errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.addScriptTag({content: checkGenerated.toString()});
    await page.evaluate(async () => {
      window.probeLines = [];
      window.probeDiagnostics = [];
      window.compiler = await createLLVMProbe({noInitialRun: true,
        print: line => probeLines.push(line), printErr: line => probeDiagnostics.push(line)});
    });
    await page.context().setOffline(true);
    const report = await page.evaluate(async () => {
      if (navigator.onLine) throw new Error('Compiler proof requires the offline browser state');
      const start = performance.now();
      compiler.callMain([]);
      const compileSeconds = (performance.now() - start) / 1000;
      const result = JSON.parse(probeLines.find(line => line.startsWith('{')));
      const generated = [];
      for (let i = 0; i < 3; i++) {
        const bytes = compiler.FS.readFile(`/shader-${i}.wasm`);
        const ir = new TextDecoder().decode(compiler.FS.readFile(`/shader-${i}.ll`));
        if (!ir.includes(`ret i32 ${42 + i}`)) throw new Error('Constant fold missing');
        generated.push({...await checkGenerated(bytes, 42 + i), bytes: bytes.length});
      }
      compiler.PThread.terminateAllThreads();
      return {...result, generated, diagnostics: probeDiagnostics, compileSeconds,
        compiledAndExecutedOffline: true,
        navigatorOnlineAtCompilation: navigator.onLine,
        crossOriginIsolated, userAgent: navigator.userAgent};
    });
    assert.equal(report.llvmVersion, '20.1.8');
    assert.equal(report.compiledModules, 3);
    assert(report.invalidIRRejected && report.invalidLinkRecovered && report.crossOriginIsolated);
    assert.equal(report.generated.length, 3);
    assert.deepEqual(errors, []);
    report.browserVersion = browser.version();
    report.browserErrors = errors;
    report.compilerWasm = {bytes: fs.statSync(path.join(directory, 'llvm-compiler.wasm')).size,
      sha256: hash(fs.readFileSync(path.join(directory, 'llvm-compiler.wasm')))};
    fs.mkdirSync(output, {recursive: true});
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify(report));
  } finally {
    if (browser) await browser.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
