/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {chromium} = require('playwright');
const {ready, consoleCommand, messages} = require('./native.cjs');
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const runnerSha256 = hash(fs.readFileSync(__filename));
const hardware = process.env.WEB_TEST_HARDWARE === '1';
const fallback = process.env.WEB_TEST_UI_FALLBACK === '1';
const output = path.resolve(process.env.WEB_OSL_PYTHON_OUTPUT ||
  `build/native/osl-python-evidence${hardware ? '-hardware' : ''}${fallback ? '-ui' : ''}`);
const fixture = path.resolve(process.env.WEB_OSL_PYTHON_FIXTURE ||
  'web/native/tests/fixtures/osl-query/web_query.oso');
const directory = '/user/blender-web/home/osl-python';

async function main() {
  fs.mkdirSync(output, {recursive: true});
  messages.length = 0;
  const browser = await chromium.launch({headless: true, args: hardware ? [
    '--enable-unsafe-webgpu', '--use-angle=vulkan', '--enable-features=Vulkan',
    '--use-vulkan=native', '--disable-vulkan-surface',
  ] : ['--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-angle=swiftshader',
    '--enable-features=Vulkan', '--use-vulkan=swiftshader']});
  const context = await browser.newContext({viewport: {width: 1280, height: 800},
    recordVideo: {dir: output, size: {width: 1280, height: 800}}});
  if (fallback) await context.addInitScript(() => {
    delete WebAssembly.promising;
    delete WebAssembly.Suspending;
  });
  const page = await context.newPage(), errors = [], startupPythonErrors = [];
  const chapters = [], started = Date.now();
  let checking = false, rejectFailure;
  const failure = new Promise((_, reject) => { rejectFailure = reject; });
  failure.catch(() => {});
  const fail = text => { errors.push(text); rejectFailure(Error(text)); };
  page.on('console', message => {
    const text = message.text(); messages.push(text);
    fs.appendFileSync(path.join(output, 'browser.log'), text + '\n');
    if (/Traceback \(most recent call last\)/.test(text)) {
      if (checking) fail(text); else startupPythonErrors.push(text);
    }
    if (/OSL_PYTHON_FAILED:|Aborted\(|Blender startup failed|Blender UI frame failed|Blender GPU job failed|Blender WebGPU (error|device lost)/.test(text)) fail(text);
  });
  page.on('pageerror', error => fail(error.stack || error.message));
  page.on('crash', () => fail('Chromium crashed during the OSL Python query test'));
  const command = (code, marker) => Promise.race([failure,
    consoleCommand(page, code, marker, () => page.screenshot(), 180000)]);
  const chapter = title => chapters.push({seconds: (Date.now() - started) / 1000, title});
  try {
    chapter('Start original Blender with the original OSL Python query module');
    await page.goto(process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/');
    await ready(page, failure); checking = true;
    const manifest = await page.evaluate(() => fetch('manifest.json').then(r => r.json()));
    const adapter = await page.evaluate(async () => {
      const a = await navigator.gpu.requestAdapter({powerPreference: 'high-performance'});
      return {vendor: a.info.vendor, architecture: a.info.architecture,
        device: a.info.device, description: a.info.description};
    });
    if (hardware) assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
    const source = fs.readFileSync(path.join(__dirname, '../native/tests/osl-python.py'));
    const bytecode = fs.readFileSync(fixture);
    await page.evaluate(({directory, source, bytecode}) => {
      Module.FS.mkdirTree(directory);
      const decode = value => Uint8Array.from(atob(value), c => c.charCodeAt(0));
      Module.FS.writeFile(directory + '/check.py', decode(source));
      Module.FS.writeFile(directory + '/web_query.oso', decode(bytecode));
    }, {directory, source: source.toString('base64'), bytecode: bytecode.toString('base64')});
    await page.mouse.move(300, 250);
    const execute = `try:\n    op_report=__import__('runpy').run_path('${directory}/check.py')['run']('${directory}','${directory}/web_query.oso')\n    __import__('os').write(1,('OSL_PYTHON_DONE:'+__import__('json').dumps(op_report)+'\\n').encode())\nexcept BaseException:\n    __import__('os').write(2,('OSL_PYTHON_FAILED:'+__import__('traceback').format_exc()+'\\n').encode())`;
    chapter('Query real shader metadata, reject invalid input, and check Python threads');
    await command(`exec(${JSON.stringify(execute)})`, 'OSL_PYTHON_DONE:');
    const result = JSON.parse(messages.findLast(x => x.startsWith('OSL_PYTHON_DONE:')).slice('OSL_PYTHON_DONE:'.length));
    assert.equal(result.threadedQueries, 32);
    assert.equal(result.query.parameters.length, 9);
    assert(result.decoderRecovery && result.sharedOIIOTypeDesc && result.unicodeFilename);
    const overview = 'Original OSL Python query module\n\nOSL 1.15.3.0, original package and binding source\nNine parameters: scalar, string, vector, array, matrix, outputs\nMetadata and OpenImageIO TypeDesc exchange: passed\nFile, memory, search path and Unicode filename: passed\nInvalid bytecode, missing files and recovery: passed\n32 queries from Python worker threads: passed\n\nCycles OSL renderer integration remains pending.\n';
    await command(`import bpy,json,os; op_text=bpy.data.texts.new('OSL Python query results'); op_text.write(${JSON.stringify(overview)}); op_text.cursor_set(0); bpy.data.texts.new('OSL Python query proof').write(json.dumps(op_report)); bpy.ops.wm.save_as_mainfile(filepath='${directory}/osl-python.blend'); os.write(1,b'OSL_PYTHON_SAVED\\n')`, 'OSL_PYTHON_SAVED');
    const display = `def op_display():\n    area=max(bpy.context.screen.areas,key=lambda a:a.width*a.height)\n    area.type='TEXT_EDITOR'\n    area.spaces.active.text=op_text\n    area.tag_redraw()\nbpy.app.timers.register(op_display,first_interval=1.5)`;
    await command(`exec(${JSON.stringify(display)}); os.write(1,b'OSL_PYTHON_DISPLAY\\n')`, 'OSL_PYTHON_DISPLAY');
    await page.waitForTimeout(3500);
    await page.screenshot({path: path.join(output, 'osl-python-browser.png')});
    const files = {};
    for (const name of ['osl-python.blend', 'osl-python.json', 'web_query.oso', 'shader-雪.oso']) {
      const bytes = Buffer.from(await page.evaluate(filename =>
        Array.from(Module.FS.readFile(filename)), directory + '/' + name));
      fs.writeFileSync(path.join(output, name), bytes);
      files[name] = {bytes: bytes.length, sha256: hash(bytes)};
    }
    chapter('Reload offline, reopen the saved scene, and query the cached shader again');
    await context.setOffline(true); checking = false;
    await page.reload(); await ready(page, failure); checking = true;
    await page.mouse.move(300, 250);
    await command(`import bpy,json,os; bpy.ops.wm.open_mainfile(filepath='${directory}/osl-python.blend'); assert json.loads(bpy.data.texts['OSL Python query proof'].as_string())==json.loads(open('${directory}/osl-python.json').read()); os.write(1,b'OSL_PYTHON_REOPENED\\n')`, 'OSL_PYTHON_REOPENED');
    await command(`exec(${JSON.stringify(execute)})`, 'OSL_PYTHON_DONE:');
    const reopened = JSON.parse(messages.findLast(x => x.startsWith('OSL_PYTHON_DONE:')).slice('OSL_PYTHON_DONE:'.length));
    assert.deepEqual(reopened, result);
    assert.deepEqual(errors, []); assert.deepEqual(startupPythonErrors, []);
    const report = {bundleVersion: manifest.version, hardware, fallback, adapter, result,
      offlineReopen: true, browserErrors: errors, startupPythonErrors, files, chapters,
      browserVersion: browser.version(), measuredAt: new Date().toISOString(),
      sourceSha256: hash(source), fixtureSha256: hash(bytecode),
      runnerSourceSha256: runnerSha256};
    assert.equal(hash(fs.readFileSync(__filename)), runnerSha256,
      'OSL browser runner changed during validation');
    report.videoPath = path.join(output, 'osl-python.webm');
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify({bundleVersion: manifest.version, version: result.version,
      threadedQueries: result.threadedQueries, offlineReopen: true}));
  } catch (error) {
    await page.screenshot({path: path.join(output, 'failure.png')}).catch(() => {});
    throw error;
  } finally {
    const video = page.video();
    await context.close();
    if (video) await video.saveAs(path.join(output, 'osl-python.webm'));
    await browser.close();
  }
}
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
