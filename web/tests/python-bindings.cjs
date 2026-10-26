/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');
const {chromium}=require('playwright');
const {ready,consoleCommand,messages}=require('./native.cjs');
const hardware=process.env.WEB_TEST_HARDWARE==='1';
const fallback=process.env.WEB_TEST_UI_FALLBACK==='1';
const output=path.resolve(process.env.WEB_PYTHON_BINDINGS_OUTPUT||`build/native/python-bindings-evidence${hardware?'-hardware':''}${fallback?'-ui':''}`);
const directory='/user/blender-web/home/python-bindings';
const hash=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
async function main(){
  fs.mkdirSync(output,{recursive:true});messages.length=0;fs.writeFileSync(path.join(output,'browser.log'),'');
  const browser=await chromium.launch({headless:true,args:hardware?[
    '--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--use-vulkan=native','--disable-vulkan-surface',
  ]:[
    '--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader','--enable-features=Vulkan','--use-vulkan=swiftshader',
  ]});
  const context=await browser.newContext({viewport:{width:1280,height:800},recordVideo:{dir:output,size:{width:1280,height:800}}});
  if(fallback)await context.addInitScript(()=>{delete WebAssembly.promising;delete WebAssembly.Suspending;});
  const page=await context.newPage(),errors=[],startupPythonErrors=[],chapters=[],started=Date.now();
  let checking=false,rejectFailure;
  const failure=new Promise((_,reject)=>rejectFailure=reject);failure.catch(()=>{});
  const fail=text=>{errors.push(text);fs.appendFileSync(path.join(output,'browser.log'),'BROWSER_FAILURE:'+text+'\n');rejectFailure(Error(text));};
  page.on('console',message=>{const text=message.text();messages.push(text);fs.appendFileSync(path.join(output,'browser.log'),text+'\n');
    if(/Traceback \(most recent call last\)/.test(text)){
      if(checking)fail(text);else startupPythonErrors.push(text);
    }
    if(/PYTHON_BINDINGS_FAILED:|Aborted\(|Blender startup failed|Blender UI frame failed|Blender GPU job failed|Blender WebGPU (error|device lost)/.test(text))fail(text);
  });
  page.on('pageerror',error=>fail(error.stack||error.message));
  page.on('crash',()=>fail('Chromium renderer crashed during the Blender binding checks'));
  const command=(code,marker)=>Promise.race([failure,consoleCommand(page,code,marker,()=>page.screenshot(),300000)]);
  const chapter=title=>chapters.push({seconds:(Date.now()-started)/1000,title});
  try{
    chapter('Start original Blender with the image, color, volume and material bindings');
    await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');await ready(page,failure);checking=true;
    const manifest=await page.evaluate(()=>fetch('manifest.json').then(r=>r.json()));
    const adapter=await page.evaluate(async()=>{const a=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});return {vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description};});
    if(hardware)assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
    const source=fs.readFileSync(path.join(__dirname,'../native/tests/python-bindings.py'));
    const avifSource=fs.readFileSync(path.join(__dirname,'../native/tests/avif.py'));
    await page.evaluate(({data,directory})=>{Module.FS.mkdirTree(directory);Module.FS.writeFile(directory+'/check.py',Uint8Array.from(atob(data),c=>c.charCodeAt(0)));},{data:source.toString('base64'),directory});
    await page.evaluate(({data,directory})=>Module.FS.writeFile(directory+'/avif.py',Uint8Array.from(atob(data),c=>c.charCodeAt(0))),{data:avifSource.toString('base64'),directory});
    await page.mouse.move(300,250);chapter('Run original Python APIs and export native-readable files');
    const execute=`try:\n    pb_report=__import__('runpy').run_path('${directory}/check.py')['run']('${directory}')\n    __import__('os').write(1,b'PYTHON_BINDINGS_DONE\\n')\nexcept BaseException:\n    __import__('os').write(2,('PYTHON_BINDINGS_FAILED:'+__import__('traceback').format_exc()+'\\n').encode())`;
    await command(`exec(${JSON.stringify(execute)})`,'PYTHON_BINDINGS_DONE');
    chapter('Export AVIF through Blender at 8, 10 and 12 bits with odd-width byte input');
    const checkAVIF=`try:\n    pb_avif=__import__('runpy').run_path('${directory}/avif.py')['run']('${directory}')\n    __import__('pathlib').Path('${directory}/avif.json').write_text(__import__('json').dumps(pb_avif))\n    __import__('os').write(1,b'PYTHON_BINDINGS_AVIF\\n')\nexcept BaseException:\n    __import__('os').write(2,('PYTHON_BINDINGS_FAILED:'+__import__('traceback').format_exc()+'\\n').encode())`;
    await command(`exec(${JSON.stringify(checkAVIF)})`,'PYTHON_BINDINGS_AVIF');
    const overview='Original Blender Python binding checks\n\nOpenColorIO: NumPy transforms, config roundtrip, shader generation\nOpenImageIO: EXR/TIFF/PNG, NumPy, resize, pixel statistics\nOpenVDB: nine grid types, 64-bit values, NumPy, file exchange\nMaterialX: document roundtrip and GLSL/MSL/OSL/MDL generation\nMaterialX image and mesh utilities: passed\nAVIF: nine exports, 8/10/12 bits, BW/RGB/RGBA, odd width\nThreaded calls and invalid-input rejection: passed\n\nPackage versions\n';
    await command(`import bpy,json,os; bpy.data.texts.new('Browser Python binding proof').write(json.dumps(pb_report,indent=2)); pb_text=bpy.data.texts.new('Python binding results'); pb_text.write(${JSON.stringify(overview)}+'\\n'.join(k+' '+v for k,v in pb_report['versions'].items())); pb_text.cursor_set(0); bpy.ops.wm.save_as_mainfile(filepath='${directory}/python-bindings.blend'); os.write(1,b'PYTHON_BINDINGS_SAVED\\n')`,'PYTHON_BINDINGS_SAVED');
    const files={};
    const names=['config.ocio','color-transform.glsl','image.exr','image.tif','image.png','volume.vdb','material.mtlx','shader-genglsl.txt','shader-genmsl.txt','shader-genosl.txt','shader-genmdl.txt','python-bindings.json','python-bindings.blend','avif.json'];
    for(const bits of [8,10,12])for(const mode of ['BW','RGB','RGBA'])for(const suffix of ['','.f32','.blender.f32'])names.push(`avif-${bits}-${mode}.avif${suffix}`);
    for(const name of names){
      const bytes=Buffer.from(await page.evaluate(name=>Array.from(Module.FS.readFile(name)),directory+'/'+name));
      fs.writeFileSync(path.join(output,name),bytes);files[name]={sha256:hash(bytes),bytes:bytes.length};
    }
    const report=JSON.parse(fs.readFileSync(path.join(output,'python-bindings.json')));
    const avif=JSON.parse(fs.readFileSync(path.join(output,'avif.json')));
    assert.equal(avif.cases.length,9);assert.equal(avif.oddWidth,37);assert(avif.truncatedInputRejected&&avif.decoderRecovery);
    assert.equal(report.volume.gridTypes.length,9);assert.equal(report.material.shaders.length,4);
    assert.equal(report.volume.gridTypes.find(g=>g.name==='Int64Grid').value,'18014398509481991');
    assert(report.originalPackageWrappers&&report.color.invalidColorSpaceRejected&&report.volume.missingGridRejected&&report.material.xmlRejection);
    chapter('Inspect the exported 12-bit AVIF in Blender Image Editor');
    const displayImage=`def pb_display_image():\n    area=max(bpy.context.screen.areas,key=lambda a:a.width*a.height)\n    area.type='IMAGE_EDITOR'\n    area.spaces.active.image=bpy.data.images.load('${directory}/avif-12-RGBA.avif',check_existing=False)\n    region=next(r for r in area.regions if r.type=='WINDOW')\n    with bpy.context.temp_override(area=area,region=region):\n        bpy.ops.image.view_all(fit_view=True)\n    area.tag_redraw()\nbpy.app.timers.register(pb_display_image,first_interval=1.5)`;
    await command(`exec(${JSON.stringify(displayImage)}); os.write(1,b'PYTHON_BINDINGS_IMAGE\\n')`,'PYTHON_BINDINGS_IMAGE');
    await page.waitForTimeout(4000);await page.screenshot({path:path.join(output,'avif-browser.png')});
    chapter('Inspect the results in the original Text Editor');
    const display=`def pb_display():\n    area=max(bpy.context.screen.areas,key=lambda a:a.width*a.height)\n    area.type='TEXT_EDITOR'\n    area.spaces.active.text=pb_text\n    area.tag_redraw()\nbpy.app.timers.register(pb_display,first_interval=1.5)`;
    await command(`exec(${JSON.stringify(display)}); os.write(1,b'PYTHON_BINDINGS_DISPLAY\\n')`,'PYTHON_BINDINGS_DISPLAY');
    await page.waitForTimeout(4000);await page.screenshot({path:path.join(output,'python-bindings-browser.png')});
    chapter('Reopen the saved scene and volume after an offline reload');
    await context.setOffline(true);checking=false;await page.reload();await ready(page,failure);checking=true;await page.mouse.move(300,250);
    const reopen=`import bpy,json,os,openvdb; bpy.ops.wm.open_mainfile(filepath='${directory}/python-bindings.blend'); pb_saved=json.loads(bpy.data.texts['Browser Python binding proof'].as_string()); pb_grids,pb_meta=openvdb.readAll('${directory}/volume.vdb'); assert len(pb_grids)==10; assert next(g for g in pb_grids if g.name=='Int64Grid').getAccessor().getValue((-1,2,3))==(1<<54)+7; os.write(1,('PYTHON_BINDINGS_REOPENED:'+json.dumps(pb_saved)+'\\n').encode())`;
    await command(reopen,'PYTHON_BINDINGS_REOPENED:');
    const reopened=JSON.parse(messages.findLast(text=>text.startsWith('PYTHON_BINDINGS_REOPENED:')).slice('PYTHON_BINDINGS_REOPENED:'.length));
    assert.deepEqual(reopened,report);assert.deepEqual(errors,[]);assert.deepEqual(startupPythonErrors,[]);
    const acceptance={bundleVersion:manifest.version,sourceSha256:hash(source),avifSourceSha256:hash(avifSource),runnerSourceSha256:hash(fs.readFileSync(__filename)),
      hardware,fallback,adapter,browserVersion:browser.version(),measuredAt:new Date().toISOString(),report,avif,files,
      offlineReopen:true,startupPythonErrors,browserErrors:errors,chapters,videoPath:path.join(output,'python-bindings.webm')};
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(acceptance,null,2)+'\n');
    console.log(JSON.stringify({bundleVersion:manifest.version,versions:report.versions,gridTypes:9,shaderGenerators:4,offlineReopen:true}));
  }catch(error){await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});throw error;}
  finally{const video=page.video();await context.close();if(video)await video.saveAs(path.join(output,'python-bindings.webm'));await browser.close();}
}
if(require.main===module)main().catch(error=>{console.error(error);process.exitCode=1;});
