/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {chromium}=require('playwright');
const {ready,consoleCommand,messages}=require('./native.cjs');
const fallback=process.env.WEB_TEST_UI_FALLBACK==='1',hardware=process.env.WEB_TEST_HARDWARE==='1';
const output=path.resolve(process.env.WEB_USD_OUTPUT||`build/native/usd-evidence${hardware?'-hardware':''}${fallback?'-ui':''}`);
async function main(){
 fs.mkdirSync(output,{recursive:true});messages.length=0;fs.writeFileSync(path.join(output,'browser.log'),'');
 const browser=await chromium.launch({headless:true,args:hardware?['--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--use-vulkan=native','--disable-vulkan-surface']:['--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader','--enable-features=Vulkan','--use-vulkan=swiftshader']});
 const context=await browser.newContext({viewport:{width:1280,height:800},recordVideo:{dir:output,size:{width:1280,height:800}}});
 if(fallback)await context.addInitScript(()=>{delete WebAssembly.promising;delete WebAssembly.Suspending;});
 const page=await context.newPage(),errors=[],chapters=[],created=Date.now();
 let rejectFailure;const failure=new Promise((_,reject)=>rejectFailure=reject);failure.catch(()=>{});
 const fail=text=>{errors.push(text);rejectFailure(Error(text));};
 page.on('console',message=>{const text=message.text();messages.push(text);fs.appendFileSync(path.join(output,'browser.log'),text+'\n');if(/USD_WORKER_EXCEPTION|Aborted\(|Blender startup failed|Blender WebGPU error|Blender WebGPU device lost|Blender GPU job failed|WGPU_SH .*FAIL|WGPU_SHADER translate FAILED/.test(text))fail(text);});
 page.on('pageerror',error=>fail(error.stack||error.message));
 page.on('worker',worker=>worker.evaluate(()=>self.addEventListener('error',event=>console.error('USD_WORKER_EXCEPTION',event.error?.message,event.error?.stack))).catch(()=>{}));
 const chapter=title=>chapters.push({seconds:(Date.now()-created)/1000,title});
 const command=(code,marker)=>Promise.race([failure,consoleCommand(page,code,marker,()=>page.screenshot(),180000)]);
 try{
  chapter('Open original Blender with USD and MaterialX');
  await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');await ready(page,failure);
  const manifest=await page.evaluate(()=>fetch('manifest.json').then(r=>r.json()));
  for(const flag of ['WITH_USD','WITH_MATERIALX'])assert.equal(manifest.buildOptions[flag],true,flag);
  const adapter=await page.evaluate(async()=>{const a=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});return {vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description,features:[...a.features]};});
  if(hardware)assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
  const source=fs.readFileSync(path.join(__dirname,'usd_exchange.py'),'utf8'),sourceSha256=crypto.createHash('sha256').update(source).digest('hex');
  const fixtures=path.join(__dirname,'fixtures/usd-exchange'),ref=JSON.parse(fs.readFileSync(path.join(fixtures,'reference.json')));
  assert.equal(ref.sourceSha256,sourceSha256,'Regenerate native USD references');
  const fixtureHashes={};
  const fixtureNames=['reference.json','primitives.usda','desktop.usda','desktop.usdc','desktop.usdz',...fs.readdirSync(path.join(fixtures,'textures')).map(name=>'textures/'+name)];
  for(const name of fixtureNames){
   const bytes=fs.readFileSync(path.join(fixtures,name));fixtureHashes[name]=crypto.createHash('sha256').update(bytes).digest('hex');
   await page.evaluate(({name,base64})=>{const bytes=Uint8Array.from(atob(base64),c=>c.charCodeAt(0));for(const dir of ['/tmp/usd-exchange','/user/blender-web/home/usd-exchange']){Module.FS.mkdirTree((dir+'/'+name).slice(0,(dir+'/'+name).lastIndexOf('/')));Module.FS.writeFile(dir+'/'+name,bytes);}},{name,base64:bytes.toString('base64')});
  }
  await page.evaluate(source=>{Module.FS.writeFile('/tmp/usd_exchange.py',source);Module.FS.writeFile('/user/blender-web/home/usd-exchange/check.py',source);},source);
  chapter('Import native scenes; export USDA, USDC and USDZ with MaterialX and Python hooks');
  await page.mouse.move(300,250);
  await command("exec(compile(open('/tmp/usd_exchange.py').read(),'usd_exchange.py','exec')); browser_check('/tmp/usd-exchange')",'USD_EXCHANGE_FINISHED:');
  assert(!messages.includes('USD_EXCHANGE_FAILED'),'See browser.log');
  const report=JSON.parse(messages.find(t=>t.startsWith('USD_EXCHANGE_DONE:')).slice('USD_EXCHANGE_DONE:'.length));
  assert(report.staticPythonModules.length>=32,'Import every original static pxr extension');
  for(const name of ['results.json','browser.usda','browser.usdc','browser.usdz','usd-exchange.blend']){
   const sourcePath='/tmp/usd-exchange/'+(name.startsWith('browser.')?'export/':'')+name;
   const bytes=await page.evaluate(name=>Array.from(Module.FS.readFile(name)),sourcePath);fs.writeFileSync(path.join(output,name),Buffer.from(bytes));
  }
  const textures=await page.evaluate(()=>Module.FS.readdir('/tmp/usd-exchange/export/textures').filter(name=>name!=='.'&&name!=='..'));
  fs.mkdirSync(path.join(output,'textures'),{recursive:true});
  for(const name of textures){
   const bytes=await page.evaluate(name=>Array.from(Module.FS.readFile('/tmp/usd-exchange/export/textures/'+name)),name);
   fs.writeFileSync(path.join(output,'textures',name),Buffer.from(bytes));
  }
  await command("bpy.context.scene.frame_set(10); bpy.ops.object.select_all(action='DESELECT'); cube=next(o for o in bpy.context.scene.objects if o.type=='MESH'); cube.select_set(True); bpy.context.view_layer.objects.active=cube; [o.hide_set(True) for o in bpy.context.scene.objects if o.type in {'CAMERA','LIGHT'}]; [(setattr(s.shading,'color_type','MATERIAL'),setattr(s.region_3d,'view_distance',6),setattr(s.region_3d,'view_location',(2,-1,.5))) for a in bpy.context.screen.areas for s in a.spaces if s.type=='VIEW_3D']; __import__('os').write(1,b'USD_VIEW_READY\\n')",'USD_VIEW_READY');
  await page.screenshot({path:path.join(output,'usd-viewport.png')});await page.waitForTimeout(2500);
  chapter('USD is available in Blender\'s original export menu');
  await command("bpy.app.timers.register(lambda: (bpy.ops.wm.call_menu(name='TOPBAR_MT_file_export'), None)[1],first_interval=2); __import__('os').write(1,b'USD_MENU_SCHEDULED\\n')",'USD_MENU_SCHEDULED');
  await page.waitForTimeout(2000);await page.screenshot({path:path.join(output,'usd-export-menu.png')});await page.keyboard.press('Escape');
  chapter('Reload offline and repeat interchange from persistent browser storage');
  await context.setOffline(true);await page.reload();await ready(page,failure);await page.mouse.move(300,250);
  await command("exec(compile(open('/user/blender-web/home/usd-exchange/check.py').read(),'usd_exchange.py','exec')); browser_check('/user/blender-web/home/usd-exchange','USD_OFFLINE')",'USD_OFFLINE_FINISHED:');
  assert(!messages.includes('USD_OFFLINE_FAILED'));
  const offline=JSON.parse(messages.find(t=>t.startsWith('USD_OFFLINE_DONE:')).slice('USD_OFFLINE_DONE:'.length));
  for(const ext of ['usda','usdc','usdz'])assert(offline.formats[ext].bytes>0);
  assert.deepEqual(errors,[]);
  Object.assign(report,{bundleVersion:manifest.version,browserVersion:browser.version(),fallback,hardware,adapter,sourceSha256,fixtureHashes,browserErrors:errors,offlineReopen:true,chapters,videoPath:path.join(output,'usd-exchange.webm')});
  fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify({bundleVersion:report.bundleVersion,formats:Object.keys(report.formats),hookEvents:report.hookEvents,offlineReopen:true}));
 }catch(error){await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});throw error;}
 finally{const video=page.video();await context.close();if(video)await video.saveAs(path.join(output,'usd-exchange.webm'));await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
