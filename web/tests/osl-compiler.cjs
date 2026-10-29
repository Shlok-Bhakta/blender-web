// SPDX-License-Identifier: GPL-2.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const {chromium} = require('playwright');
const directory = path.resolve(process.argv[2]), base = path.resolve(process.argv[4]);
const output = path.resolve(process.argv[3]);
const source = fs.readFileSync(path.join(base, 'query-fixtures/web_query.osl'), 'utf8');
const headers = {};
for (const name of fs.readdirSync(path.join(base, 'osl/src/shaders'))) {
  if (name.endsWith('.h')) headers[name] = fs.readFileSync(path.join(base, 'osl/src/shaders', name), 'utf8');
}

function prepare(compiler, source, headers) {
  compiler.FS.mkdirTree('/fixtures');
  compiler.FS.mkdirTree('/osl/include');
  for (const [name, text] of Object.entries(headers)) compiler.FS.writeFile('/osl/include/' + name, text);
  compiler.FS.writeFile('/fixtures/web_query.osl', source);
  compiler.FS.writeFile('/fixtures/bad.osl', 'shader bad(output color result=0) { result=not_declared; }');
}

function compileCases(compiler) {
  const args = (input, result, extra = []) => ['-q', '-I/osl/include', '-o', result, ...extra, input];
  const file = compiler.callMain(args('/fixtures/web_query.osl', '/fixtures/file.oso'));
  if (file !== 0) throw new Error('OSL file compilation failed: ' + file);
  const memory = compiler.callMain(args('/fixtures/web_query.osl', '/fixtures/memory.oso', ['-buffer']));
  if (memory !== 0) throw new Error('OSL memory compilation failed: ' + memory);
  const rejected = compiler.callMain(args('/fixtures/bad.osl', '/fixtures/bad.oso'));
  if (rejected === 0) throw new Error('Invalid shader was accepted');
  const recovered = compiler.callMain(args('/fixtures/web_query.osl', '/fixtures/recovered.oso'));
  if (recovered !== 0) throw new Error('OSL compilation did not recover');
  const decoder = new TextDecoder();
  const results = {};
  for (const name of ['file', 'memory', 'recovered']) {
    results[name] = decoder.decode(compiler.FS.readFile('/fixtures/' + name + '.oso'));
    if (!results[name].startsWith('OpenShadingLanguage')) throw new Error('OSO header missing');
  }
  compiler.PThread.terminateAllThreads();
  return {results, invalidRejected: true, recovered: true};
}

(async () => {
  fs.mkdirSync(output, {recursive: true});
  const lines = [], diagnostics = [];
  const create = require(path.join(directory, 'oslc.js'));
  const compiler = await create({noInitialRun: true, print: line => lines.push(line),
    printErr: line => { diagnostics.push(line); console.error(line); }});
  prepare(compiler, source, headers);
  const node = compileCases(compiler);
  for (const [name, result] of Object.entries(node.results)) fs.writeFileSync(path.join(output, 'node-' + name + '.oso'), result);
  const server = http.createServer((request, response) => {
    const name = new URL(request.url, 'http://localhost').pathname;
    response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    response.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    if (name === '/') {
      response.setHeader('Content-Type', 'text/html');
      response.end(`<!doctype html><meta charset="utf-8"><title>Original OSL compiler</title>
        <style>body{margin:32px;background:#17191d;color:#e8e9ed;font:18px monospace}
        h1{font-size:27px}main{display:grid;grid-template-columns:1fr 1fr;gap:28px}
        pre{white-space:pre-wrap;font-size:16px;line-height:1.5;padding:20px;background:#22262c}
        #status{color:#a8d5ad}</style><h1>OSL 1.15.3.0 · original Wasm compiler</h1>
        <p id="status">Loading local Clang and LLVM libraries…</p>
        <main><pre id="source"></pre><pre id="result">Standalone compiler proof.
Browser shader execution and Blender integration are separate work.</pre></main>
        <script src="oslc.js"></script>`);
    } else if (['/oslc.js', '/oslc.wasm'].includes(name)) {
      response.setHeader('Content-Type', name.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
      fs.createReadStream(path.join(directory, name.slice(1))).pipe(response);
    } else response.writeHead(404).end();
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await chromium.launch({headless: true});
    const page = await browser.newPage({viewport: {width: 1440, height: 900},
      recordVideo: {dir: path.join(output, 'video'), size: {width: 1440, height: 900}}}), errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.addScriptTag({content: prepare.toString() + '\n' + compileCases.toString()});
    await page.evaluate(async ({source, headers}) => {
      document.querySelector('#source').textContent = source;
      window.lines = [];
      window.diagnostics = [];
      window.compiler = await createOSLCompilerProbe({noInitialRun: true,
        print: line => lines.push(line), printErr: line => diagnostics.push(line)});
      prepare(compiler, source, headers);
    }, {source, headers});
    await page.context().setOffline(true);
    const proof = await page.evaluate(() => {
      if (navigator.onLine) throw new Error('OSL compiler check requires offline Chromium');
      const result = compileCases(compiler);
      document.querySelector('#status').textContent = 'OFFLINE · all compiler checks passed';
      document.querySelector('#result').textContent = JSON.stringify({
        fileCompilation: 'passed', memoryCompilation: 'passed', invalidShader: 'rejected',
        recovery: 'passed', bytecodeBytes: result.results.file.length,
        source: 'original OSL, Clang and LLVM', browserShaderExecution: 'pending',
        bytecodeHeader: result.results.file.split('\n').slice(0, 8)}, null, 2);
      return {...result, diagnostics, crossOriginIsolated, offline: !navigator.onLine};
    });
    assert.deepEqual(proof.results, node.results);
    assert.deepEqual(errors, []);
    assert(proof.offline && proof.crossOriginIsolated);
    for (const [name, result] of Object.entries(proof.results)) fs.writeFileSync(path.join(output, 'browser-' + name + '.oso'), result);
    await page.screenshot({path: path.join(output, 'osl-compiler.png')});
    await page.waitForTimeout(2000);
    const video = page.video();
    await page.close();
    fs.copyFileSync(await video.path(), path.join(output, 'osl-compiler.webm'));
    const report = {node, browser: proof, diagnostics, browserErrors: errors,
      browserVersion: browser.version(), applicationIntegrated: false, shaderExecutionVerified: false};
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify({fileBytes: node.results.file.length, bufferBytes: node.results.memory.length,
      browserErrors: errors, invalidRejected: true, recovered: true, offline: proof.offline}));
  } finally {
    if (browser) await browser.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exit(1); });
