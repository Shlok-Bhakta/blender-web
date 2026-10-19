/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');
const {chromium}=require('playwright');
const {ready,consoleCommand,messages}=require('./native.cjs');
const fallback=process.env.WEB_TEST_UI_FALLBACK==='1';
const hardware=process.env.WEB_TEST_HARDWARE==='1';
const output=path.resolve(process.env.WEB_VOLUME_OUTPUT || `build/native/volume-evidence${hardware?'-hardware':fallback?'-ui':''}`);

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
    chapter('Start Blender with OpenVDB and NanoVDB');
    await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');
    await ready(page,new Promise(()=>{}));
    const manifest=await page.evaluate(async()=>(await(await fetch('manifest.json')).json()));
    assert.equal(manifest.buildOptions.WITH_OPENVDB,true);
    assert.equal(manifest.buildOptions.WITH_NANOVDB,true);
    assert.equal(manifest.buildOptions.WITH_MOD_FLUID,true);
    const fixture=path.join(__dirname,'fixtures/volume');
    assert.equal(JSON.parse(fs.readFileSync(path.join(fixture,'reference.json'))).workloadSha256,
      crypto.createHash('sha256').update(fs.readFileSync(path.join(__dirname,'volume.py'))).digest('hex'),
      'Regenerate the paired desktop volume reference for this workload');
    const adapter=await page.evaluate(async()=>{
      const info=(await navigator.gpu.requestAdapter({powerPreference:'high-performance'})).info;
      return {vendor:info.vendor,architecture:info.architecture,device:info.device,description:info.description};
    });
    if(hardware) assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)),'Hardware volume recording requires a physical adapter');
    const hashes={};
    for(const name of ['reference.json','sphere-zip.vdb','sphere-blosc.vdb']) {
      const bytes=fs.readFileSync(path.join(fixture,name));
      hashes[name]=crypto.createHash('sha256').update(bytes).digest('hex');
      await page.evaluate(({name,bytes})=>{
        Module.FS.mkdirTree('/tmp/volume');
        Module.FS.writeFile('/tmp/volume/'+name,Uint8Array.from(bytes));
        Module.FS.mkdirTree('/user/blender-web/home/volume');
        Module.FS.writeFile('/user/blender-web/home/volume/'+name,Uint8Array.from(bytes));
      },{name,bytes:[...bytes]});
    }
    await page.evaluate(source=>{
      Module.FS.writeFile('/tmp/volume.py',source);
      Module.FS.writeFile('/user/blender-web/home/volume/check.py',source);
    },fs.readFileSync(path.join(__dirname,'volume.py'),'utf8'));
    chapter('Compare VDB import and volume modifiers with desktop Blender');
    await page.mouse.move(300,250);
    await consoleCommand(page,"exec(compile(open('/tmp/volume.py').read(),'/tmp/volume.py','exec')); browser_check('/tmp/volume')",'VOLUME_FINISHED:',undefined,180000);
    assert(!messages.some(line=>line==='VOLUME_FAILED'),'Native volume check raised a Python exception; see browser.log');
    const report=JSON.parse(messages.find(line=>line.startsWith('VOLUME_DONE:')).slice('VOLUME_DONE:'.length));
    chapter('Mesh to Volume and Volume to Mesh in the original viewport');
    await consoleCommand(page,"import bpy,os; [(setattr(s.region_3d,'view_distance',4),setattr(s.shading,'color_type','OBJECT')) for a in bpy.context.screen.areas for s in a.spaces if s.type=='VIEW_3D']; os.write(1,b'VOLUME_VIEW\\n')",'VOLUME_VIEW');
    await page.waitForTimeout(2500);
    await page.screenshot({path:path.join(output,'volume.png')});
    chapter('Replay the four-frame Mantaflow gas simulation in Blender');
    await consoleCommand(page,"_gas=fluid('/tmp/volume/visual-cache'); bpy.data.objects['Native gas domain'].display_type='WIRE'; bpy.data.objects['Sphere'].hide_set(True); bpy.context.scene.frame_end=4; bpy.context.scene.frame_set(1); __import__('os').write(1,b'GAS_VIEW_READY\\n')",'GAS_VIEW_READY',undefined,180000);
    await page.mouse.move(400,350);
    for(let frame=2;frame<=4;frame++) {await page.keyboard.press('ArrowRight');await page.waitForTimeout(600);}
    await page.screenshot({path:path.join(output,'gas.png')});
    await consoleCommand(page,"assert bpy.context.scene.frame_current==4; d=bpy.data.objects['Native gas domain'].evaluated_get(bpy.context.evaluated_depsgraph_get()); assert max(d.modifiers['Mantaflow domain'].domain_settings.density_grid)>0.1; __import__('os').write(1,b'GAS_FRAME_4_OK\\n')",'GAS_FRAME_4_OK');
    report.gasReplayFrame=4;
    chapter('Reload offline and reopen the VDB files from browser storage');
    await context.setOffline(true);await page.reload();await ready(page,new Promise(()=>{}));
    await page.mouse.move(300,250);
    await consoleCommand(page,"exec(compile(open('/user/blender-web/home/volume/check.py').read(),'volume-check.py','exec')); browser_check('/user/blender-web/home/volume','VOLUME_OFFLINE')",'VOLUME_OFFLINE_FINISHED:',undefined,180000);
    assert(!messages.some(line=>line==='VOLUME_OFFLINE_FAILED'),'Offline native volume check raised a Python exception');
    const offline=JSON.parse(messages.find(line=>line.startsWith('VOLUME_OFFLINE_DONE:')).slice('VOLUME_OFFLINE_DONE:'.length));
    report.offlineMaximumCoordinateError=offline.maximumCoordinateError;
    report.offlineMaximumDensityError=offline.fluid.maximumDesktopDensityError;
    assert(report.offlineMaximumCoordinateError<2e-5);
    assert(report.offlineMaximumDensityError<1e-3);
    report.offlineReopen=true;
    assert.deepEqual(errors,[]);
    Object.assign(report,{bundleVersion:manifest.version,browserVersion:browser.version(),fallback,hardware,adapter,
      fixtureHashes:hashes,browserErrors:errors,chapters,videoPath:path.join(output,'volume.webm'),sourceSha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(__dirname,'volume.py'))).digest('hex')});
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report));
  } catch(error) {
    await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});
    throw error;
  } finally {
    const video=page.video();await context.close();
    if(video) await video.saveAs(path.join(output,'volume.webm'));
    await browser.close();
  }
}
main().catch(error=>{console.error(error);process.exitCode=1;});
