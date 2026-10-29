/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {chromium} = require('playwright');
const {ready, consoleCommand, messages} = require('./native.cjs');
const hardware = process.env.WEB_TEST_HARDWARE === '1';
const fallback = process.env.WEB_TEST_UI_FALLBACK === '1';
const output = path.resolve(process.env.WEB_CTYPES_OUTPUT ||
  `build/native/python-ctypes-evidence${hardware ? '-hardware' : ''}${fallback ? '-ui' : ''}`);
const directory = '/user/blender-web/home/python-ctypes';
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');

async function main() {
  fs.mkdirSync(output, {recursive: true});
  messages.length = 0;
  fs.writeFileSync(path.join(output, 'browser.log'), '');
  const browser = await chromium.launch({headless: true, args: hardware ? [
    '--enable-unsafe-webgpu', '--use-angle=vulkan', '--enable-features=Vulkan',
    '--use-vulkan=native', '--disable-vulkan-surface',
  ] : [
    '--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-angle=swiftshader',
    '--enable-features=Vulkan', '--use-vulkan=swiftshader',
  ]});
  const context = await browser.newContext({viewport: {width: 1280, height: 800},
    recordVideo: {dir: output, size: {width: 1280, height: 800}}});
  if (fallback) await context.addInitScript(() => {
    delete WebAssembly.promising;
    delete WebAssembly.Suspending;
  });
  const page = await context.newPage();
  const errors = [], startupPythonErrors = [], chapters = [], started = Date.now();
  let checking = false, rejectFailure;
  const failure = new Promise((_, reject) => rejectFailure = reject);
  failure.catch(() => {});
  function fail(text) {
    errors.push(text);
    rejectFailure(Error(text));
  }
  page.on('console', message => {
    const text = message.text();
    messages.push(text);
    fs.appendFileSync(path.join(output, 'browser.log'), text + '\n');
    if (/Traceback \(most recent call last\)/.test(text)) {
      if (checking) fail(text);
      else startupPythonErrors.push(text);
    }
    if (/CTYPES_FAILED:|Aborted\(|GHOST Web libffi:|Blender startup failed|Blender UI frame failed|Blender GPU job failed|Blender WebGPU (error|device lost)/.test(text)) fail(text);
  });
  page.on('pageerror', error => fail(error.stack || error.message));
  page.on('crash', () => fail('Chromium crashed during the ctypes checks'));
  const command = (code, marker) => Promise.race([
    failure, consoleCommand(page, code, marker, () => page.screenshot(), 300000),
  ]);
  const chapter = title => chapters.push({seconds: (Date.now() - started) / 1000, title});
  try {
    chapter('Start original Blender with the original ctypes module');
    await page.goto(process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/');
    await ready(page, failure);
    checking = true;
    const manifest = await page.evaluate(() => fetch('manifest.json').then(r => r.json()));
    const adapter = await page.evaluate(async () => {
      const value = await navigator.gpu.requestAdapter({powerPreference: 'high-performance'});
      return {vendor: value.info.vendor, architecture: value.info.architecture,
        device: value.info.device, description: value.info.description};
    });
    if (hardware) assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
    const source = fs.readFileSync(path.join(__dirname, '../native/tests/python-ctypes.py'));
    await page.evaluate(({data, directory}) => {
      Module.FS.mkdirTree(directory);
      Module.FS.writeFile(directory + '/check.py', Uint8Array.from(atob(data), c => c.charCodeAt(0)));
    }, {data: source.toString('base64'), directory});
    await page.mouse.move(300, 250);
    chapter('Test Python APIs, variadic calls, NumPy and 248 worker callbacks');
    const execute = `try:\n    ctypes_report=__import__('runpy').run_path('${directory}/check.py')['run']()\n    __import__('pathlib').Path('${directory}/report.json').write_text(__import__('json').dumps(ctypes_report))\n    __import__('os').write(1,b'CTYPES_DONE\\n')\nexcept BaseException:\n    __import__('os').write(2,('CTYPES_FAILED:'+__import__('traceback').format_exc()+'\\n').encode())`;
    await command(`exec(${JSON.stringify(execute)})`, 'CTYPES_DONE');
    await command(`import bpy,json,os; ctypes_text=bpy.data.texts.new('Original ctypes results'); ctypes_text.write(json.dumps(ctypes_report,indent=2)); ctypes_text.cursor_set(0); bpy.ops.wm.save_as_mainfile(filepath='${directory}/ctypes.blend'); os.write(1,b'CTYPES_SAVED\\n')`, 'CTYPES_SAVED');
    const report = JSON.parse(await page.evaluate(name => new TextDecoder().decode(Module.FS.readFile(name)), directory + '/report.json'));
    assert.equal(report.reusedWorkerCallbacks, 240);
    assert.equal(report.pythonThreadCallbacks, 8);
    assert.equal(report.pointerBytes, 4);
    assert(report.originalCtypes && report.pythonAPIErrorRecovery && report.numpyCtypesInterop);
    assert.deepEqual(report.unraisableErrors, []);
    fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
    const blend = Buffer.from(await page.evaluate(name => Array.from(Module.FS.readFile(name)), directory + '/ctypes.blend'));
    fs.writeFileSync(path.join(output, 'ctypes.blend'), blend);
    chapter('Inspect the result in the original Blender Text Editor');
    const display = `def ctypes_display():\n    area=max(bpy.context.screen.areas,key=lambda a:a.width*a.height)\n    area.type='TEXT_EDITOR'\n    area.spaces.active.text=ctypes_text\n    area.tag_redraw()\nbpy.app.timers.register(ctypes_display,first_interval=1.5)`;
    await command(`exec(${JSON.stringify(display)}); os.write(1,b'CTYPES_DISPLAY\\n')`, 'CTYPES_DISPLAY');
    await page.waitForTimeout(4000);
    await page.screenshot({path: path.join(output, 'ctypes-browser.png')});
    chapter('Reopen the saved result offline and repeat callback checks');
    await context.setOffline(true);
    checking = false;
    await page.reload();
    await ready(page, failure);
    checking = true;
    await page.mouse.move(300, 250);
    const reopen = `import bpy,json,os; bpy.ops.wm.open_mainfile(filepath='${directory}/ctypes.blend'); ctypes_saved=json.loads(bpy.data.texts['Original ctypes results'].as_string()); assert __import__('runpy').run_path('${directory}/check.py')['run']()==ctypes_saved; os.write(1,('CTYPES_REOPENED:'+json.dumps(ctypes_saved)+'\\n').encode())`;
    await command(reopen, 'CTYPES_REOPENED:');
    const reopened = JSON.parse(messages.findLast(text => text.startsWith('CTYPES_REOPENED:')).slice('CTYPES_REOPENED:'.length));
    assert.deepEqual(reopened, report);
    assert.deepEqual(errors, []);
    assert.deepEqual(startupPythonErrors, []);
    const acceptance = {bundleVersion: manifest.version, sourceSha256: hash(source),
      runnerSourceSha256: hash(fs.readFileSync(__filename)), hardware, fallback, adapter,
      browserVersion: browser.version(), measuredAt: new Date().toISOString(), report,
      blend: {bytes: blend.length, sha256: hash(blend)}, offlineReopen: true,
      offlineRepeatedChecks: true, browserErrors: errors, startupPythonErrors, chapters,
      videoPath: path.join(output, 'python-ctypes.webm')};
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(acceptance, null, 2) + '\n');
    console.log(JSON.stringify({bundleVersion: manifest.version, workerCallbacks: 248, offlineReopen: true}));
  } catch (error) {
    await page.screenshot({path: path.join(output, 'failure.png')}).catch(() => {});
    throw error;
  } finally {
    const video = page.video();
    await context.close();
    if (video) await video.saveAs(path.join(output, 'python-ctypes.webm'));
    await browser.close();
  }
}
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
