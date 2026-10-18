/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {chromium}=require('playwright');
const {PNG}=require('pngjs');
const {ready,consoleCommand,messages}=require('./native.cjs');
const output=path.resolve(process.env.WEB_CYCLES_OUTPUT || path.join(__dirname,'../../build/native/cycles-evidence'));
async function main() {
  fs.mkdirSync(output,{recursive:true});
  fs.writeFileSync(path.join(output,'browser.log'),'');
  messages.length=0;
  const software=process.env.WEB_RENDER_SOFTWARE==='1';
  const browser=await chromium.launch({headless:true,args:software?[
    '--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader','--enable-features=Vulkan','--use-vulkan=swiftshader',
  ]:[
    '--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--use-vulkan=native','--disable-vulkan-surface',
  ]});
  const context=await browser.newContext({viewport:{width:1280,height:800},recordVideo:{dir:output,size:{width:1280,height:800}}});
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
    const setup="import bpy; s=bpy.context.scene; s.render.engine='CYCLES'; s.cycles.device='CPU'; s.cycles.samples=16; s.cycles.use_denoising=False; s.cycles.use_adaptive_sampling=False; s.cycles.seed=0; s.render.resolution_x=128; s.render.resolution_y=128; s.render.resolution_percentage=100; s.render.image_settings.file_format='PNG'; s.render.filepath='/tmp/web-cycles.png'; __import__('os').write(1,b'CYCLES_CONFIGURED\\n')";
    await consoleCommand(page,setup,'CYCLES_CONFIGURED');
    const clipStart=(Date.now()-created)/1000;
    await page.mouse.click(118,13);
    await page.waitForTimeout(700);
    const previous=messages.length,started=Date.now();
    await page.mouse.click(160,41);
    const deadline=Date.now()+300000;
    while (!messages.slice(previous).some(text=>text.includes('WEB_GPU_JOB complete'))) {
      if (errors.length) throw Error(errors.join('\n'));
      if (Date.now()>deadline) throw Error('CPU render did not finish');
      await page.waitForTimeout(100);
    }
    const renderSeconds=(Date.now()-started)/1000;
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
      clipStart,clipEnd,renderSeconds,meanAbsoluteError,outlierFraction,errors};
    report.adapter=await page.evaluate(async()=>{
      const adapter=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});
      return {vendor:adapter.info.vendor,architecture:adapter.info.architecture,
        device:adapter.info.device,description:adapter.info.description};
    });
    if (!software) assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(report.adapter)));
    await context.close();report.videoPath=await video.path();
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report));
  } finally {await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
