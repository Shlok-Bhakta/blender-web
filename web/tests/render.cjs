/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require('playwright');
const {PNG} = require('pngjs');
const {ready, consoleCommand, messages} = require('./native.cjs');
const output = path.resolve(process.env.WEB_RENDER_OUTPUT || path.join(__dirname, '../../build/native/render-evidence'));
const snapshot = `dict(engine=s.render.engine,width=s.render.resolution_x,height=s.render.resolution_y,percent=s.render.resolution_percentage,samples=s.eevee.taa_render_samples,frame=s.frame_current,camera=s.camera.name,display=bpy.context.preferences.view.render_display_type,view_transform=s.view_settings.view_transform,look=s.view_settings.look,exposure=s.view_settings.exposure,gamma=s.view_settings.gamma,objects=[dict(name=o.name,type=o.type,location=list(o.location),rotation=list(o.rotation_euler),scale=list(o.scale)) for o in sorted(s.objects,key=lambda o:o.name)],light_energy=bpy.data.objects['Light'].data.energy,camera_lens=s.camera.data.lens,eevee={p.identifier:getattr(s.eevee,p.identifier) for p in s.eevee.bl_rna.properties if p.type in ('BOOLEAN','INT','FLOAT','STRING','ENUM') and not p.is_readonly and not getattr(p,'is_array',False)})`;
const flags = process.env.WEB_RENDER_SOFTWARE === '1' ?
 ['--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader','--enable-features=Vulkan','--use-vulkan=swiftshader'] :
 ['--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--use-vulkan=native','--disable-vulkan-surface'];
async function main() {
  fs.mkdirSync(output,{recursive:true});
  fs.writeFileSync(path.join(output,'browser.log'),'');
  const browser = await chromium.launch({headless:true,args:flags});
  const deviceScaleFactor=Number(process.env.WEB_RENDER_DEVICE_SCALE || 1);
  assert(Number.isFinite(deviceScaleFactor) && deviceScaleFactor>0 && deviceScaleFactor<=4);
  const benchmark=process.env.WEB_RENDER_BENCHMARK==='1';
  const context = await browser.newContext({viewport:{width:1280,height:800},deviceScaleFactor,
    ...(benchmark?{}:{recordVideo:{dir:output,size:{width:1280,height:800}}})});
  const created = Date.now();
  const page = await context.newPage();
  const profile = process.env.WEB_RENDER_PROFILE === '1' ? await require('./render-profile.cjs').profileRender(browser, page, output) : null;
  const fatal = [];
  page.on('console', message => {
    const text=message.text(); messages.push(text);
    fs.appendFileSync(path.join(output,'browser.log'),text+'\n');
    if (/Aborted\(|Blender WebGPU error|Blender WebGPU device lost|Blender GPU job failed|WGPU_SH .*FAIL|WGPU_SHADER translate FAILED/.test(text)) fatal.push(text);
  });
  page.on('pageerror', error => {fatal.push(error.stack);fs.appendFileSync(path.join(output,'browser.log'),'PAGE_ERROR '+error.stack+'\n');});
  try {
    await page.goto(process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/');
    await ready(page,new Promise(()=>{}));
    assert.equal(await page.evaluate(()=>typeof WebAssembly.promising==='function'),true,'Rendering requires JSPI');
    const adapter=await page.evaluate(async()=>{const a=await navigator.gpu.requestAdapter();return {vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description,features:[...a.features],maxComputeInvocationsPerWorkgroup:a.limits.maxComputeInvocationsPerWorkgroup,maxStorageTexturesPerShaderStage:a.limits.maxStorageTexturesPerShaderStage,maxStorageBufferBindingSize:a.limits.maxStorageBufferBindingSize};});
    await page.mouse.move(300,250);
    await consoleCommand(page,`import bpy, os, json; s=bpy.context.scene; os.write(1, ('RENDER_DEFAULTS:'+json.dumps(${snapshot},default=sorted)+'\\n').encode())`,'RENDER_DEFAULTS:');
    const before=JSON.parse(messages.find(x=>x.startsWith('RENDER_DEFAULTS:')).slice('RENDER_DEFAULTS:'.length));
    assert.equal(before.engine,'BLENDER_EEVEE');
    assert.deepEqual([before.width,before.height,before.percent,before.samples],[1920,1080,100,64]);
    assert.deepEqual(before.objects.map(x=>x.name),['Camera','Cube','Light']);
    if(process.env.WEB_RENDER_TIMING==='1') {
      await page.evaluate(source=>Module.FS.writeFile('/tmp/eevee-timing.py',source),fs.readFileSync(path.join(__dirname,'eevee_timing.py'),'utf8'));
      await consoleCommand(page,"exec(compile(open('/tmp/eevee-timing.py').read(),'/tmp/eevee-timing.py','exec')); configure_eevee_timing(); __import__('os').write(1,b'EEVEE_TIMING_READY\\n')",'EEVEE_TIMING_READY');
    }
    await page.screenshot({path:path.join(output,'before-render.png')});
    const clipStart=(Date.now()-created)/1000;
    const renderRuns=[];
    const resizeDuringRender=process.env.WEB_RENDER_RESIZE_DURING_RENDER === '1';
    const renderResizeChecks=[];
    const repeats=Number(process.env.WEB_RENDER_REPEATS || 1);
    assert(Number.isInteger(repeats) && repeats >= 1 && repeats <= 10);
    for (let run=0;run<repeats;run++) {
      await page.waitForTimeout(2000);
      // Use Blender's native Render menu so the recording shows the action.
      await page.mouse.click(118,13);
      await page.waitForTimeout(700);
      await page.screenshot({path:path.join(output,`render-menu-${run}.png`)});
      if (profile) await profile.start();
      const previous=messages.length;
      const started=Date.now();
      await page.mouse.click(160,41);
      const deadline=Date.now()+600000;
      let resized=false;
      while(!messages.slice(previous).some(x=>x.includes('WEB_GPU_JOB complete'))) {
        if(fatal.length) throw new Error(fatal.join('\n'));
        if(Date.now()>deadline) throw new Error('Default Eevee render did not complete');
        if (resizeDuringRender && !resized && messages.slice(previous).some(x=>x.includes('WEB_GPU_JOB begin'))) {
          resized=true;
          for (const size of [[1040,720],[1440,900],[1280,800]]) {
            const jobActive=!messages.slice(previous).some(x=>x.includes('WEB_GPU_JOB complete'));
            assert(jobActive,'Resize check must run during the native GPU job');
            await page.setViewportSize({width:size[0],height:size[1]});
            renderResizeChecks.push({run,css:size,jobActiveWhenRequested:jobActive});
          }
        }
        await page.waitForTimeout(100);
      }
      if (resizeDuringRender) assert(resized,'Native render must exercise resizing');
      renderRuns.push((Date.now()-started)/1000);
      assert(messages.slice(previous).some(x=>x.includes('WEB_READBACK 1920x1080 channels=4')&&x.includes('success=1')),'Each render must receive real 1920x1080 pixels');
      if (profile) await profile.stop(run);
    }
    const renderSeconds=renderRuns[0];
    const handlerSeconds=messages.filter(text=>text.startsWith('EEVEE_TIMING:')).map(text=>JSON.parse(text.slice('EEVEE_TIMING:'.length)).seconds);
    if(process.env.WEB_RENDER_TIMING==='1') assert.equal(handlerSeconds.length,repeats);
    await page.waitForTimeout(2000);
    await page.mouse.move(600,400);
    await page.keyboard.press('Home');
    await page.waitForTimeout(2000);
    await page.screenshot({path:path.join(output,'render-result.png')});
    await page.waitForTimeout(4000);
    const clipEnd=(Date.now()-created)/1000;
    await consoleCommand(page,`import bpy, os, json, base64; s=bpy.context.scene; bpy.data.images['Render Result'].save_render('/user/blender-web/home/eevee-default.png',scene=s); os.write(1, ('RENDER_PNG:'+base64.b64encode(open('/user/blender-web/home/eevee-default.png','rb').read()).decode()+'\\n').encode()); os.write(1, ('RENDER_AFTER:'+json.dumps(${snapshot},default=sorted)+'\\n').encode())`,'RENDER_AFTER:');
    const after=JSON.parse(messages.find(x=>x.startsWith('RENDER_AFTER:')).slice('RENDER_AFTER:'.length));
    assert.deepEqual(after,before,'Rendering must not alter the factory scene or render settings');
    const encoded=messages.find(x=>x.startsWith('RENDER_PNG:'));
    assert(encoded,'Blender must save the rendered image through its normal PNG writer');
    const bytes=Buffer.from(encoded.slice('RENDER_PNG:'.length),'base64');
    fs.writeFileSync(path.join(output,'eevee-default.png'),bytes);
    const png=PNG.sync.read(bytes);
    assert.deepEqual([png.width,png.height],[1920,1080]);
    const background=[...png.data.subarray(0,3)];
    let opaque=0,foreground=0;
    for(let i=0;i<png.data.length;i+=4) {
      if(png.data[i+3]===255) opaque++;
      if(Math.abs(png.data[i]-background[0])+Math.abs(png.data[i+1]-background[1])+Math.abs(png.data[i+2]-background[2])>30) foreground++;
    }
    assert(opaque>png.width*png.height*.99,'Factory world must remain opaque');
    assert(foreground>30000&&foreground<png.width*png.height*.6,'Expected a visible cube against the factory world');
    let referencePixelsEqual;
    if (process.env.WEB_RENDER_REFERENCE) {
      const reference=PNG.sync.read(fs.readFileSync(process.env.WEB_RENDER_REFERENCE));
      assert.deepEqual([png.width,png.height],[reference.width,reference.height]);
      let changed=0, maxDelta=0;
      for (let i=0;i<png.data.length;i++) {
        const delta=Math.abs(png.data[i]-reference.data[i]);
        if (delta) changed++;
        maxDelta=Math.max(maxDelta,delta);
      }
      assert.equal(changed,0,`Rendered channels differ from reference: ${changed}, max delta ${maxDelta}`);
      referencePixelsEqual=true;
    }
    assert.deepEqual(fatal,[]);
    const bundleVersion=await page.evaluate(async()=>(await(await fetch('manifest.json')).json()).version);
    const report={browserVersion:browser.version(),bundleVersion,deviceScaleFactor,renderResizeChecks,engine:before.engine,settingsUnchanged:true,referencePixelsEqual,width:png.width,height:png.height,samples:before.samples,renderSeconds,renderRuns,handlerSeconds,videoCaptured:!benchmark,adapter,foregroundPixels:foreground,opaquePixels:opaque,browserErrors:0,clipStart,clipEnd,defaults:before};
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report));
  } catch(error) {
    await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});
    throw error;
  } finally {
    if (profile) await profile.dispose();
    const video=page.video();
    await context.close();
    if(video) fs.writeFileSync(path.join(output,'video-path.txt'),await video.path());
    await browser.close();
  }
}
main().catch(error=>{console.error(error);process.exitCode=1;});
