/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Render the shared Cycles OSL scene in the browser and compare it with native Blender. */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {chromium}=require('playwright');
const {PNG}=require('pngjs');
const {ready,consoleCommand,messages}=require('./native.cjs');
const output=path.resolve(process.env.WEB_CYCLES_OUTPUT || path.join(__dirname,'../../build/native/cycles-osl-evidence'));
const reference=path.resolve(process.env.WEB_CYCLES_OSL_REFERENCE || path.join(__dirname,'../docs/cycles-osl-reference.png'));
function compare(image,expected) {
  assert.deepEqual([image.width,image.height],[expected.width,expected.height]);
  let absoluteError=0,outliers=0,count=0;
  for(let i=0;i<image.data.length;i++) {
    if(i%4===3)continue;
    const error=Math.abs(image.data[i]-expected.data[i]);absoluteError+=error;count++;if(error>20)outliers++;
  }
  return {meanAbsoluteError:absoluteError/count,outlierFraction:outliers/count};
}
async function render(page,errors) {
  await page.mouse.move(600,400);
  await page.locator('canvas').focus();
  const previous=messages.length,started=Date.now();
  await page.keyboard.press('F12');
  const startDeadline=Date.now()+30000,deadline=Date.now()+600000;
  while(!messages.slice(previous).some(text=>text.includes('WEB_GPU_JOB complete'))) {
    if(errors.length)throw Error(errors.join('\n'));
    if(Date.now()>startDeadline&&!messages.slice(previous).some(text=>text.includes('WEB_GPU_JOB begin')))throw Error('Render shortcut did not start a job');
    if(Date.now()>deadline)throw Error('OSL render did not finish');
    await page.waitForTimeout(100);
  }
  return (Date.now()-started)/1000;
}
async function savedRender(page,marker) {
  await consoleCommand(page,"import bpy, os, base64; bpy.data.images['Render Result'].save_render('/tmp/web-osl.png',scene=bpy.context.scene); os.write(1,('"+marker+":'+base64.b64encode(open('/tmp/web-osl.png','rb').read()).decode()+'\\n').encode())",marker+':');
  return Buffer.from(messages.findLast(text=>text.startsWith(marker+':')).slice(marker.length+1),'base64');
}
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
  const context=await browser.newContext({viewport:{width:1280,height:800}});
  const page=await context.newPage();
  const errors=[];
  page.on('console',message=>{
    const text=message.text();messages.push(text);fs.appendFileSync(path.join(output,'browser.log'),text+'\n');
    if(/Aborted\(|Blender startup failed|Blender WebGPU error|Blender GPU job failed|OSL_FAILED:|Owned OSL|Missing OSL function|Unresolved owned-program symbol/.test(text))errors.push(text);
  });
  page.on('pageerror',error=>errors.push(error.stack));
  try {
    await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');
    await ready(page,new Promise(()=>{}));
    await page.mouse.move(300,250);
    await page.evaluate(source=>Module.FS.writeFile('/tmp/cycles_osl.py',source),fs.readFileSync(path.join(__dirname,'cycles_osl.py'),'utf8'));
    const setup=["import sys, os, time, json, traceback","try:",
      " sys.path.insert(0,'/tmp'); import cycles_osl; t=time.perf_counter(); s=cycles_osl.configure_cycles_osl(); s.render.threads_mode='AUTO'",
      ...(process.env.WEB_CYCLES_OSL_EXTRA?[' exec(open(\'/tmp/cycles_osl_extra.py\').read())']:[]),
      " os.write(1,('OSL_CONFIGURED:'+json.dumps({'shadingSystem':s.cycles.shading_system,'threads':s.render.threads,'configureSeconds':time.perf_counter()-t})+'\\n').encode())",
      "except BaseException:"," os.write(1,('OSL_FAILED:'+traceback.format_exc().replace(chr(10),' | ')+'\\n').encode())"].join('\n');
    await page.evaluate(source=>Module.FS.writeFile('/tmp/cycles_osl_setup.py',source),setup);
    if(process.env.WEB_CYCLES_OSL_EXTRA)await page.evaluate(source=>Module.FS.writeFile('/tmp/cycles_osl_extra.py',source),fs.readFileSync(process.env.WEB_CYCLES_OSL_EXTRA,'utf8'));
    await consoleCommand(page,"exec(open('/tmp/cycles_osl_setup.py').read())",'OSL_',undefined,300000);
    if(errors.length)throw Error(errors.join('\n'));
    const configured=JSON.parse(messages.findLast(text=>text.startsWith('OSL_CONFIGURED:')).slice('OSL_CONFIGURED:'.length));
    assert.equal(configured.shadingSystem,true);
    const firstSeconds=await render(page,errors);
    const bytes=await savedRender(page,'OSL_PNG');
    fs.writeFileSync(path.join(output,'browser.png'),bytes);
    const image=PNG.sync.read(bytes);
    const native=compare(image,PNG.sync.read(fs.readFileSync(reference)));
    // A second render reuses the compiled shader groups.
    const secondSeconds=await render(page,errors);
    await page.screenshot({path:path.join(output,'cycles-osl-browser.png')});
    if(!process.env.WEB_CYCLES_OSL_EXTRA) {
      assert(native.meanAbsoluteError<3,JSON.stringify(native));
      assert(native.outlierFraction<.01,JSON.stringify(native));
    }
    assert.deepEqual(errors,[]);
    const manifest=await page.evaluate(async()=>(await(await fetch('manifest.json')).json()));
    const report={bundleVersion:manifest.version,browserVersion:browser.version(),engine:'CYCLES',device:'CPU',
      shadingSystem:'OSL',samples:16,size:[128,128],seed:0,softwareDisplay:software,configured,
      firstRenderSeconds:firstSeconds,secondRenderSeconds:secondSeconds,native,errors};
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report));
  } catch(error) {
    await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});throw error;
  } finally {await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
