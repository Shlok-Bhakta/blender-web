/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {PNG} = require('pngjs');
const {chromium} = require('playwright');
const {ready, consoleCommand, messages} = require('./native.cjs');
const fallback = process.env.WEB_TEST_UI_FALLBACK === '1';
const hardware = process.env.WEB_TEST_HARDWARE === '1';
const output = path.resolve(process.env.WEB_TEXT_OUTPUT || `build/native/text-evidence${hardware ? '-hardware' : ''}${fallback ? '-ui' : ''}`);
const fixtures = path.join(__dirname, 'fixtures/text');
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');

async function main() {
  fs.mkdirSync(output, {recursive:true});
  fs.writeFileSync(path.join(output, 'browser.log'), '');
  messages.length = 0;
  const browser = await chromium.launch({headless:true, args:hardware ? [
    '--enable-unsafe-webgpu', '--use-angle=vulkan', '--enable-features=Vulkan',
    '--use-vulkan=native', '--disable-vulkan-surface',
  ] : [
    '--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-angle=swiftshader',
    '--enable-features=Vulkan', '--use-vulkan=swiftshader',
  ]});
  const context = await browser.newContext({viewport:{width:1280,height:800},
    recordVideo:{dir:output, size:{width:1280,height:800}}});
  if (fallback) await context.addInitScript(() => { delete WebAssembly.promising; delete WebAssembly.Suspending; });
  const page = await context.newPage(), errors = [], chapters = [], started = Date.now();
  let rejectFailure;
  const failure = new Promise((_, reject) => rejectFailure = reject);
  failure.catch(() => {});
  const fail = text => { errors.push(text); rejectFailure(Error(text)); };
  page.on('console', message => {
    const text = message.text(); messages.push(text);
    fs.appendFileSync(path.join(output, 'browser.log'), text + '\n');
    if (/TEXT_FAILED:|Aborted\(|Blender startup failed|Blender UI frame failed|Blender GPU job failed|Blender WebGPU (error|device lost)/.test(text)) fail(text);
  });
  page.on('pageerror', error => fail(error.stack || error.message));
  const command = (code, marker) => Promise.race([failure,
    consoleCommand(page, code, marker, () => page.screenshot(), 180000)]);
  const chapter = title => chapters.push({seconds:(Date.now() - started) / 1000,title});
  const invoke = directory => `text_ns={'__file__':'${directory}/check.py','__name__':'text_check'}; exec(compile(__import__('pathlib').Path('${directory}/check.py').read_text(),'text.py','exec'),text_ns); text_result=text_ns['browser_check']('${directory}'); __import__('os').write(1,('TEXT_COMPLETE:'+__import__('json').dumps(text_result)+'\\n').encode())`;
  try {
    chapter('Start original Blender with translation catalogs');
    await page.goto(process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/');
    await ready(page, failure);
    const manifest = await page.evaluate(() => fetch('manifest.json').then(r => r.json()));
    assert.equal(manifest.buildOptions.WITH_INTERNATIONAL, true);
    const adapter = await page.evaluate(async () => {
      const adapter = await navigator.gpu.requestAdapter({powerPreference:'high-performance'});
      return {vendor:adapter.info.vendor,architecture:adapter.info.architecture,
        device:adapter.info.device,description:adapter.info.description};
    });
    if (hardware) assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
    const source = fs.readFileSync(path.join(__dirname, 'text.py'));
    const reference = JSON.parse(fs.readFileSync(path.join(fixtures, 'results.json')));
    assert.equal(reference.sourceSha256, hash(source));
    const fixtureHashes = {};
    for (const name of ['check.py', 'results.json', 'font-samples.png']) {
      const bytes = name === 'check.py' ? source : fs.readFileSync(path.join(fixtures, name));
      fixtureHashes[name] = hash(bytes);
      await page.evaluate(({name, data}) => {
        const bytes = Uint8Array.from(atob(data), c => c.charCodeAt(0));
        for (const directory of ['/tmp/text', '/user/blender-web/home/text']) {
          Module.FS.mkdirTree(directory); Module.FS.writeFile(directory + '/' + name, bytes);
        }
      }, {name, data:bytes.toString('base64')});
    }
    chapter('Compare six languages and four font samples with native Blender');
    await page.mouse.move(300, 250);
    await command(invoke('/tmp/text'), 'TEXT_COMPLETE:');
    const report = JSON.parse(messages.find(text => text.startsWith('TEXT_COMPLETE:')).slice('TEXT_COMPLETE:'.length));
    assert(report.buildOptions.international);
    assert.deepEqual(report.translations, reference.translations);
    assert.deepEqual(report.catalogs, reference.catalogs);
    assert.deepEqual(report.availableLanguages, reference.availableLanguages);
    for (let i=0; i<report.fonts.length; i++) {
      assert.equal(report.fonts[i].fontSha256, reference.fonts[i].fontSha256);
      for (let j=0; j<2; j++) assert(Math.abs(report.fonts[i].dimensions[j] - reference.fonts[i].dimensions[j]) < .02);
    }
    const pixels = Buffer.from(await page.evaluate(() => Array.from(Module.FS.readFile('/tmp/text/font-samples.png'))));
    fs.writeFileSync(path.join(output, 'font-samples.png'), pixels);
    const actual = PNG.sync.read(pixels), expected = PNG.sync.read(fs.readFileSync(path.join(fixtures, 'font-samples.png')));
    assert.equal(actual.width, expected.width); assert.equal(actual.height, expected.height);
    let maximum = 0, total = 0, ink = 0;
    for (let i=0; i<actual.data.length; i++) {
      const error = Math.abs(actual.data[i] - expected.data[i]);
      maximum = Math.max(maximum, error); total += error;
      if (i%4===3 && actual.data[i]) ink++;
    }
    report.fontPixels = {maximumError:maximum, meanError:total/actual.data.length, nonzeroAlpha:ink};
    assert(maximum <= 4 && report.fontPixels.meanError < .05 && ink > 1000, JSON.stringify(report.fontPixels));
    chapter('Switch the original interface to French');
    await command("bpy.context.preferences.view.language='fr_FR'; __import__('os').write(1,b'TEXT_FRENCH\\n')", 'TEXT_FRENCH');
    await page.screenshot({path:path.join(output, 'french-interface.png')});
    chapter('Type Unicode characters through native GHOST keyboard events');
    const editor = `def text_editor():\n    area=max((a for a in bpy.context.screen.areas if a.type in {'CONSOLE','VIEW_3D'}),key=lambda a:a.width*a.height)\n    area.type='TEXT_EDITOR'\n    area.spaces.active.text=bpy.data.texts.new('Unicode key proof')\n    area.tag_redraw()\n    __import__('os').write(1,b'TEXT_EDITOR_READY\\n')\nbpy.app.timers.register(text_editor,first_interval=1.5)`;
    await command(`exec(${JSON.stringify(editor)}); __import__('os').write(1,b'TEXT_EDITOR_SCHEDULED\\n')`, 'TEXT_EDITOR_SCHEDULED');
    const editorDeadline = Date.now() + 30000;
    while (!messages.includes('TEXT_EDITOR_READY')) {
      if (Date.now() > editorDeadline) throw new Error('Text Editor did not become ready');
      await page.waitForTimeout(100);
    }
    await page.locator('canvas').focus();
    // Changing editor type under a stationary pointer does not establish its
    // active native region. Focus the text body before sending trusted keys.
    await page.mouse.click(100, 100);
    await page.screenshot();
    await page.evaluate(() => {
      window.textKeyProof = [];
      window.addEventListener('keydown', event => window.textKeyProof.push({key:event.key,
        code:event.code,trusted:event.isTrusted}), {capture:true});
    });
    const cdp = await context.newCDPSession(page);
    const input = 'Café 日本 😀';
    for (const key of input) {
      const code = key === ' ' ? 'Space' : 'KeyE';
      await cdp.send('Input.dispatchKeyEvent', {type:'keyDown',key,code,text:key,unmodifiedText:key});
      await cdp.send('Input.dispatchKeyEvent', {type:'keyUp',key,code});
    }
    await cdp.send('Input.dispatchKeyEvent', {type:'keyDown',key:'Dead',code:'Quote'});
    await cdp.send('Input.dispatchKeyEvent', {type:'keyUp',key:'Dead',code:'Quote'});
    await page.waitForTimeout(1000);
    const keyEvents = await page.evaluate(() => window.textKeyProof);
    assert.deepEqual(keyEvents.map(event => event.key), [...input, 'Dead']);
    assert(keyEvents.every(event => event.trusted));
    await page.screenshot({path:path.join(output, 'unicode-keyboard.png')});
    await command("__import__('os').write(1,('TEXT_TYPED:'+__import__('json').dumps(bpy.data.texts['Unicode key proof'].as_string())+'\\n').encode())", 'TEXT_TYPED:');
    const typed = JSON.parse(messages.find(text => text.startsWith('TEXT_TYPED:')).slice('TEXT_TYPED:'.length));
    assert.equal(typed, input);
    await command("bpy.ops.wm.save_as_mainfile(filepath='/user/blender-web/home/text/unicode.blend'); bpy.ops.wm.save_userpref(); __import__('os').write(1,b'TEXT_SAVED\\n')", 'TEXT_SAVED');
    chapter('Switch to Japanese and display the native font samples');
    const display = `def text_display():\n    area=max((a for a in bpy.context.screen.areas if a.type in {'CONSOLE','VIEW_3D'}),key=lambda a:a.width*a.height)\n    area.type='IMAGE_EDITOR'\n    area.spaces.active.image=bpy.data.images.load('/tmp/text/font-samples.png')\n    area.tag_redraw()\nbpy.context.preferences.view.language='ja_JP'\nbpy.ops.wm.save_userpref()\nbpy.app.timers.register(text_display,first_interval=1.5)`;
    await command(`exec(${JSON.stringify(display)}); __import__('os').write(1,b'TEXT_JAPANESE_SCHEDULED\\n')`, 'TEXT_JAPANESE_SCHEDULED');
    await page.waitForTimeout(2500);
    await page.screenshot({path:path.join(output, 'japanese-interface.png')});
    chapter('Reload offline, restore Japanese preferences and reopen Unicode text');
    await context.setOffline(true); await page.reload(); await ready(page, failure);
    await page.mouse.move(300, 250);
    await command("__import__('os').write(1,('TEXT_LANGUAGE:'+bpy.context.preferences.view.language+'\\n').encode())", 'TEXT_LANGUAGE:');
    assert(messages.includes('TEXT_LANGUAGE:ja_JP'));
    const before = messages.length;
    await command(invoke('/user/blender-web/home/text'), 'TEXT_COMPLETE:');
    const offline = JSON.parse(messages.slice(before).find(text => text.startsWith('TEXT_COMPLETE:')).slice('TEXT_COMPLETE:'.length));
    assert.deepEqual(offline.translations, report.translations);
    await command("bpy.ops.wm.open_mainfile(filepath='/user/blender-web/home/text/unicode.blend'); __import__('os').write(1,('TEXT_REOPENED:'+__import__('json').dumps(bpy.data.texts['Unicode key proof'].as_string())+'\\n').encode())", 'TEXT_REOPENED:');
    const reopened = JSON.parse(messages.find(text => text.startsWith('TEXT_REOPENED:')).slice('TEXT_REOPENED:'.length));
    assert.equal(reopened, input);
    await page.screenshot({path:path.join(output, 'offline-interface.png')});
    Object.assign(report, {bundleVersion:manifest.version,hardware,fallback,adapter,
      unicodeInput:{expected:input,typed,reopened,deadKeyIgnored:true,
        trustedBrowserEvents:keyEvents.every(event => event.trusted),keyEvents},
      offlineReopen:true,preferencesRestored:true,browserErrors:errors,chapters,fixtureHashes,
      videoPath:path.join(output, 'text.webm')});
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify({bundleVersion:manifest.version,languages:Object.keys(report.translations),
      fontPixels:report.fontPixels,unicodeInput:report.unicodeInput,offlineReopen:true}));
  }
  catch (error) { await page.screenshot({path:path.join(output, 'failure.png')}).catch(() => {}); throw error; }
  finally {
    const video=page.video(); await context.close();
    if (video) await video.saveAs(path.join(output, 'text.webm'));
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode=1; });
