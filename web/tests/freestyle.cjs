/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {chromium}=require('playwright'),{PNG}=require('pngjs');
const {ready,consoleCommand,messages}=require('./native.cjs');
const software=process.env.WEB_RENDER_SOFTWARE==='1';
const output=path.resolve(process.env.WEB_FREESTYLE_OUTPUT||'build/native/freestyle-evidence');
async function main(){
 fs.mkdirSync(output,{recursive:true});messages.length=0;fs.writeFileSync(path.join(output,'browser.log'),'');
 const browser=await chromium.launch({headless:true,args:software?['--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader','--enable-features=Vulkan','--use-vulkan=swiftshader']:['--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--use-vulkan=native','--disable-vulkan-surface']});
 const context=await browser.newContext({viewport:{width:1280,height:800},recordVideo:{dir:output,size:{width:1280,height:800}}});
 const page=await context.newPage(),errors=[],chapters=[],created=Date.now();
 const chapter=title=>chapters.push({seconds:(Date.now()-created)/1000,title});
 page.on('console',message=>{const text=message.text();messages.push(text);fs.appendFileSync(path.join(output,'browser.log'),text+'\n');if(/Aborted\(|Blender startup failed|Blender WebGPU error|Blender WebGPU device lost|Blender GPU job failed|WGPU_SH .*FAIL|WGPU_SHADER translate FAILED/.test(text))errors.push(text);});
 page.on('pageerror',error=>errors.push(error.stack));
 try{
  chapter('Start the original Blender interface');await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');await ready(page,new Promise(()=>{}));
  const manifest=await page.evaluate(async()=>(await(await fetch('manifest.json')).json()));assert.equal(manifest.buildOptions.WITH_FREESTYLE,true);
  const source=fs.readFileSync(path.join(__dirname,'freestyle.py'),'utf8'),sourceSha256=crypto.createHash('sha256').update(source).digest('hex');
  const fixture=path.join(__dirname,'fixtures/freestyle'),ref=JSON.parse(fs.readFileSync(path.join(fixture,'reference.json')));assert.equal(ref.sourceSha256,sourceSha256);
  await page.evaluate(source=>Module.FS.writeFile('/tmp/freestyle.py',source),source);await page.mouse.move(300,250);
  chapter('Enable Cycles CPU and Freestyle red contour strokes');
  await consoleCommand(page,"exec(compile(open('/tmp/freestyle.py').read(),'/tmp/freestyle.py','exec')); _settings=setup(); bpy.context.scene.render.threads_mode='AUTO'; __import__('os').write(1,('FREESTYLE_SETTINGS:'+__import__('json').dumps(_settings)+'\\n').encode())",'FREESTYLE_SETTINGS:');
  const settings=JSON.parse(messages.find(text=>text.startsWith('FREESTYLE_SETTINGS:')).slice('FREESTYLE_SETTINGS:'.length));assert.deepEqual(settings,ref.settings);
  chapter('Render the cube with original Freestyle stroke rendering');
  await page.mouse.move(600,400);await page.locator('canvas').focus();const previous=messages.length,started=Date.now();await page.keyboard.press('F12');
  while(!messages.slice(previous).some(text=>text.includes('WEB_GPU_JOB complete'))){if(errors.length)throw Error(errors.join('\n'));assert(Date.now()-started<300000,'Freestyle render must finish');await page.waitForTimeout(100);}
  await page.waitForTimeout(2000);await page.mouse.move(600,400);await page.keyboard.press('Home');
  for(let i=0;i<5;i++){await page.mouse.wheel(0,-100);await page.waitForTimeout(100);}
  await page.waitForTimeout(1500);await page.screenshot({path:path.join(output,'freestyle-browser.png')});
  await consoleCommand(page,"bpy.data.images['Render Result'].save_render('/tmp/freestyle.png',scene=bpy.context.scene); __import__('os').write(1,b'FREESTYLE_SAVED\\n')",'FREESTYLE_SAVED');
  const bytes=Buffer.from(await page.evaluate(()=>Array.from(Module.FS.readFile('/tmp/freestyle.png'))));fs.writeFileSync(path.join(output,'freestyle.png'),bytes);
  const image=PNG.sync.read(bytes),expected=PNG.sync.read(fs.readFileSync(path.join(fixture,'freestyle.png')));assert.deepEqual([image.width,image.height],[256,256]);
  let redPixels=0,absolute=0,outliers=0,count=0;
  for(let i=0;i<image.data.length;i+=4){if(image.data[i]>100&&image.data[i]>image.data[i+1]*2&&image.data[i]>image.data[i+2]*2)redPixels++;for(let j=0;j<3;j++){const d=Math.abs(image.data[i+j]-expected.data[i+j]);absolute+=d;count++;if(d>20)outliers++;}}
  const meanAbsoluteChannelError=absolute/count,outlierFraction=outliers/count;
  assert(redPixels>500,'The rendered image must contain actual Freestyle contour strokes');assert(meanAbsoluteChannelError<3,{meanAbsoluteChannelError});assert(outlierFraction<.01,{outlierFraction});assert.deepEqual(errors,[]);
  chapter('Save the Freestyle scene for desktop Blender verification');
  await consoleCommand(page,"bpy.ops.wm.save_as_mainfile(filepath='/tmp/freestyle-scene.blend',check_existing=False,compress=True); __import__('os').write(1,b'FREESTYLE_BLEND_SAVED\\n')",'FREESTYLE_BLEND_SAVED');
  const blend=Buffer.from(await page.evaluate(()=>Array.from(Module.FS.readFile('/tmp/freestyle-scene.blend'))));
  fs.writeFileSync(path.join(output,'freestyle-scene.blend'),blend);
  assert.deepEqual(errors,[]);
  const report={bundleVersion:manifest.version,browserVersion:browser.version(),softwareDisplay:software,settings,redPixels,meanAbsoluteChannelError,outlierFraction,browserErrors:errors,sourceSha256,referenceImageSha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(fixture,'freestyle.png'))).digest('hex'),chapters,videoPath:path.join(output,'freestyle.webm')};
  report.blendSha256=crypto.createHash('sha256').update(blend).digest('hex');
  fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify(report));
 }catch(error){await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});throw error;}
 finally{const video=page.video();await context.close();if(video)await video.saveAs(path.join(output,'freestyle.webm'));await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
