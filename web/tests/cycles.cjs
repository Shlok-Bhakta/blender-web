/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {chromium}=require('playwright');
const {PNG}=require('pngjs');
const {ready,consoleCommand,messages}=require('./native.cjs');
const output=path.resolve(process.env.WEB_CYCLES_OUTPUT || path.join(__dirname,'../../build/native/cycles-evidence'));
const backend=process.env.WEB_CYCLES_BVH || null;
async function main() {
  if(backend)assert(['BVH2','EMBREE'].includes(backend));
  fs.mkdirSync(output,{recursive:true});
  fs.writeFileSync(path.join(output,'browser.log'),'');
  messages.length=0;
  const software=process.env.WEB_RENDER_SOFTWARE==='1';
  const browser=await chromium.launch({headless:true,args:software?[
    '--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader','--enable-features=Vulkan','--use-vulkan=swiftshader',
  ]:[
    '--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--use-vulkan=native','--disable-vulkan-surface',
  ]});
  const benchmark=process.env.WEB_CYCLES_BENCHMARK==='1';
  const context=await browser.newContext({viewport:{width:1280,height:800},
    ...(benchmark?{}:{recordVideo:{dir:output,size:{width:1280,height:800}}})});
  if(backend) {
    await context.addInitScript(()=>{
      let module;
      Object.defineProperty(window,'Module',{configurable:true,get:()=>module,set:value=>{
        if(value?.arguments&&!value.arguments.includes('--log'))value.arguments.unshift('--log','cycles','--log-level','info');
        module=value;
      }});
    });
  }
  const created=Date.now();
  const page=await context.newPage();
  const errors=[];
  page.on('console',message=>{
    const text=message.text();messages.push(text);fs.appendFileSync(path.join(output,'browser.log'),text+'\n');
    if (/Aborted\(|Blender startup failed|Blender WebGPU error|Blender GPU job failed|CYCLES_FAILED:/.test(text)) errors.push(text);
  });
  page.on('pageerror',error=>errors.push(error.stack));
  try {
    await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');
    await ready(page,new Promise(()=>{}));
    await page.mouse.move(300,250);
    // The native job executor owns the render wait, as for Eevee. Schedule it
    // with Blender's operator after restoring the viewport from the console.
    await page.evaluate(source=>Module.FS.writeFile('/tmp/cycles-timing.py',source),fs.readFileSync(path.join(__dirname,'cycles_timing.py'),'utf8'));
    // Fixed eight-thread settings belong to the paired benchmark on its
    // eight-core host. Normal acceptance follows the browser's CPU count.
    const setup="exec(compile(open('/tmp/cycles-timing.py').read(),'/tmp/cycles-timing.py','exec')); configure_cycles_timing(); "+
      (benchmark?"":"bpy.context.scene.render.threads_mode='AUTO'; ")+
      (backend?"import _cycles; bpy.context.preferences.experimental.use_cycles_debug=True; bpy.context.preferences.view.show_developer_ui=True; bpy.context.scene.cycles.debug_bvh_layout='"+backend+"'; _cycles.debug_flags_update(bpy.context.scene.as_pointer()); ":"")+
      "__import__('os').write(1,('CYCLES_THREADS:'+str(bpy.context.scene.render.threads)+'\\nCYCLES_CONFIGURED\\n').encode())";
    await consoleCommand(page,setup,'CYCLES_CONFIGURED');
    const clipStart=(Date.now()-created)/1000;
    const repeats=Number(process.env.WEB_CYCLES_REPEATS||1),renderRuns=[];
    assert(Number.isInteger(repeats)&&repeats>=1&&repeats<=10);
    for(let run=0;run<repeats;run++) {
      // Use the original render shortcut. A fixed-delay popup click can miss
      // its menu while a slow software adapter finishes the viewport redraw.
      await page.mouse.move(600,400);
      await page.locator('canvas').focus();
      const previous=messages.length,started=Date.now();
      await page.keyboard.press('F12');
      const startDeadline=Date.now()+30000;
      const deadline=Date.now()+300000;
      while (!messages.slice(previous).some(text=>text.includes('WEB_GPU_JOB complete'))) {
        if (errors.length) throw Error(errors.join('\n'));
        if(Date.now()>startDeadline && !messages.slice(previous).some(text=>text.includes('WEB_GPU_JOB begin'))) throw Error('Native render shortcut did not start a job');
        if (Date.now()>deadline) throw Error('CPU render did not finish');
        await page.waitForTimeout(100);
      }
      renderRuns.push((Date.now()-started)/1000);
      await page.waitForTimeout(1000);
    }
    const renderSeconds=renderRuns[0];
    const handlerSeconds=messages.filter(text=>text.startsWith('CYCLES_TIMING:')).map(text=>JSON.parse(text.slice('CYCLES_TIMING:'.length)).seconds);
    assert.equal(handlerSeconds.length,repeats);
    await page.waitForTimeout(2000);
    await page.mouse.move(600,400);
    await page.keyboard.press('Home');
    await page.waitForTimeout(1500);
    await page.mouse.move(640,415);
    for (let step=0;step<8;step++) {
      await page.mouse.wheel(0,-100);
      await page.waitForTimeout(100);
    }
    await page.waitForTimeout(1500);
    await page.screenshot({path:path.join(output,'cycles-browser.png')});
    const clipEnd=(Date.now()-created)/1000;
    await consoleCommand(page,"import bpy, os, base64; bpy.data.images['Render Result'].save_render('/tmp/web-cycles.png',scene=bpy.context.scene); os.write(1,('CYCLES_PNG:'+base64.b64encode(open('/tmp/web-cycles.png','rb').read()).decode()+'\\n').encode())",'CYCLES_PNG:');
    const encoded=messages.findLast(text=>text.startsWith('CYCLES_PNG:')).slice('CYCLES_PNG:'.length);
    const bytes=Buffer.from(encoded,'base64');
    const image=PNG.sync.read(bytes);
    assert.deepEqual([image.width,image.height],[128,128]);
    const reference=PNG.sync.read(fs.readFileSync(path.join(__dirname,'../docs/cycles-cpu-reference.png')));
    let absoluteError=0,outliers=0,count=0;
    for (let i=0;i<image.data.length;i++) {
      if (i%4===3) continue;
      const error=Math.abs(image.data[i]-reference.data[i]);absoluteError+=error;count++;if(error>20) outliers++;
    }
    const meanAbsoluteError=absoluteError/count,outlierFraction=outliers/count;
    assert(meanAbsoluteError<3,{meanAbsoluteError,outlierFraction});
    assert(outlierFraction<.01,{meanAbsoluteError,outlierFraction});
    fs.writeFileSync(path.join(output,'cycles.png'),bytes);
    await page.waitForTimeout(2000);
    assert.deepEqual(errors,[]);
    const video=page.video();
    const manifest=await page.evaluate(async()=>(await(await fetch('manifest.json')).json()));
    const report={bundleVersion:manifest.version,browserVersion:browser.version(),engine:'CYCLES',device:'CPU',samples:16,size:[128,128],denoising:false,adaptiveSampling:false,seed:0,softwareDisplay:software,
      clipStart,clipEnd,renderSeconds,renderRuns,handlerSeconds,
      threads:Number(messages.findLast(text=>text.startsWith('CYCLES_THREADS:')).slice('CYCLES_THREADS:'.length)),
      videoCaptured:!benchmark,meanAbsoluteError,outlierFraction,errors};
    if(backend) {
      report.bvhBackend=backend;
      report.backendLogs=messages.filter(text=>/Using (BVH2|EMBREE) layout\./.test(text));
      assert.equal(report.backendLogs.length,repeats);
      assert(report.backendLogs.every(text=>text.includes('Using '+backend+' layout.')));
    }
    report.adapter=await page.evaluate(async()=>{
      const adapter=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});
      return {vendor:adapter.info.vendor,architecture:adapter.info.architecture,
        device:adapter.info.device,description:adapter.info.description};
    });
    if (!software) assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(report.adapter)));
    await context.close();if(video) report.videoPath=await video.path();
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report));
  } catch(error) {
    await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});throw error;
  } finally {await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
