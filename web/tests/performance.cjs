/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {spawn,execFileSync}=require('node:child_process');
const {chromium}=require('playwright');
const {ready,consoleCommand,messages}=require('./native.cjs');
const output=path.resolve('build/native/performance-browser');
function browserPid() {
  return fs.readFileSync(`/proc/self/task/${process.pid}/children`,'utf8').trim().split(/\s+/)
    .map(Number).find(pid=>{
      try{return fs.readFileSync(`/proc/${pid}/cmdline`,'utf8').includes('--remote-debugging-pipe');}
      catch{return false;}
    });
}
function memory(root) {
  return JSON.parse(execFileSync('python3',['-c',
    'import sys,json;sys.path.insert(0,"web/tools");from process_memory import sample;print(json.dumps(sample(int(sys.argv[1]))))',String(root)],{encoding:'utf8'}));
}
async function main() {
  fs.mkdirSync(output,{recursive:true});messages.length=0;
  fs.writeFileSync(path.join(output,'browser.log'),'');
  const browser=await chromium.launch({headless:true,args:[
    '--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan',
    '--use-vulkan=native','--disable-vulkan-surface',
  ]});
  const root=browserPid();assert(root,'Find only this test browser process');
  const sampler=spawn('python3',['web/tools/process_memory.py','--root',String(root),
    '--output',path.join(output,'memory.jsonl')],{stdio:'inherit'});
  const stages=[],errors=[];
  let rejectFailure;
  const failure=new Promise((_,reject)=>{rejectFailure=reject;});failure.catch(()=>{});
  let report;
  try {
    const context=await browser.newContext({viewport:{width:1280,height:800}});
    const page=await context.newPage();
    page.on('console',message=>{
      const text=message.text();messages.push(text);
      fs.appendFileSync(path.join(output,'browser.log'),text+'\n');
      if(text.startsWith('PERF_MEMORY:')) stages.push({...JSON.parse(text.slice(12)),...memory(root)});
      if(/Aborted\(|PERF_FAILED:|Blender startup failed|Blender WebGPU error|Blender WebGPU device lost/.test(text)) {errors.push(text);rejectFailure(Error(text));}
    });
    page.on('pageerror',error=>{errors.push(error.stack);rejectFailure(error);});
    page.on('crash',()=>rejectFailure(Error('Benchmark renderer crashed')));
    await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');
    await ready(page,failure);
    const manifest=await page.evaluate(async()=>(await(await fetch('manifest.json')).json()));
    await page.evaluate(source=>Module.FS.writeFile('/tmp/performance.py',source),fs.readFileSync(path.join(__dirname,'performance.py'),'utf8'));
    await page.mouse.move(300,250);
    await Promise.race([failure,consoleCommand(page,"exec(compile(open('/tmp/performance.py').read(),'/tmp/performance.py','exec'))",'PERF_DONE:',undefined,600000)]);
    assert.deepEqual(errors,[]);
    const complete=messages.findLast(text=>text.startsWith('PERF_COMPLETE:'));
    assert(complete,'All scene-size workloads must complete');
    report=JSON.parse(complete.slice('PERF_COMPLETE:'.length));
    report.wasmHeapCapacityBytes=await page.evaluate(()=>typeof wasmMemory!=='undefined'?wasmMemory.buffer.byteLength:null);
    report.wasmMaximumBytes=4294967296;
    assert(report.wasmHeapCapacityBytes>0 && report.wasmHeapCapacityBytes<=report.wasmMaximumBytes,'Read the actual Wasm memory capacity');
    report.memoryStages=stages;
    report.bundleVersion=manifest.version;
    report.browserVersion=browser.version();
    report.adapter=await page.evaluate(async()=>{const a=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});return {vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description};});
    assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(report.adapter)));
    report.errors=errors;
    report.memoryMetric='Linux proportional set size; Chromium process tree; 200ms sampling';
    await context.close();
  } finally {
    if(sampler.exitCode===null && sampler.signalCode===null) {
      const done=new Promise(resolve=>sampler.once('exit',resolve));sampler.kill('SIGTERM');await done;
    }
    await browser.close();
  }
  const points=fs.readFileSync(path.join(output,'memory.jsonl'),'utf8').trim().split('\n').map(JSON.parse);
  report.peakPssMiB=Math.max(...points.map(point=>point.pssKiB))/1024;
  fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
  console.log(JSON.stringify(report));
}
main().catch(error=>{console.error(error);process.exitCode=1;});
