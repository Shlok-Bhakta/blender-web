// SPDX-License-Identifier: GPL-2.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const {chromium} = require('playwright');
const runtimeDirectory = path.resolve(process.argv[2]);
const compilerBase = path.resolve(process.argv[3]);
const output = path.resolve(process.argv[4]);
const fixture = fs.readFileSync(path.join(__dirname, '../native/tests/fixtures/osl-runtime/web_execute.osl'), 'utf8');
const headers = {};
for (const name of fs.readdirSync(path.join(compilerBase, 'osl/src/shaders')))
  if (name.endsWith('.h')) headers[name] = fs.readFileSync(path.join(compilerBase, 'osl/src/shaders', name), 'utf8');

function compile(compiler, source, headers) {
  compiler.FS.mkdirTree('/fixtures');
  compiler.FS.mkdirTree('/osl/include');
  for (const [name, text] of Object.entries(headers)) compiler.FS.writeFile('/osl/include/' + name, text);
  compiler.FS.writeFile('/fixtures/web_execute.osl', source);
  const result = compiler.callMain(['-q', '-I/osl/include', '-o', '/fixtures/web_execute.oso', '/fixtures/web_execute.osl']);
  if (result !== 0) throw new Error('Original OSL compilation failed: ' + result);
  return new TextDecoder().decode(compiler.FS.readFile('/fixtures/web_execute.oso'));
}

function execute(runtime, bytecode, lines) {
  runtime.FS.mkdirTree('/fixtures');
  runtime.FS.writeFile('/fixtures/web_execute.oso', bytecode);
  const status = runtime.callMain([]);
  if (status !== 0) throw new Error('Original OSL execution failed: ' + status);
  const report = lines.find(line => line.startsWith('{'));
  if (!report) throw new Error('OSL execution report missing');
  return JSON.parse(report);
}

function verify(report) {
  assert.equal(report.varyingOSLExecution, true);
  assert.equal(report.points, 768);
  assert.equal(report.rendererCallbacks, 1536);
  assert.equal(report.existingWorkers, 4);
  assert.equal(report.newWorkers, 1);
  assert.equal(report.stringOperations, true);
  assert.equal(report.reclamationVerified, false);
}

(async () => {
  fs.mkdirSync(output, {recursive: true});
  const compilerDirectory = path.join(compilerBase, 'osl-wasm/bin');
  const createCompiler = require(path.join(compilerDirectory, 'oslc.js'));
  const compiler = await createCompiler({noInitialRun: true});
  const nodeBytecode = compile(compiler, fixture, headers);
  compiler.PThread.terminateAllThreads();
  const lines = [], diagnostics = [];
  const createRuntime = require(path.join(runtimeDirectory, 'osl-runtime.js'));
  const runtime = await createRuntime({noInitialRun: true, print: line => lines.push(line),
    printErr: line => {diagnostics.push(line); console.error(line);}});
  const node = execute(runtime, nodeBytecode, lines);
  verify(node);
  await new Promise(resolve => setImmediate(resolve));
  runtime.PThread.terminateAllThreads();
  const server = http.createServer((request, response) => {
    const name = new URL(request.url, 'http://localhost').pathname;
    response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    response.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    if (name === '/') {
      response.setHeader('Content-Type', 'text/html');
      response.end(`<!doctype html><meta charset="utf-8"><title>Original OSL execution</title>
        <style>body{margin:32px;background:#17191d;color:#e8e9ed;font:18px monospace}
        h1{font-size:27px}main{display:grid;grid-template-columns:1fr 1fr;gap:28px}
        pre{white-space:pre-wrap;font-size:17px;line-height:1.5;padding:20px;background:#22262c}
        #status{color:#a8d5ad}</style><h1>Original OSL · browser compilation and execution</h1>
        <p id="status">Loading original compiler and runtime libraries…</p>
        <main><pre id="source"></pre><pre id="result">Standalone runtime proof.
Production Cycles integration and generated-module reclamation remain pending.</pre></main>
        <script src="compiler/oslc.js"></script><script src="runtime/osl-runtime.js"></script>`);
      return;
    }
    const match = name.match(/^\/(compiler\/oslc|runtime\/osl-runtime)\.(js|wasm)$/);
    if (!match) {response.writeHead(404).end(); return;}
    const directory = name.startsWith('/compiler/') ? compilerDirectory : runtimeDirectory;
    response.setHeader('Content-Type', match[2] === 'wasm' ? 'application/wasm' : 'text/javascript');
    fs.createReadStream(path.join(directory, path.basename(name))).pipe(response);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await chromium.launch({headless: true});
    const page = await browser.newPage({viewport: {width: 1440, height: 900},
      recordVideo: {dir: path.join(output, 'video'), size: {width: 1440, height: 900}}});
    const errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.addScriptTag({content: compile.toString() + '\n' + execute.toString()});
    await page.evaluate(async source => {
      document.querySelector('#source').textContent = source;
      window.lines = []; window.diagnostics = [];
      window.compiler = await createOSLCompilerProbe({noInitialRun: true});
      window.runtime = await createOSLRuntimeProbe({noInitialRun: true,
        print: line => lines.push(line), printErr: line => diagnostics.push(line)});
    }, fixture);
    await page.context().setOffline(true);
    const proof = await page.evaluate(async ({source, headers}) => {
      if (navigator.onLine) throw new Error('OSL runtime proof requires offline Chromium');
      const bytecode = compile(compiler, source, headers);
      compiler.PThread.terminateAllThreads();
      const result = execute(runtime, bytecode, lines);
      document.querySelector('#status').textContent = 'OFFLINE · original OSL shader compiled and executed';
      document.querySelector('#result').textContent = JSON.stringify(result, null, 2)
        + '\n\nAll numeric values match the independent reference.\nFull string hashes match exactly.\nNative library readback follows this recording.';
      await new Promise(resolve => setTimeout(resolve, 0));
      runtime.PThread.terminateAllThreads();
      return {result, bytecode, diagnostics, crossOriginIsolated, offline: !navigator.onLine};
    }, {source: fixture, headers});
    verify(proof.result);
    assert.deepEqual(proof.result, node);
    assert.equal(proof.bytecode, nodeBytecode);
    assert.deepEqual(errors, []);
    assert(proof.crossOriginIsolated && proof.offline);
    await page.screenshot({path: path.join(output, 'osl-runtime.png')});
    await page.waitForTimeout(4000);
    const video = page.video(); await page.close();
    fs.copyFileSync(await video.path(), path.join(output, 'osl-runtime.webm'));
    for (const [name, bytecode] of [['node', nodeBytecode], ['browser', proof.bytecode]]) {
      const directory = path.join(output, name + '-fixtures');
      fs.mkdirSync(directory, {recursive: true});
      fs.writeFileSync(path.join(directory, 'web_execute.oso'), bytecode);
    }
    const report = {node, browser: proof, nodeDiagnostics: diagnostics, browserErrors: errors,
      browserVersion: browser.version(), applicationIntegrated: false, browserShaderExecutionVerified: true};
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify({node, browser: proof.result, browserErrors: errors, offline: proof.offline}));
  } finally {
    if (browser) await browser.close();
    server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
  }
})().catch(error => {console.error(error); process.exit(1);});
