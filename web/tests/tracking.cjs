/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {chromium} = require('playwright');
const {ready, consoleCommand, messages} = require('./native.cjs');
const fallback = process.env.WEB_TEST_UI_FALLBACK === '1';
const hardware = process.env.WEB_TEST_HARDWARE === '1';
const output = path.resolve(process.env.WEB_TRACKING_OUTPUT || `build/native/tracking-evidence${hardware ? '-hardware' : ''}${fallback ? '-ui' : ''}`);
const fixtures = path.join(__dirname, 'fixtures/tracking');
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const directory = '/user/blender-web/home/tracking';

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
    if (/TRACKING_FAILED:|Aborted\(|Blender startup failed|Blender UI frame failed|Blender WebGPU (error|device lost)/.test(text)) fail(text);
  });
  page.on('pageerror', error => fail(error.stack || error.message));
  const command = (code, marker) => Promise.race([failure,
    consoleCommand(page, code, marker, () => page.screenshot(), 180000)]);
  const chapter = title => chapters.push({seconds:(Date.now() - started) / 1000,title});
  const emit = (name, expression) => `__import__('os').write(1,('${name}:'+__import__('json').dumps(${expression})+'\\n').encode())`;
  const result = name => JSON.parse(messages.find(text => text.startsWith(name + ':')).slice(name.length+1));
  try {
    chapter('Start original Blender with libmv and Ceres');
    await page.goto(process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/');
    await ready(page, failure);
    const manifest = await page.evaluate(() => fetch('manifest.json').then(r => r.json()));
    assert.equal(manifest.buildOptions.WITH_LIBMV, true);
    const adapter = await page.evaluate(async () => {
      const a = await navigator.gpu.requestAdapter({powerPreference:'high-performance'});
      return {vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description};
    });
    if (hardware) assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
    const source = fs.readFileSync(path.join(__dirname, 'tracking.py'));
    const reference = JSON.parse(fs.readFileSync(path.join(fixtures, 'results.json')));
    assert.equal(reference.sourceSha256, hash(source));
    const fixtureHashes = {};
    for (const name of ['check.py', ...Object.keys(reference.fixtureHashes)]) {
      const bytes = name === 'check.py' ? source : fs.readFileSync(path.join(fixtures, name));
      fixtureHashes[name] = hash(bytes);
      if (name !== 'check.py') assert.equal(hash(bytes), reference.fixtureHashes[name]);
      await page.evaluate(({name, data, directory}) => {
        Module.FS.mkdirTree(directory);
        Module.FS.writeFile(directory + '/' + name, Uint8Array.from(atob(data), c => c.charCodeAt(0)));
      }, {name, directory, data:bytes.toString('base64')});
    }
    await page.mouse.move(300, 250);
    chapter('Track known pixel motion and solve twelve cameras from thirty-two points');
    await command(`tracking_ns={'__file__':'${directory}/check.py','__name__':'tracking_check'}; exec(compile(__import__('pathlib').Path('${directory}/check.py').read_text(),'tracking.py','exec'),tracking_ns); tracking_result=tracking_ns['browser_check']('${directory}'); ${emit('TRACKING_COMPLETE', 'tracking_result')}`, 'TRACKING_COMPLETE:');
    const report = result('TRACKING_COMPLETE');
    assert(report.libmv && report.solve.valid);
    assert.equal(report.solve.cameras, 12); assert.equal(report.solve.bundles, 32);
    assert(report.solve.averageErrorPixels < .05 && report.maximumTrackErrorPixels < .15);
    assert(report.solve.maximumTrajectoryRatioError < .01);
    const maximumNativeTrajectoryError = Math.max(...report.solve.cameraDistanceRatios.map(
      (ratio, i) => Math.abs(ratio - reference.solve.cameraDistanceRatios[i])));
    assert(maximumNativeTrajectoryError < .001, maximumNativeTrajectoryError);
    assert.equal(report.markers.length, reference.markers.length);
    let maximumNativeError = 0;
    for (let i=0; i<report.markers.length; i++) {
      assert.equal(report.markers[i].frame, reference.markers[i].frame);
      const a = report.markers[i].co, b = reference.markers[i].co;
      maximumNativeError = Math.max(maximumNativeError, Math.hypot((a[0]-b[0])*256,(a[1]-b[1])*192));
    }
    assert(maximumNativeError < .05, maximumNativeError);
    await command(`bpy.ops.wm.save_as_mainfile(filepath='${directory}/tracking.blend'); ${emit('TRACKING_SAVED', "tracking_ns['saved_check']()")}`, 'TRACKING_SAVED:');
    const saved = result('TRACKING_SAVED');
    const blend = Buffer.from(await page.evaluate(directory => Array.from(Module.FS.readFile(directory + '/tracking.blend')), directory));
    fs.writeFileSync(path.join(output, 'tracking.blend'), blend);
    chapter('Inspect tracked footage in Blender’s original Movie Clip Editor');
    const display = `def tracking_display():\n    area=max(bpy.context.screen.areas,key=lambda a:a.width*a.height)\n    area.type='CLIP_EDITOR'\n    area.spaces.active.clip=bpy.data.movieclips['Tracking pixel motion']\n    area.spaces.active.show_marker_pattern=True\n    region=next(r for r in area.regions if r.type=='WINDOW')\n    with bpy.context.temp_override(area=area,region=region):\n        bpy.ops.clip.view_all(fit_view=True)\n    bpy.context.scene.frame_set(12)\n    area.tag_redraw()\nbpy.app.timers.register(tracking_display,first_interval=1.5)`;
    await command(`exec(${JSON.stringify(display)}); __import__('os').write(1,b'TRACKING_DISPLAY\\n')`, 'TRACKING_DISPLAY');
    await page.waitForTimeout(3000);
    await page.screenshot({path:path.join(output, 'tracked-footage.png')});
    chapter('Reload offline and reopen the saved tracks and reconstructed cameras');
    await context.setOffline(true); await page.reload(); await ready(page, failure);
    await page.mouse.move(300, 250);
    await command(`tracking_ns={'__file__':'${directory}/check.py','__name__':'tracking_check'}; exec(compile(__import__('pathlib').Path('${directory}/check.py').read_text(),'tracking.py','exec'),tracking_ns); bpy.ops.wm.open_mainfile(filepath='${directory}/tracking.blend'); ${emit('TRACKING_REOPENED', "tracking_ns['saved_check']()")}`, 'TRACKING_REOPENED:');
    const reopened = result('TRACKING_REOPENED');
    assert.deepEqual(reopened, saved);
    await page.screenshot({path:path.join(output, 'offline-tracking.png')});
    Object.assign(report, {bundleVersion:manifest.version,hardware,fallback,adapter,
      maximumNativeMarkerErrorPixels:maximumNativeError,offlineReopen:true,saved,reopened,
      maximumNativeTrajectoryRatioError:maximumNativeTrajectoryError,
      browserErrors:errors,chapters,fixtureHashes,referenceSha256:hash(fs.readFileSync(path.join(fixtures,'results.json'))),
      blend:{bytes:blend.length,sha256:hash(blend)},videoPath:path.join(output,'tracking.webm')});
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify({bundleVersion:manifest.version,tracking:report.maximumTrackErrorPixels,
      nativeMarkerError:maximumNativeError,solve:report.solve,offlineReopen:true}));
  }
  catch (error) { await page.screenshot({path:path.join(output, 'failure.png')}).catch(() => {}); throw error; }
  finally {
    const video=page.video(); await context.close();
    if (video) await video.saveAs(path.join(output,'tracking.webm'));
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode=1; });
