/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');
const {PNG}=require('pngjs');
const {chromium}=require('playwright');
const {ready,consoleCommand,messages}=require('./native.cjs');
const hardware=process.env.WEB_TEST_HARDWARE==='1';
const fallback=process.env.WEB_TEST_UI_FALLBACK==='1';
const output=path.resolve(process.env.WEB_RENDER_LIBRARIES_OUTPUT||`build/native/render-libraries-evidence${hardware?'-hardware':''}${fallback?'-ui':''}`);
const fixture=path.join(__dirname,'fixtures/render-libraries');
const directory='/user/blender-web/home/render-libraries';
const hash=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
function compare(actual,expected,caseName){
  const a=PNG.sync.read(actual),b=PNG.sync.read(expected);
  assert.deepEqual([a.width,a.height],[192,144]);assert.deepEqual([a.width,a.height],[b.width,b.height]);
  let error=0,count=0,outliers=0,maximum=0;
  for(let i=0;i<a.data.length;i++)if(i%4!==3){const d=Math.abs(a.data[i]-b.data[i]);error+=d;count++;maximum=Math.max(maximum,d);if(d>25)outliers++;}
  const result={meanAbsoluteError:error/count,outlierFraction:outliers/count,maximumError:maximum};
  // Guiding trains a floating-point distribution. SIMD width and 32-bit
  // layouts can change subsequent sampled paths, so compare image statistics.
  const guided=caseName==='guided'||caseName==='volume-guided';
  assert(result.meanAbsoluteError<(guided?4:1.5)&&result.outlierFraction<(guided?.035:.005),JSON.stringify({caseName,...result}));
  return result;
}
async function main(){
  fs.mkdirSync(output,{recursive:true});messages.length=0;fs.writeFileSync(path.join(output,'browser.log'),'');
  const browser=await chromium.launch({headless:true,args:hardware?[
    '--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--use-vulkan=native','--disable-vulkan-surface',
  ]:[
    '--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader','--enable-features=Vulkan','--use-vulkan=swiftshader',
  ]});
  const context=await browser.newContext({viewport:{width:1280,height:800},recordVideo:{dir:output,size:{width:1280,height:800}}});
  if(fallback)await context.addInitScript(()=>{delete WebAssembly.promising;delete WebAssembly.Suspending;});
  await context.addInitScript(()=>{
    let module;Object.defineProperty(window,'Module',{configurable:true,get:()=>module,set:value=>{
      if(value?.arguments&&!value.arguments.includes('--log'))value.arguments.unshift('--log','cycles','--log-level','trace');module=value;
    }});
  });
  const page=await context.newPage(),errors=[],chapters=[],started=Date.now();
  let rejectFailure;const failure=new Promise((_,reject)=>rejectFailure=reject);failure.catch(()=>{});
  const fail=text=>{errors.push(text);rejectFailure(Error(text));};
  page.on('console',message=>{const text=message.text();messages.push(text);fs.appendFileSync(path.join(output,'browser.log'),text+'\n');
    if(/RENDER_LIBRARIES_FAILED:|Aborted\(|Blender startup failed|Blender UI frame failed|Blender GPU job failed|Blender WebGPU (error|device lost)/.test(text))fail(text);
  });
  page.on('pageerror',error=>fail(error.stack||error.message));
  const command=(code,marker)=>Promise.race([failure,consoleCommand(page,code,marker,()=>page.screenshot(),300000)]);
  const chapter=title=>chapters.push({seconds:(Date.now()-started)/1000,title});
  const result=name=>JSON.parse(messages.findLast(text=>text.startsWith(name+':')).slice(name.length+1));
  try{
    chapter('Start Blender with OpenPGL and Open Image Denoise');
    await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');await ready(page,failure);
    const manifest=await page.evaluate(()=>fetch('manifest.json').then(r=>r.json()));
    assert.equal(manifest.buildOptions.WITH_CYCLES_PATH_GUIDING,true);assert.equal(manifest.buildOptions.WITH_OPENIMAGEDENOISE,true);
    const adapter=await page.evaluate(async()=>{const a=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});return {vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description};});
    if(hardware)assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
    const source=fs.readFileSync(path.join(__dirname,'render-libraries.py'));
    const reference=JSON.parse(fs.readFileSync(path.join(fixture,'reference.json')));assert.equal(hash(source),reference.sourceSha256);
    const fixtureHashes={};
    for(const name of ['check.py','scene.blend','reference.json','gradient.exr','hevc-8.mkv','hevc-10.mkv','hevc-12.mkv',...Object.keys(reference.cases).map(name=>name+'.png')]){
      const bytes=name==='check.py'?source:fs.readFileSync(path.join(fixture,name));fixtureHashes[name]=hash(bytes);
      if(!name.endsWith('.png')&&!name.endsWith('.mkv'))await page.evaluate(({name,data,directory})=>{Module.FS.mkdirTree(directory);Module.FS.writeFile(directory+'/'+name,Uint8Array.from(atob(data),c=>c.charCodeAt(0)));},{name,data:bytes.toString('base64'),directory});
    }
    await page.mouse.move(300,250);chapter('Render Cycles denoising, surface guiding, volume guiding and compositor denoising');
    await command(`rl_ns={'__file__':'${directory}/check.py','__name__':'render_libraries'}; exec(compile(__import__('pathlib').Path('${directory}/check.py').read_text(),'render-libraries.py','exec'),rl_ns); rl_ns['browser_check']('${directory}',render=${fallback?'False':'True'})`,'RENDER_LIBRARIES_FINISHED');
    const report=result('RENDER_LIBRARIES_DONE');assert.equal(report.sourceSha256,hash(source));assert(report.pathGuiding&&report.embree);
    const images={};
    for(const name of await page.evaluate(directory=>Module.FS.readdir(directory).filter(n=>/\.png$|^hevc-\d+\.mkv$|^render-libraries\.blend$/.test(n)),directory)){
      const bytes=Buffer.from(await page.evaluate(name=>Array.from(Module.FS.readFile(name)),directory+'/'+name));fs.writeFileSync(path.join(output,name),bytes);
      if(name.endsWith('.mkv'))report.hevc[name.match(/^hevc-(\d+)\.mkv$/)[1]].sha256=hash(bytes);
      if(name.endsWith('.png'))images[name]={...compare(bytes,fs.readFileSync(path.join(fixture,name)),name.slice(0,-4)),bytes:bytes.length,sha256:hash(bytes)};
    }
    const log=messages.join('\n');
    const surfaceSamples=[...log.matchAll(/Number of surface samples: (\d+)/g)].map(m=>Number(m[1]));
    const volumeSamples=[...log.matchAll(/Number of volume samples: (\d+)/g)].map(m=>Number(m[1]));
    const fieldUpdates=[...log.matchAll(/Path guiding field valid: (\d+)/g)].map(m=>Number(m[1]));
    if(!fallback){
      assert.equal(Object.keys(images).length,5);assert.deepEqual(Object.keys(report.hevc),['8','10','12']);assert(Math.max(...surfaceSamples)>1024);assert(Math.max(...volumeSamples)>1024);
      assert(fieldUpdates.length>=8&&fieldUpdates.every(value=>value===1));
      assert.notEqual(images['noisy.png'].sha256,images['denoised.png'].sha256);
      assert.notEqual(images['noisy.png'].sha256,images['compositor.png'].sha256);
      for(const [caseName,title] of [['noisy','Inspect the noisy Cycles render'],['denoised','Inspect Cycles CPU denoising'],['volume-guided','Inspect volume guiding with denoising']]){
      chapter(title);
      const display=`def rl_display():\n    area=max(bpy.context.screen.areas,key=lambda a:a.width*a.height)\n    area.type='IMAGE_EDITOR'\n    area.spaces.active.image=bpy.data.images.load('${directory}/${caseName}.png',check_existing=False)\n    region=next(r for r in area.regions if r.type=='WINDOW')\n    with bpy.context.temp_override(area=area,region=region):\n        bpy.ops.image.view_all(fit_view=True)\n    area.tag_redraw()\nbpy.app.timers.register(rl_display,first_interval=1.5)`;
      await command(`import bpy; exec(${JSON.stringify(display)}); __import__('os').write(1,b'RENDER_LIBRARIES_DISPLAY\\n')`,'RENDER_LIBRARIES_DISPLAY');
      await page.waitForTimeout(3000);await page.screenshot({path:path.join(output,caseName==='volume-guided'?'render-libraries-browser.png':caseName+'-browser.png')});
      }
    }
    chapter('Reload offline and reopen the saved Blender scene');
    await context.setOffline(true);await page.reload();await ready(page,failure);await page.mouse.move(300,250);
    await command(`import bpy,json,os; bpy.ops.wm.open_mainfile(filepath='${directory}/render-libraries.blend'); os.write(1,('RENDER_LIBRARIES_REOPENED:'+json.dumps({'name':bpy.context.scene.name,'guiding':bpy.context.scene.cycles.use_guiding,'denoising':bpy.context.scene.cycles.use_denoising,'volume':not bpy.context.scene.objects['Guiding volume'].hide_render})+'\\n').encode())`,'RENDER_LIBRARIES_REOPENED:');
    const reopened=result('RENDER_LIBRARIES_REOPENED');assert.deepEqual(reopened,{name:'Path guiding and denoising',guiding:true,denoising:true,volume:true});
    const blend=fs.readFileSync(path.join(output,'render-libraries.blend'));
    Object.assign(report,{runnerSourceSha256:hash(fs.readFileSync(__filename)),bundleVersion:manifest.version,hardware,fallback,adapter,images,fixtureHashes,chapters,
      guiding:{surfaceSamples,volumeSamples,fieldUpdates},offlineReopen:true,reopened,browserErrors:errors,
      blend:{bytes:blend.length,sha256:hash(blend)},browserVersion:browser.version(),videoPath:path.join(output,'render-libraries.webm')});
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify({bundleVersion:manifest.version,images:Object.keys(images),fieldUpdates:fieldUpdates.length,offlineReopen:true}));
  }catch(error){await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});throw error;}
  finally{const video=page.video();await context.close();if(video)await video.saveAs(path.join(output,'render-libraries.webm'));await browser.close();}
}
if(require.main===module)main().catch(error=>{console.error(error);process.exitCode=1;});
module.exports={compare};
