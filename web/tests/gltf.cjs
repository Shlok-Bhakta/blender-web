/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {chromium} = require('playwright');
const {ready, consoleCommand, messages} = require('./native.cjs');
const fallback = process.env.WEB_TEST_UI_FALLBACK === '1';
const hardware = process.env.WEB_TEST_HARDWARE === '1';
const developmentOverlay = process.env.WEB_GLTF_PYTHON_OVERLAY === '1';
const output = path.resolve(process.env.WEB_GLTF_OUTPUT || `build/native/gltf-evidence${hardware ? '-hardware' : ''}${fallback ? '-ui' : ''}`);
const fixtures = path.join(__dirname, 'fixtures/gltf');
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const directory = '/user/blender-web/home/gltf';

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
    recordVideo:{dir:output,size:{width:1280,height:800}}});
  if (fallback) await context.addInitScript(() => { delete WebAssembly.promising; delete WebAssembly.Suspending; });
  const page = await context.newPage(), errors = [], chapters = [], started = Date.now();
  let rejectFailure;
  const failure = new Promise((_, reject) => rejectFailure = reject);
  failure.catch(() => {});
  const fail = text => { errors.push(text); rejectFailure(Error(text)); };
  page.on('console', message => {
    const text = message.text(); messages.push(text);
    fs.appendFileSync(path.join(output,'browser.log'),text+'\n');
    if (/GLTF_FAILED:|Aborted\(|Blender startup failed|Blender UI frame failed|Blender WebGPU (error|device lost)/.test(text)) fail(text);
  });
  page.on('pageerror', error => fail(error.stack || error.message));
  const command = (code, marker) => Promise.race([failure,
    consoleCommand(page, code, marker, () => page.screenshot(),180000)]);
  const chapter = title => chapters.push({seconds:(Date.now()-started)/1000,title});
  const emit = (name, expression) => `__import__('os').write(1,('${name}:'+__import__('json').dumps(${expression})+'\\n').encode())`;
  const result = name => JSON.parse(messages.find(text => text.startsWith(name+':')).slice(name.length+1));
  try {
    chapter('Start Blender with static Draco and meshoptimizer codecs');
    await page.goto(process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/');
    await ready(page,failure);
    if (developmentOverlay) {
      // Test Python-only changes while the release linker runs. Release
      // acceptance explicitly rejects these development-only runs.
      for (const [local, target] of [
        ['web/native/ghost/GHOST_WebGLTF.py','/5.2/scripts/modules/_bpy_ghost_web_gltf.py'],
        ['build/native/source/scripts/addons_core/io_scene_gltf2/io/exp/buffer.py',
         '/5.2/scripts/addons_core/io_scene_gltf2/io/exp/buffer.py'],
        ['build/native/source/scripts/addons_core/io_scene_gltf2/blender/exp/animation/sampled/object/sampler.py',
         '/5.2/scripts/addons_core/io_scene_gltf2/blender/exp/animation/sampled/object/sampler.py'],
      ]) {
        const data=fs.readFileSync(local).toString('base64');
        await page.evaluate(({target,data})=>Module.FS.writeFile(target,Uint8Array.from(atob(data),c=>c.charCodeAt(0))),{target,data});
      }
    }
    const manifest = await page.evaluate(() => fetch('manifest.json').then(r=>r.json()));
    assert.equal(manifest.buildOptions.WITH_DRACO,true);
    assert.equal(manifest.buildOptions.WITH_MESHOPTIMIZER,true);
    const adapter = await page.evaluate(async()=>{
      const a=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});
      return {vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description};
    });
    if(hardware) assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
    const source=fs.readFileSync(path.join(__dirname,'gltf.py'));
    const reference=JSON.parse(fs.readFileSync(path.join(fixtures,'results.json')));
    assert.equal(reference.sourceSha256,hash(source));
    const fixtureHashes={};
    for(const name of ['check.py','bridge-check.py','results.json',...Object.keys(reference.cases).map(n=>n+'.glb')]) {
      const bytes=name==='check.py'?source:name==='bridge-check.py'?fs.readFileSync(path.join(__dirname,'../native/tests/gltf.py')):fs.readFileSync(path.join(fixtures,name));
      fixtureHashes[name]=hash(bytes);
      await page.evaluate(({name,data,directory})=>{
        Module.FS.mkdirTree(directory);
        Module.FS.writeFile(directory+'/'+name,Uint8Array.from(atob(data),c=>c.charCodeAt(0)));
      },{name,directory,data:bytes.toString('base64')});
    }
    await page.mouse.move(300,250);
    chapter('Reject malformed buffers and test codec lifetime cleanup');
    await command(`bridge_ns=__import__('runpy').run_path('${directory}/bridge-check.py'); ${emit('GLTF_BRIDGE',"bridge_ns['check']()")}`,'GLTF_BRIDGE:');
    const bridge=result('GLTF_BRIDGE');
    await command(emit('GLTF_COLOR',"bridge_ns['addon_color_check']()"),'GLTF_COLOR:');
    const colorFilter=result('GLTF_COLOR');
    assert.equal(colorFilter.knownColors,4); assert(colorFilter.maximumUnorm8Error<=2);
    assert(bridge.rejectedCases.length>=12 && bridge.truncatedDataRejected);
    chapter('Reject malformed GLBs, then import and export Draco, EXT and KHR meshoptimizer');
    await command(`gltf_ns={'__file__':'${directory}/check.py','__name__':'gltf_check'}; exec(compile(__import__('pathlib').Path('${directory}/check.py').read_text(),'gltf.py','exec'),gltf_ns); gltf_result=gltf_ns['browser_check']('${directory}'); ${emit('GLTF_COMPLETE','gltf_result')}`,'GLTF_COMPLETE:');
    const report=result('GLTF_COMPLETE');
    const exports={};
    for(const [caseName, entry] of Object.entries(report.cases)) {
      assert(entry.nativeImportError<.005 && entry.roundtripError<.005);
      const name='browser-'+caseName+'.glb';
      const bytes=Buffer.from(await page.evaluate(name=>Array.from(Module.FS.readFile(name)),directory+'/'+name));
      assert.equal(hash(bytes),entry.sha256);
      fs.writeFileSync(path.join(output,name),bytes);
      exports[name]={bytes:bytes.length,sha256:hash(bytes)};
    }
    const blend=Buffer.from(await page.evaluate(directory=>Array.from(Module.FS.readFile(directory+'/compressed-assets.blend')),directory));
    fs.writeFileSync(path.join(output,'compressed-assets.blend'),blend);
    chapter('Inspect imported geometry and animation in the original Blender viewport');
    const display=`def gltf_display():\n    area=max(bpy.context.screen.areas,key=lambda a:a.width*a.height)\n    area.type='VIEW_3D'\n    area.spaces.active.shading.color_type='VERTEX'\n    region=next(r for r in area.regions if r.type=='WINDOW')\n    with bpy.context.temp_override(area=area,region=region):\n        bpy.ops.view3d.view_all(center=True)\n    bpy.context.scene.frame_set(24)\n    area.tag_redraw()\nbpy.app.timers.register(gltf_display,first_interval=1.5)`;
    await command(`exec(${JSON.stringify(display)}); __import__('os').write(1,b'GLTF_DISPLAY\\n')`,'GLTF_DISPLAY');
    await page.waitForTimeout(3000);
    await page.screenshot({path:path.join(output,'compressed-assets.png')});
    chapter('Reload offline and reopen compressed assets from persistent storage');
    await context.setOffline(true); await page.reload(); await ready(page,failure);
    await page.mouse.move(300,250);
    await command(`gltf_ns={'__file__':'${directory}/check.py','__name__':'gltf_check'}; exec(compile(__import__('pathlib').Path('${directory}/check.py').read_text(),'gltf.py','exec'),gltf_ns); bpy.ops.wm.open_mainfile(filepath='${directory}/compressed-assets.blend'); ${emit('GLTF_REOPENED',"gltf_ns['snapshot']()")}`,'GLTF_REOPENED:');
    const reopened=result('GLTF_REOPENED');
    assert.deepEqual(reopened,report.saved);
    Object.assign(report,{bundleVersion:manifest.version,hardware,fallback,developmentOverlay,adapter,bridge,colorFilter,fixtureHashes,
      offlineReopen:true,reopened,exports,browserErrors:errors,chapters,
      referenceSha256:hash(fs.readFileSync(path.join(fixtures,'results.json'))),
      blend:{bytes:blend.length,sha256:hash(blend)},videoPath:path.join(output,'gltf.webm')});
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify({bundleVersion:manifest.version,cases:Object.keys(report.cases),rejectedCases:bridge.rejectedCases.length,offlineReopen:true}));
  }
  catch(error) { await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{}); throw error; }
  finally {
    const video=page.video(); await context.close();
    if(video) await video.saveAs(path.join(output,'gltf.webm'));
    await browser.close();
  }
}
main().catch(error=>{console.error(error);process.exitCode=1;});
