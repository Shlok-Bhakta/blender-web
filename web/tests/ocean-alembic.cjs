/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');
const {chromium}=require('playwright');
const {ready,consoleCommand,messages}=require('./native.cjs');
const fallback=process.env.WEB_TEST_UI_FALLBACK==='1';
const hardware=process.env.WEB_TEST_HARDWARE==='1';
const output=path.resolve(process.env.WEB_OCEAN_OUTPUT || `build/native/ocean-evidence${hardware?'-hardware':fallback?'-ui':''}`);
async function readFile(page,file) {
  return Buffer.from(await page.evaluate(file=>{
    const data=Module.FS.readFile(file),pieces=[];
    for(let i=0;i<data.length;i+=32768) pieces.push(String.fromCharCode(...data.subarray(i,i+32768)));
    return btoa(pieces.join(''));
  },file),'base64');
}
async function main() {
  fs.mkdirSync(output,{recursive:true});messages.length=0;
  fs.writeFileSync(path.join(output,'browser.log'),'');
  const browser=await chromium.launch({headless:true,args:hardware?[
    '--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan',
    '--use-vulkan=native','--disable-vulkan-surface',
  ]:[
    '--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader',
    '--enable-features=Vulkan','--use-vulkan=swiftshader',
  ]});
  const context=await browser.newContext({viewport:{width:1280,height:800},
    recordVideo:{dir:output,size:{width:1280,height:800}}});
  if(fallback) await context.addInitScript(()=>{delete WebAssembly.promising;delete WebAssembly.Suspending;});
  const page=await context.newPage(),errors=[],chapters=[],created=Date.now();
  const chapter=title=>chapters.push({seconds:(Date.now()-created)/1000,title});
  page.on('console',message=>{
    const text=message.text();messages.push(text);fs.appendFileSync(path.join(output,'browser.log'),text+'\n');
    if(/Aborted\(|Blender startup failed|Blender WebGPU error|Blender WebGPU device lost|Blender GPU job failed|WGPU_SH .*FAIL|WGPU_SHADER translate FAILED/.test(text)) errors.push(text);
  });
  page.on('pageerror',error=>errors.push(error.stack));
  try {
    chapter('Blender starts with FFTW ocean simulation and Alembic enabled');
    await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');
    await ready(page,new Promise(()=>{}));
    assert.equal(await page.evaluate(()=>document.documentElement.dataset.blenderVariant),fallback?'ui':'render');
    const manifest=await page.evaluate(async()=>(await(await fetch('manifest.json')).json()));
    for(const flag of ['WITH_FFTW3','WITH_MOD_OCEANSIM','WITH_ALEMBIC']) assert.equal(manifest.buildOptions[flag],true,flag);
    const fixture=path.join(__dirname,'fixtures/ocean-alembic');
    for(const name of ['reference.json','desktop-ocean.abc','desktop-hierarchy.abc']) {
      await page.evaluate(({name,bytes})=>{
        Module.FS.mkdirTree('/tmp/ocean-alembic');
        Module.FS.writeFile('/tmp/ocean-alembic/'+name,Uint8Array.from(bytes));
      },{name,bytes:[...fs.readFileSync(path.join(fixture,name))]});
    }
    await page.evaluate(source=>Module.FS.writeFile('/tmp/ocean-alembic.py',source),fs.readFileSync(path.join(__dirname,'ocean_alembic.py'),'utf8'));
    chapter('Check ocean waves and exchange animated Alembic files with desktop Blender');
    await page.mouse.move(300,250);
    await consoleCommand(page,"exec(compile(open('/tmp/ocean-alembic.py').read(),'/tmp/ocean-alembic.py','exec'))",'OCEAN_DONE');
    assert(!messages.some(text=>text.includes('OCEAN_FAILED:')),messages.find(text=>text.includes('OCEAN_FAILED:')));
    const completed=messages.findLast(text=>text.startsWith('OCEAN_COMPLETE:'));
    assert(completed,'Ocean and Alembic operations must complete');
    const report=JSON.parse(completed.slice('OCEAN_COMPLETE:'.length));
    // Keep full native samples as independent desktop verification input.
    fs.writeFileSync(path.join(output,'samples.json'),await readFile(page,report.samplesPath));
    delete report.samplesPath;
    const cache=await readFile(page,report.alembic.path);
    assert.equal(cache.length,report.alembic.bytes);
    assert.equal(cache.subarray(0,5).toString(),'Ogawa');
    fs.writeFileSync(path.join(output,'browser-ocean.abc'),cache);
    report.alembic.sha256=crypto.createHash('sha256').update(cache).digest('hex');
    chapter('Alembic ocean cache in the original Blender viewport');
    const view="import bpy, os; from mathutils import Quaternion; bpy.context.scene.frame_end=12; bpy.data.objects['NativeOcean'].color=(.035,.25,.6,1); [(setattr(s.region_3d,'view_distance',40),setattr(s.region_3d,'view_location',(0,0,0)),setattr(s.region_3d,'view_rotation',Quaternion((0.880476,0.279849,0.115917,0.364705))),setattr(s.shading,'color_type','OBJECT'),setattr(s.overlay,'show_floor',False),setattr(s.overlay,'show_axis_x',False),setattr(s.overlay,'show_axis_y',False)) for a in bpy.context.screen.areas for s in a.spaces if s.type=='VIEW_3D']; os.write(1,b'OCEAN_VIEW\\n')";
    await consoleCommand(page,view,'OCEAN_VIEW');
    await page.mouse.move(450,762);await page.keyboard.press('Home');
    await page.mouse.move(300,250);
    await page.waitForTimeout(3000);
    await page.screenshot({path:path.join(output,'ocean-cache.png')});
    chapter('Scrub the cached animation from frame 1 through frame 12');
    await consoleCommand(page,"import bpy, os; bpy.context.scene.frame_set(1); os.write(1,b'OCEAN_FRAME_1\\n')",'OCEAN_FRAME_1');
    await page.waitForTimeout(1500);
    // Arrow keys use Blender's original timeline shortcuts and dependency graph.
    await page.mouse.move(400,350);
    for(let frame=2;frame<=12;frame++) {await page.keyboard.press('ArrowRight');await page.waitForTimeout(180);}
    await page.screenshot({path:path.join(output,'ocean-frame-12.png')});
    await consoleCommand(page,"import bpy, os; assert bpy.context.scene.frame_current==12; os.write(1,b'OCEAN_SCRUBBED\\n')",'OCEAN_SCRUBBED');
    chapter('Reload offline and reopen the Alembic cache from persistent browser storage');
    await context.setOffline(true);await page.reload();await ready(page,new Promise(()=>{}));
    await page.mouse.move(300,250);
    await consoleCommand(page,"import bpy, os, json; bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete(use_global=False); assert bpy.ops.wm.alembic_import(filepath='/user/blender-web/home/browser-ocean.abc',as_background_job=False)=={'FINISHED'}; bpy.context.scene.frame_set(12); ob=bpy.data.objects['NativeOcean']; ev=ob.evaluated_get(bpy.context.evaluated_depsgraph_get()); ref=json.load(open('/user/blender-web/home/browser-ocean.reference.json'))['12']; assert len(ev.data.vertices)==676; assert max(abs(a-b) for v,r in zip(ev.data.vertices,ref['vertices']) for a,b in zip(v.co,r))<2e-4; os.write(1,b'OCEAN_OFFLINE_REOPENED\\n')",'OCEAN_OFFLINE_REOPENED');
    await consoleCommand(page,view,'OCEAN_VIEW');
    await page.mouse.move(450,762);await page.keyboard.press('Home');
    await page.mouse.move(300,250);
    await page.waitForTimeout(2000);
    await page.screenshot({path:path.join(output,'offline-ocean.png')});
    assert.deepEqual(errors,[]);
    const adapter=await page.evaluate(async()=>{const a=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});return {vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description};});
    if(hardware) assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
    Object.assign(report,{bundleVersion:manifest.version,variant:fallback?'ui':'render',hardware,
      browserVersion:browser.version(),adapter,offlineReopen:true,errors,chapters});
    const video=page.video();await context.close();report.videoPath=await video.path();
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report));
  } catch(error) {
    await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});throw error;
  } finally {await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
