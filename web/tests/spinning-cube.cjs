/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const os = require('node:os');
const {performance} = require('node:perf_hooks');
const {chromium} = require('playwright');
const {PNG} = require('pngjs');
const {ready, consoleCommand, messages} = require('./native.cjs');
const {sampleBrowserResources} = require('./browser-resources.cjs');
const output=path.resolve(process.env.WEB_SPIN_OUTPUT || 'build/native/spinning-cube');
const runCount=Number(process.env.WEB_SPIN_RUNS || 3);
const frameCount=Number(process.env.WEB_SPIN_FRAMES || 50);
assert(Number.isInteger(runCount) && runCount>0 && runCount<=10);
assert(Number.isInteger(frameCount) && frameCount>1 && frameCount<=250);
const args=['--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--use-vulkan=native','--disable-vulkan-surface'];
const percentile=(values,p)=>[...values].sort((a,b)=>a-b)[Math.ceil(values.length*p)-1];
const summarize=values=>{
  const sorted=[...values].sort((a,b)=>a-b), middle=Math.floor(sorted.length/2);
  return {min:sorted[0],median:sorted.length%2 ? sorted[middle] : (sorted[middle-1]+sorted[middle])/2,
    p95:percentile(values,.95),max:sorted.at(-1),mean:values.reduce((a,b)=>a+b,0)/values.length};
};
async function run(index) {
  const directory=path.join(output,`run-${index+1}`);
  fs.mkdirSync(path.join(directory,'frames'),{recursive:true});
  messages.length=0;
  const log=fs.openSync(path.join(directory,'browser.log'),'w');
  const browser=await chromium.launch({headless:true,args});
  // No screen video in the timed region. Animation is encoded from the real PNGs afterward.
  const context=await browser.newContext({viewport:{width:1280,height:800}});
  const page=await context.newPage();
  const fatal=[], rows=[];
  let initialized, completed, gpuJobStart, gpuJobEnd, firstReadback;
  let resources;
  const t0=performance.now();
  page.on('console',message=>{
    const text=message.text(); messages.push(text); fs.writeSync(log,text+'\n');
    if (/Aborted\(|Blender WebGPU error|Blender WebGPU device lost|Blender GPU job failed|WGPU_SH .*FAIL|WGPU_SHADER translate FAILED|SPIN_CANCEL:/.test(text)) fatal.push(text);
    if (text.includes('File \"/tmp/spinning-cube.py\"')) fatal.push(text);
    if (text==='WEB_GPU_JOB begin') gpuJobStart=performance.now();
    if (text==='WEB_GPU_JOB complete') gpuJobEnd=performance.now();
    if (text.includes('WEB_READBACK 1920x1080 channels=4') && text.includes('success=1') && !firstReadback) firstReadback=performance.now();
    if (text.startsWith('SPIN_BEGIN:')) initialized=JSON.parse(text.slice(11));
    if (text.startsWith('SPIN_COMPLETE:')) completed=JSON.parse(text.slice(14));
    if (text.startsWith('SPIN_FRAME:')) {
      const row=JSON.parse(text.slice(11)); rows.push(row);
      fs.writeFileSync(path.join(directory,'progress.json'),JSON.stringify({frames:rows.length,last:row}));
      if (rows.length===1 || rows.length%10===0) console.log(`Run ${index+1}: frame ${row.frame}/${frameCount}, elapsed ${row.elapsedSeconds.toFixed(2)}s, frame ${row.frameSeconds.toFixed(2)}s`);
    }
  });
  page.on('pageerror',error=>{fatal.push(error.stack);fs.writeSync(log,'PAGE_ERROR '+error.stack+'\n');});
  try {
    await page.goto(process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/');
    await ready(page,new Promise(()=>{}));
    const startupSeconds=(performance.now()-t0)/1000;
    const environment=await page.evaluate(async()=>{
      const a=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});
      return {hardwareConcurrency:navigator.hardwareConcurrency,isolated:crossOriginIsolated,jspi:typeof WebAssembly.promising==='function',
        adapter:{vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description,features:[...a.features].sort()},
        hasFileApi:typeof Module.FS?.writeFile==='function'};
    });
    assert(environment.jspi && environment.hasFileApi);
    const source=`SPIN_FRAME_COUNT=${frameCount}\n`+fs.readFileSync(path.join(__dirname,'spinning-cube.py'),'utf8');
    await page.evaluate(source=>Module.FS.writeFile('/tmp/spinning-cube.py',source),source);
    await page.mouse.move(300,250);
    await consoleCommand(page,"exec(compile(open('/tmp/spinning-cube.py').read(), '/tmp/spinning-cube.py', 'exec'))",'SPIN_READY:');
    await page.screenshot({path:path.join(directory,'before.png')});
    resources=await sampleBrowserResources(browser);
    const requested=performance.now();
    await page.keyboard.press('Control+F12');
    const deadline=Date.now()+30*60*1000;
    while (!gpuJobEnd) {
      if (fatal.length) throw new Error(fatal.join('\n'));
      if (Date.now()>deadline) throw new Error('Native animation render timed out');
      await page.waitForTimeout(100);
    }
    const utilization=await resources.stop(); resources=null;
    fs.writeFileSync(path.join(directory,'resources.json'),JSON.stringify(utilization,null,2)+'\n');
    assert.deepEqual(fatal,[]);
    assert(initialized && completed,'Native animation callbacks must complete');
    assert.equal(rows.length,frameCount);
    assert.equal(completed.frames,frameCount);
    assert.deepEqual(completed.settingsAfter,initialized.settings,'Rendering must preserve quality settings');
    const readbacks=messages.filter(t=>t.includes('WEB_READBACK 1920x1080 channels=4')&&t.includes('success=1'));
    assert.equal(readbacks.length,frameCount,'Each animation frame must receive a real Render Result');
    const files=await page.evaluate(()=>Module.FS.readdir('/tmp/spinning-cube').filter(name=>name.endsWith('.png')).sort());
    assert.equal(files.length,frameCount);
    const hashes=[], foreground=[];
    let firstPixels;
    let visibleDifference=0;
    for (let i=0;i<frameCount;i++) {
      const row=rows[i]; assert.equal(row.frame,i+1);
      const expectedAngle=2*Math.PI*i/frameCount;
      assert(Math.abs(row.keyframedRotationZ-expectedAngle)<1e-4,`Wrong cube pose at frame ${i+1}: ${row.keyframedRotationZ}`);
      const base64=await page.evaluate(file=>{
        const bytes=Module.FS.readFile(file); const pieces=[];
        for(let start=0;start<bytes.length;start+=32768) pieces.push(String.fromCharCode(...bytes.subarray(start,start+32768)));
        return btoa(pieces.join(''));
      },row.path);
      const bytes=Buffer.from(base64,'base64');
      assert.equal(bytes.length,row.bytes);
      fs.writeFileSync(path.join(directory,'frames',`${String(i+1).padStart(4,'0')}.png`),bytes);
      const png=PNG.sync.read(bytes);
      assert.deepEqual([png.width,png.height],[1920,1080]);
      let opaque=0, visible=0;
      for (let pixel=0;pixel<png.data.length;pixel+=4) {
        if (png.data[pixel+3]===255) opaque++;
        if (Math.abs(png.data[pixel]-png.data[0])+Math.abs(png.data[pixel+1]-png.data[1])+Math.abs(png.data[pixel+2]-png.data[2])>30) visible++;
        if (i===Math.max(1,Math.floor(frameCount/8)) && Math.abs(png.data[pixel]-firstPixels[pixel])+Math.abs(png.data[pixel+1]-firstPixels[pixel+1])+Math.abs(png.data[pixel+2]-firstPixels[pixel+2])>30) visibleDifference++;
      }
      if (!firstPixels) firstPixels=png.data;
      assert(opaque>1920*1080*.99 && visible>30000 && visible<1920*1080*.6,'Expected an opaque cube render');
      foreground.push(visible);
      hashes.push(crypto.createHash('sha256').update(png.data).digest('hex'));
    }
    assert(visibleDifference>10000,'Rotation must visibly change the rendered image');
    assert(new Set(hashes).size>=Math.floor(frameCount/2),'Expected distinct rendered poses');
    await page.mouse.move(600,400); await page.keyboard.press('Home');
    await page.waitForTimeout(500);
    await page.screenshot({path:path.join(directory,'result.png')});
    const bundleVersion=await page.evaluate(async()=>(await(await fetch('manifest.json')).json()).version);
    const report={run:index+1,browserVersion:browser.version(),bundleVersion,environment,
      frames:frameCount,startupSeconds,firstSavedFrameSeconds:completed.firstFrameSeconds,
      firstReadbackSeconds:(firstReadback-gpuJobStart)/1000,
      renderAnimationSeconds:completed.totalSeconds,gpuJobSeconds:(gpuJobEnd-gpuJobStart)/1000,
      requestToJobSeconds:(gpuJobStart-requested)/1000,
      framesPerSecond:frameCount/completed.totalSeconds,steadyFrameSeconds:summarize(rows.slice(1).map(r=>r.frameSeconds)),
      resources:utilization.summary,settings:initialized.settings,settingsUnchanged:true,browserErrors:0,
      uniqueFrameHashes:new Set(hashes).size,visibleDifferencePixels:visibleDifference,foregroundPixels:foreground,
      pixelHashes:hashes,timings:rows};
    fs.writeFileSync(path.join(directory,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(`Run ${index+1} complete: first frame ${report.firstSavedFrameSeconds.toFixed(3)}s; ${frameCount} frames ${report.renderAnimationSeconds.toFixed(3)}s; ${report.framesPerSecond.toFixed(3)} fps`);
    return report;
  } catch(error) {
    await page.screenshot({path:path.join(directory,'failure.png')}).catch(()=>{});
    throw error;
  } finally {
    if (resources) await resources.stop().catch(()=>{});
    await context.close(); await browser.close(); fs.closeSync(log);
  }
}
async function main() {
  fs.mkdirSync(output,{recursive:true});
  const runs=[];
  for (let i=0;i<runCount;i++) {
    runs.push(await run(i));
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify({measuredAt:new Date().toISOString(),cpu:os.cpus()[0].model,
      logicalCpus:os.cpus().length,method:'Sequential fresh browser processes, empty IndexedDB, native Render Animation. Default quality, PNG output to Wasm memory filesystem. First frame and frame intervals end after PNG writing. No screen recording or CPU stack tracing. File export and pixel validation run after timing.',
      framesPerRun:frameCount,runCount:runs.length,firstFrameSeconds:summarize(runs.map(r=>r.firstSavedFrameSeconds)),
      totalSeconds:summarize(runs.map(r=>r.renderAnimationSeconds)),runs},null,2)+'\n');
  }
}
main().catch(error=>{console.error(error);process.exitCode=1;});
