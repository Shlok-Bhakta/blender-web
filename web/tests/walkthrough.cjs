/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {chromium}=require('playwright');
const {ready,consoleCommand,messages}=require('./native.cjs');
const output=path.resolve(__dirname,'../../build/native/walkthrough');
async function main() {
  fs.mkdirSync(output,{recursive:true});
  fs.writeFileSync(path.join(output,'browser.log'),'');
  messages.length=0;
  const browser=await chromium.launch({headless:true,args:[
    '--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan',
    '--use-vulkan=native','--disable-vulkan-surface',
  ]});
  const context=await browser.newContext({viewport:{width:1280,height:800},
    recordVideo:{dir:output,size:{width:1280,height:800}}});
  const created=Date.now(),chapters=[];
  const page=await context.newPage();
  const errors=[];
  page.on('console',message=>{
    const text=message.text();messages.push(text);
    fs.appendFileSync(path.join(output,'browser.log'),text+'\n');
    if (/Blender startup failed|Aborted\(|Blender WebGPU error|Blender WebGPU device lost/.test(text)) errors.push(text);
  });
  page.on('pageerror',e=>errors.push(e.stack));
  const chapter=title=>chapters.push({seconds:(Date.now()-created)/1000,title});
  try {
    chapter('Loading Blender; runtime files are cached for later visits');
    await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');
    await ready(page,new Promise(()=>{}));
    chapter('Original Blender UI and default cube');
    await page.waitForTimeout(3000);
    await page.evaluate(source=>Module.FS.writeFile('/tmp/walkthrough.py',source),fs.readFileSync(path.join(__dirname,'walkthrough.py'),'utf8'));
    await page.mouse.move(300,250);
    await consoleCommand(page,"exec(compile(open('/tmp/walkthrough.py').read(),'/tmp/walkthrough.py','exec')); __import__('os').write(1,b'DEMO_LOADED\\n')",'DEMO_LOADED');
    for (const [command,title,slug] of [
      ["boolean('EXACT')",'GMP exact Boolean: cylinder cut through a cube','exact'],
      ["boolean('MANIFOLD')",'Manifold Boolean: the same cylinder cut','manifold'],
      ['remesh()','Native QuadriFlow remeshing: visible quad edges','quadriflow'],
      ['wave()','NumPy builds a 1,600-vertex wave inside Blender','numpy'],
    ]) {
      chapter(title);
      await consoleCommand(page,`${command}; __import__('os').write(1,b'DEMO_${slug}\\n')`,`DEMO_${slug}`);
      await page.waitForTimeout(4000);
      await page.screenshot({path:path.join(output,slug+'.png')});
    }
    chapter('Save a native .blend file to persistent browser storage');
    await consoleCommand(page,"save(); __import__('os').write(1,b'DEMO_SAVED\\n')",'DEMO_SAVED');
    await page.waitForTimeout(2000);
    chapter('Offline reload: cached Blender starts with no network');
    await context.setOffline(true);
    await page.reload();
    await ready(page,new Promise(()=>{}));
    chapter('Reopen the saved NumPy wave after an offline reload');
    await page.mouse.move(300,250);
    await consoleCommand(page,"import bpy, os; bpy.ops.wm.open_mainfile(filepath='/user/blender-web/home/web-walkthrough.blend'); assert len(bpy.data.objects['NumPy 2.3.4 wave'].data.vertices)==1600; os.write(1,b'DEMO_REOPENED\\n')",'DEMO_REOPENED');
    await page.waitForTimeout(5000);
    await page.screenshot({path:path.join(output,'offline-reopened.png')});
    assert.deepEqual(errors,[]);
    const report={chapters,offlineReload:true,persistentBlend:true,errors,
      bundleVersion:await page.evaluate(async()=>(await(await fetch('manifest.json')).json()).version)};
    const video=page.video();
    await context.close();
    report.videoPath=await video.path();
    fs.writeFileSync(path.join(output,'recording.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report));
  } finally {await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
