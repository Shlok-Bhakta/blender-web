/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {chromium}=require('playwright');
const {PNG}=require('pngjs');
const {ready,consoleCommand,messages}=require('./native.cjs');
const fallback=process.env.WEB_TEST_UI_FALLBACK==='1',hardware=process.env.WEB_TEST_HARDWARE==='1';
const output=path.resolve(process.env.WEB_IMAGE_VECTOR_OUTPUT||`build/native/image-vector-evidence${hardware?'-hardware':''}${fallback?'-ui':''}`);
async function main(){
 fs.mkdirSync(output,{recursive:true});messages.length=0;fs.writeFileSync(path.join(output,'browser.log'),'');
 const browser=await chromium.launch({headless:true,args:hardware?['--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--use-vulkan=native','--disable-vulkan-surface']:['--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader','--enable-features=Vulkan','--use-vulkan=swiftshader']});
 const context=await browser.newContext({viewport:{width:1280,height:800},recordVideo:{dir:output,size:{width:1280,height:800}}});
 if(fallback)await context.addInitScript(()=>{delete WebAssembly.promising;delete WebAssembly.Suspending;});
 const page=await context.newPage(),errors=[],chapters=[],created=Date.now();
 let rejectFailure;const failure=new Promise((_,reject)=>rejectFailure=reject);failure.catch(()=>{});
 const fail=text=>{errors.push(text);rejectFailure(Error(text));};
 page.on('console',message=>{const text=message.text();messages.push(text);fs.appendFileSync(path.join(output,'browser.log'),text+'\n');if(/Aborted\(|Blender startup failed|Blender WebGPU error|Blender WebGPU device lost|Blender GPU job failed|WGPU_SH .*FAIL|WGPU_SHADER translate FAILED/.test(text))fail(text);});
 page.on('worker',worker=>worker.evaluate(()=>self.addEventListener('error',event=>console.error('IMAGE_VECTOR_WORKER_EXCEPTION',JSON.stringify(event.error?.message),event.error?.stack))).catch(()=>{}));
 page.on('pageerror',error=>fail(error.stack||error.message||'Unhandled Wasm exception; see browser.log'));page.on('crash',()=>fail('Browser renderer crashed'));
 const command=(source,marker)=>Promise.race([failure,consoleCommand(page,source,marker,undefined,180000)]);
 const prepareView=()=>command("import bpy; _spare=next(a for a in bpy.context.screen.areas if a.type=='PROPERTIES'); _spare.type='VIEW_3D'; __import__('os').write(1,b'VECTOR_EXPORT_VIEW_READY\\n')",'VECTOR_EXPORT_VIEW_READY');
 const chapter=title=>chapters.push({seconds:(Date.now()-created)/1000,title});
 try{
  chapter('Start Blender with image and vector dependencies');
  await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');await ready(page,failure);
  const manifest=await page.evaluate(async()=>(await(await fetch('manifest.json')).json()));
  for(const flag of ['WITH_IMAGE_WEBP','WITH_IMAGE_OPENJPEG','WITH_IMAGE_CINEON','WITH_PUGIXML','WITH_HARU','WITH_POTRACE','WITH_IO_GREASE_PENCIL'])assert.equal(manifest.buildOptions[flag],true,flag);
  const adapter=await page.evaluate(async()=>{const a=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});return {vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description,features:[...a.features]};});
  const viewportDrawing=adapter.features.includes('dual-source-blending');
  if(hardware)assert(viewportDrawing,'Hardware viewport validation requires dual-source blending');
  if(hardware)assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
  const source=fs.readFileSync(path.join(__dirname,'image_vector.py'),'utf8'),sourceSha256=crypto.createHash('sha256').update(source).digest('hex');
  const fixtures=path.join(__dirname,'fixtures/image-vector'),ref=JSON.parse(fs.readFileSync(path.join(fixtures,'reference.json')));
  assert.equal(ref.sourceSha256,sourceSha256,'Regenerate the desktop image/vector reference');
  const fixtureHashes={};
  for(const name of ['reference.json','shapes.svg','trace.png','pattern.webp','pattern.jp2','pattern.cin','pattern.dpx']){
   const bytes=fs.readFileSync(path.join(fixtures,name));fixtureHashes[name]=crypto.createHash('sha256').update(bytes).digest('hex');
   await page.evaluate(({name,base64})=>{const data=Uint8Array.from(atob(base64),c=>c.charCodeAt(0));for(const dir of ['/tmp/image-vector','/user/blender-web/home/image-vector']){Module.FS.mkdirTree(dir);Module.FS.writeFile(dir+'/'+name,data);}},{name,base64:bytes.toString('base64')});
  }
  await page.evaluate(source=>{Module.FS.writeFile('/tmp/image-vector.py',source);Module.FS.writeFile('/user/blender-web/home/image-vector/check.py',source);},source);
  chapter('Compare image codecs, SVG import and Potrace with desktop Blender');await page.mouse.move(300,250);
  await prepareView();
  await command("exec(compile(open('/tmp/image-vector.py').read(),'/tmp/image-vector.py','exec')); browser_check('/tmp/image-vector',draw="+(viewportDrawing?'True':'False')+")",'IMAGE_VECTOR_FINISHED:');
  assert(!messages.includes('IMAGE_VECTOR_FAILED'),'See browser.log for the Python failure');
  const report=JSON.parse(messages.find(text=>text.startsWith('IMAGE_VECTOR_DONE:')).slice('IMAGE_VECTOR_DONE:'.length));
  for(const name of ['results.json','export.svg','export.pdf','pattern.webp','pattern.jp2','pattern.cin','pattern.dpx']){
   const bytes=await page.evaluate(name=>Array.from(Module.FS.readFile('/tmp/image-vector/'+name)),name);fs.writeFileSync(path.join(output,name),Buffer.from(bytes));
  }
  chapter(viewportDrawing?'Native SVG artwork in the original Grease Pencil viewport':'Image/vector IO; viewport needs dual-source blending');
  const view="from mathutils import Quaternion; [(setattr(s.shading,'color_type','VERTEX'),setattr(s.region_3d,'view_distance',3),setattr(s.region_3d,'view_location',(0,0,0)),setattr(s.region_3d,'view_rotation',Quaternion((0.7071068,0.7071068,0,0))),setattr(s.overlay,'show_floor',False),setattr(s.overlay,'show_axis_x',False),setattr(s.overlay,'show_axis_y',False)) for a in bpy.context.screen.areas for s in a.spaces if s.type=='VIEW_3D']; __import__('os').write(1,('IMAGE_VECTOR_BUILD_OPTIONS:'+__import__('json').dumps({n:getattr(bpy.app.build_options,n) for n in dir(bpy.app.build_options) if not n.startswith('_') and isinstance(getattr(bpy.app.build_options,n),bool)})+'\\nVECTOR_VIEW_READY\\n').encode())";
  await command(view,'VECTOR_VIEW_READY');
  report.buildOptions=JSON.parse(messages.find(text=>text.startsWith('IMAGE_VECTOR_BUILD_OPTIONS:')).slice('IMAGE_VECTOR_BUILD_OPTIONS:'.length));
  let bluePixels=0;const deadline=Date.now()+60000;
  while(viewportDrawing){
   if(errors.length)throw Error(errors.join('\n'));
   const bytes=await page.screenshot(),png=PNG.sync.read(bytes);bluePixels=0;
   for(let y=100;y<650;y++)for(let x=50;x<1000;x++){const i=(y*png.width+x)*4;if(png.data[i]<160&&png.data[i+1]>90&&png.data[i+2]>130&&png.data[i+2]>png.data[i]*1.2)bluePixels++;}
   if(bluePixels>500){fs.writeFileSync(path.join(output,'vector.png'),bytes);break;}
   await page.waitForTimeout(500);
   if(Date.now()>=deadline)break;
  }
  if(viewportDrawing)assert(bluePixels>500,'Grease Pencil must visibly draw the authored blue fill');report.bluePixels=bluePixels;report.viewportDrawing=viewportDrawing;
  report.viewportRequirement=viewportDrawing?null:'dual-source-blending is unavailable on this adapter';
  await page.waitForTimeout(2500);
  chapter('Reload offline and reopen images and vectors from browser storage');
  await context.setOffline(true);await page.reload();await ready(page,failure);await page.mouse.move(300,250);
  await prepareView();
  await command("exec(compile(open('/user/blender-web/home/image-vector/check.py').read(),'image-vector.py','exec')); browser_check('/user/blender-web/home/image-vector','IMAGE_VECTOR_OFFLINE',draw="+(viewportDrawing?'True':'False')+")",'IMAGE_VECTOR_OFFLINE_FINISHED:');
  assert(!messages.includes('IMAGE_VECTOR_OFFLINE_FAILED'));
  const offline=JSON.parse(messages.find(text=>text.startsWith('IMAGE_VECTOR_OFFLINE_DONE:')).slice('IMAGE_VECTOR_OFFLINE_DONE:'.length));
  report.offlineImages=offline.images;report.offlineReopen=true;
  assert.deepEqual(errors,[]);
  Object.assign(report,{bundleVersion:manifest.version,browserVersion:browser.version(),fallback,hardware,adapter,sourceSha256,fixtureHashes,browserErrors:errors,chapters,videoPath:path.join(output,'image-vector.webm')});
  fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify(report));
 }catch(error){await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});throw error;}
 finally{const video=page.video();await context.close();if(video)await video.saveAs(path.join(output,'image-vector.webm'));await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
