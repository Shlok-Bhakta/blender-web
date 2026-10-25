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
const benchmark=process.env.WEB_EMBREE_BENCHMARK==='1';
const profiled=process.env.WEB_EMBREE_PROFILE==='1';
const recordVideo=!benchmark&&!profiled;
const output=path.resolve(process.env.WEB_EMBREE_OUTPUT||`build/native/embree-evidence${hardware?'-hardware':''}${fallback?'-ui':''}`);
const fixtures=path.join(__dirname,'fixtures/embree');
const directory='/user/blender-web/home/embree';
const hash=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
function compare(actual,expected) {
  const a=PNG.sync.read(actual),b=PNG.sync.read(expected);
  assert.deepEqual([a.width,a.height],[256,192]);assert.deepEqual([a.width,a.height],[b.width,b.height]);
  let sum=0,outliers=0,count=0,maximum=0;
  for(let i=0;i<a.data.length;i++) if(i%4!==3) {
    const error=Math.abs(a.data[i]-b.data[i]);sum+=error;count++;maximum=Math.max(maximum,error);if(error>25)outliers++;
  }
  const result={meanAbsoluteError:sum/count,outlierFraction:outliers/count,maximumError:maximum};
  assert(result.meanAbsoluteError<1.5 && result.outlierFraction<.005,JSON.stringify(result));
  result.regions={};
  for(const [name,[left,top,right,bottom]] of Object.entries({curves:[8,90,44,148],points:[50,125,135,165]})) {
    let error=0,large=0,samples=0;
    for(let y=top;y<bottom;y++)for(let x=left;x<right;x++)for(let c=0;c<3;c++){
      const i=(y*a.width+x)*4+c,d=Math.abs(a.data[i]-b.data[i]);error+=d;samples++;if(d>25)large++;
    }
    const region={meanAbsoluteError:error/samples,outlierFraction:large/samples};
    assert(region.meanAbsoluteError<2 && region.outlierFraction<.02,JSON.stringify({name,...region}));
    result.regions[name]=region;
  }
  return result;
}
async function main() {
  fs.mkdirSync(output,{recursive:true});messages.length=0;fs.writeFileSync(path.join(output,'browser.log'),'');
  const browser=await chromium.launch({headless:true,args:hardware?[
    '--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--use-vulkan=native','--disable-vulkan-surface',
  ]:[
    '--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader','--enable-features=Vulkan','--use-vulkan=swiftshader',
  ]});
  const context=await browser.newContext({viewport:{width:1280,height:800},...(!recordVideo?{}:{recordVideo:{dir:output,size:{width:1280,height:800}}})});
  if(fallback)await context.addInitScript(()=>{delete WebAssembly.promising;delete WebAssembly.Suspending;});
  // Enable the original Cycles log only inside this isolated test context.
  await context.addInitScript(()=>{
    let module;
    Object.defineProperty(window,'Module',{configurable:true,get:()=>module,set:value=>{
      if(value?.arguments && !value.arguments.includes('--log'))value.arguments.unshift('--log','cycles','--log-level','info');
      module=value;
    }});
  });
  const page=await context.newPage(),errors=[],chapters=[],started=Date.now();
  const profile=profiled?await require('./render-profile.cjs').profileRender(browser,page,output):null;
  let rejectFailure;const failure=new Promise((_,reject)=>rejectFailure=reject);failure.catch(()=>{});
  const fail=text=>{errors.push(text);rejectFailure(Error(text));};
  page.on('console',message=>{
    const text=message.text();messages.push(text);fs.appendFileSync(path.join(output,'browser.log'),text+'\n');
    if(/EMBREE_FAILED:|Aborted\(|Blender startup failed|Blender UI frame failed|Blender GPU job failed|Blender WebGPU (error|device lost)/.test(text))fail(text);
  });
  page.on('pageerror',error=>fail(error.stack||error.message));
  const command=(code,marker)=>Promise.race([failure,consoleCommand(page,code,marker,()=>page.screenshot(),300000)]);
  const chapter=title=>chapters.push({seconds:(Date.now()-started)/1000,title});
  const emit=(name,expression)=>`__import__('os').write(1,('${name}:'+__import__('json').dumps(${expression})+'\\n').encode())`;
  const result=name=>JSON.parse(messages.findLast(text=>text.startsWith(name+':')).slice(name.length+1));
  try {
    chapter('Start Blender with the Embree Wasm SIMD ray tracer');
    await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');await ready(page,failure);
    const manifest=await page.evaluate(()=>fetch('manifest.json').then(r=>r.json()));
    assert.equal(manifest.buildOptions.WITH_CYCLES_EMBREE,true);
    const adapter=await page.evaluate(async()=>{const a=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});return {vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description};});
    if(hardware)assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
    const source=fs.readFileSync(path.join(__dirname,'embree.py'));
    const reference=JSON.parse(fs.readFileSync(path.join(fixtures,'reference.json')));
    assert.equal(hash(source),reference.sourceSha256,'Regenerate the native Embree reference');
    const fixtureHashes={};
    for(const name of ['check.py','scene.blend','reference.json',...['bvh2','embree'].flatMap(b=>[1,2,3].map(f=>`${b}-${f}.png`))]) {
      const bytes=name==='check.py'?source:fs.readFileSync(path.join(fixtures,name));fixtureHashes[name]=hash(bytes);
      if(!name.endsWith('.png'))await page.evaluate(({directory,name,data})=>{
        Module.FS.mkdirTree(directory);Module.FS.writeFile(directory+'/'+name,Uint8Array.from(atob(data),c=>c.charCodeAt(0)));
      },{directory,name,data:bytes.toString('base64')});
    }
    chapter(fallback?'Load linked instances, curves, points and animation':'Render linked instances, curves, points, transparency and motion blur');
    await page.mouse.move(300,250);
    if(profile)await profile.start();
    await command(`embree_ns={'__file__':'${directory}/check.py','__name__':'embree_check'}; exec(compile(__import__('pathlib').Path('${directory}/check.py').read_text(),'embree.py','exec'),embree_ns); embree_ns['browser_check']('${directory}',render=${fallback?'False':'True'},benchmark=${benchmark?'True':'False'})`,'EMBREE_FINISHED');
    if(profile)await profile.stop();
    const report=result('EMBREE_DONE');
    assert.equal(report.sourceSha256,hash(source));assert.equal(report.scene.instances,64);assert.equal(report.scene.withEmbree,true);
    const images={};
    for(const name of await page.evaluate(directory=>Module.FS.readdir(directory).filter(n=>/^(bvh2|embree)-[123]\.png$|^embree\.blend$/.test(n)),directory)) {
      const bytes=Buffer.from(await page.evaluate(name=>Array.from(Module.FS.readFile(name)),directory+'/'+name));
      fs.writeFileSync(path.join(output,name),bytes);
      if(name.endsWith('.png'))images[name]={...compare(bytes,fs.readFileSync(path.join(fixtures,name))),bytes:bytes.length,sha256:hash(bytes)};
    }
    // Tie the requested backend to the original C++ geometry manager's log.
    const backends=[];let current=null;
    for(const text of messages) {
      if(text.startsWith('EMBREE_RENDER_BEGIN:'))current=JSON.parse(text.slice('EMBREE_RENDER_BEGIN:'.length));
      const match=text.match(/Using (BVH2|EMBREE) layout\./);
      if(match&&current){assert.equal(match[1],current.backend);backends.push({...current,actual:match[1]});}
    }
    assert.equal(backends.length,fallback?0:benchmark?10:4);
    if(!fallback) {
      chapter('Inspect the completed Cycles render in the original Image Editor');
      const display=`def embree_display():\n    area=max(bpy.context.screen.areas,key=lambda a:a.width*a.height)\n    area.type='IMAGE_EDITOR'\n    area.spaces.active.image=bpy.data.images.load('${directory}/embree-1.png',check_existing=False)\n    region=next(r for r in area.regions if r.type=='WINDOW')\n    with bpy.context.temp_override(area=area,region=region):\n        bpy.ops.image.view_all(fit_view=True)\n    area.tag_redraw()\nbpy.app.timers.register(embree_display,first_interval=1.5)`;
      await command(`import bpy; exec(${JSON.stringify(display)}); __import__('os').write(1,b'EMBREE_DISPLAY\\n')`,'EMBREE_DISPLAY');
      await page.waitForTimeout(3000);await page.screenshot({path:path.join(output,'embree-cycles.png')});
    }
    chapter('Reload offline and reopen the saved animated scene');
    await context.setOffline(true);await page.reload();await ready(page,failure);await page.mouse.move(300,250);
    await command(`embree_ns={'__file__':'${directory}/check.py','__name__':'embree_check'}; exec(compile(__import__('pathlib').Path('${directory}/check.py').read_text(),'embree.py','exec'),embree_ns); import bpy; bpy.ops.wm.open_mainfile(filepath='${directory}/embree.blend'); ${emit('EMBREE_REOPENED',"embree_ns['inspect_scene']()")}`,'EMBREE_REOPENED:');
    assert.deepEqual(result('EMBREE_REOPENED'),report.scene);
    const blend=fs.readFileSync(path.join(output,'embree.blend'));
    Object.assign(report,{blend:{bytes:blend.length,sha256:hash(blend)},bundleVersion:manifest.version,hardware,fallback,adapter,images,backends,
      browserVersion:browser.version(),offlineReopen:true,browserErrors:errors,fixtureHashes,chapters,
      profiled,videoCaptured:recordVideo,videoPath:path.join(output,'embree.webm')});
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify({bundleVersion:manifest.version,backends:backends.length,images:Object.keys(images),offlineReopen:true}));
  } catch(error) {await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});throw error;}
  finally {const video=page.video();await context.close();if(video)await video.saveAs(path.join(output,'embree.webm'));await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
