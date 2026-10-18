/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {chromium} = require('playwright');
const {ready, consoleCommand, messages} = require('./native.cjs');
const uiFallback = process.env.WEB_TEST_UI_FALLBACK === '1';
const output = path.resolve(uiFallback ? 'build/native/feature-evidence-ui' : 'build/native/feature-evidence');

async function main() {
  fs.mkdirSync(output, {recursive:true});
  messages.length = 0;
  const browser = await chromium.launch({headless:true, args:[
    '--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-angle=swiftshader',
    '--enable-features=Vulkan', '--use-vulkan=swiftshader',
  ]});
  const errors = [];
  const context = await browser.newContext({viewport:{width:1280,height:800}});
  if (uiFallback) await context.addInitScript(() => {
    delete WebAssembly.promising;
    delete WebAssembly.Suspending;
  });
  const page = await context.newPage();
  const tracing = process.env.WEB_FEATURE_PROFILE === '1' ? await browser.newBrowserCDPSession() : null;
  const log = fs.openSync(path.join(output,'browser.log'),'w');
  let report, failure, tracingStarted=false;
  page.on('console', message => {
    const text = message.text(); messages.push(text); fs.writeSync(log,text+'\n');
    if (text.startsWith('FEATURE_COMPLETE:')) report = JSON.parse(text.slice(17));
    if (text.startsWith('FEATURE_FAILED:')) failure = text;
    if (/Aborted\(|Blender WebGPU error|Blender WebGPU device lost|Blender startup failed/.test(text)) errors.push(text);
  });
  page.on('pageerror', e => errors.push(e.stack));
  try {
    await page.goto(process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/');
    await ready(page, new Promise(()=>{}));
    assert.equal(await page.evaluate(()=>document.documentElement.dataset.blenderVariant),uiFallback ? 'ui' : 'render');
    const manifest = await page.evaluate(async()=>fetch('manifest.json').then(r=>r.json()));
    for (const flag of ['WITH_BULLET','WITH_IK_SOLVER','WITH_IK_ITASC','WITH_IO_WAVEFRONT_OBJ','WITH_IO_STL','WITH_IO_PLY','WITH_IO_FBX']) {
      assert.equal(manifest.buildOptions[flag],true,flag);
    }
    await page.evaluate(source=>Module.FS.writeFile('/tmp/web-features.py',source),
      fs.readFileSync(path.join(__dirname,'features.py'),'utf8'));
    await page.mouse.move(300,250);
    if (tracing) {
      await tracing.send('Tracing.start', {
        categories:'v8,disabled-by-default-v8.cpu_profiler',transferMode:'ReturnAsStream',
      });
      tracingStarted=true;
    }
    // Always emit a completion marker so a Python assertion reports immediately.
    await consoleCommand(page,"exec(compile(open('/tmp/web-features.py').read(), '/tmp/web-features.py', 'exec'))",'FEATURE_DONE:');
    assert(!failure,failure);
    assert(report,'Native Python must finish all feature checks');
    const base64 = await page.evaluate(file=>{
      const bytes=Module.FS.readFile(file), pieces=[];
      for(let offset=0;offset<bytes.length;offset+=32768) pieces.push(String.fromCharCode(...bytes.subarray(offset,offset+32768)));
      return btoa(pieces.join(''));
    },'/tmp/web-features/feature-scene.blend');
    const blend=Buffer.from(base64,'base64');
    assert.equal(blend.length,report.blendFile.bytes);
    const zstd=blend.readUInt32LE(0)===0xfd2fb528;
    assert(zstd || blend.subarray(0,7).toString()==='BLENDER','Expected a native or Zstd-compressed blend file');
    report.blendFile.compression=zstd ? 'zstd' : 'none';
    report.blendFile.sha256=crypto.createHash('sha256').update(blend).digest('hex');
    fs.writeFileSync(path.join(output,'feature-scene.blend'),blend);
    await page.screenshot({path:path.join(output,'physics-scene.png')});
    // Start a new browser page while preserving OPFS, then read the saved native file.
    await page.reload();
    await ready(page,new Promise(()=>{}));
    await page.mouse.move(300,250);
    await consoleCommand(page,"import bpy; bpy.ops.wm.open_mainfile(filepath='/user/blender-web/home/web-feature-scene.blend'); __import__('os').write(1,b'FEATURE_BLEND_OPEN\\n')",'FEATURE_BLEND_OPEN');
    await page.mouse.move(300,250);
    await consoleCommand(page,"import bpy, json, os; os.write(1, ('FEATURE_REOPEN:'+json.dumps({'objects':sorted(o.name for o in bpy.context.scene.objects),'bullet':bpy.context.scene.rigidbody_world is not None,'active':bpy.data.objects['WebFallingCube'].rigid_body.type,'passive':bpy.data.objects['WebFloor'].rigid_body.type})+'\\n').encode())",'FEATURE_REOPEN:');
    const reopened = JSON.parse(messages.findLast(t=>t.startsWith('FEATURE_REOPEN:')).slice(15));
    assert.deepEqual(reopened.objects,report.blendFile.expectedObjects);
    assert(reopened.bullet);
    assert.equal(reopened.active,'ACTIVE'); assert.equal(reopened.passive,'PASSIVE');
    assert.deepEqual(errors,[]);
    report.blendFile.reopenedAfterBrowserReload = true;
    report.bundleVersion = manifest.version;
    report.browserVersion = browser.version();
    report.measuredAt = new Date().toISOString();
    report.variant = uiFallback ? 'ui' : 'render';
    report.wasmBytes = manifest.files.find(file=>file.name.endsWith(uiFallback ? '/blender-ui.wasm' : '/blender.wasm')).bytes;
    report.browserErrors = errors.length;
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    await page.screenshot({path:path.join(output,'reopened-scene.png')});
    console.log(JSON.stringify(report));
  } catch (e) {
    await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});
    throw new Error(failure || e.stack);
  } finally {
    if (tracingStarted) {
      const completed=new Promise(resolve=>tracing.once('Tracing.tracingComplete',resolve));
      await tracing.send('Tracing.end');
      const {stream}=await completed;
      const file=fs.openSync(path.join(output,'browser-trace.json'),'w');
      for (;;) {
        const part=await tracing.send('IO.read',{handle:stream});
        fs.writeSync(file,part.base64Encoded ? Buffer.from(part.data,'base64') : part.data);
        if (part.eof) break;
      }
      fs.closeSync(file); await tracing.send('IO.close',{handle:stream});
    }
    if (tracing) await tracing.detach();
    await browser.close(); fs.closeSync(log);
  }
}
main().catch(e=>{console.error(e);process.exitCode=1;});
