/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const http=require('node:http');
const zlib=require('node:zlib');
const crypto=require('node:crypto');
const {spawnSync}=require('node:child_process');
const {chromium}=require('playwright');
const {ready,consoleCommand,messages}=require('./native.cjs');
const hardware=process.env.WEB_TEST_HARDWARE==='1',fallback=process.env.WEB_TEST_UI_FALLBACK==='1';
const output=path.resolve(process.env.WEB_PYTHON_OUTPUT||`build/native/python-foundation-evidence${hardware?'-hardware':''}${fallback?'-ui':''}`);
const directory='/user/blender-web/home/python-foundation';
const hash=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
async function main(){
  fs.mkdirSync(output,{recursive:true});messages.length=0;fs.writeFileSync(path.join(output,'browser.log'),'');
  const payload=Buffer.from(Array.from({length:262147},(_,i)=>(i*17+Math.floor(i/251))%256));
  let activeBackgroundDownloads=0,peakBackgroundDownloads=0;
  const server=http.createServer((request,response)=>{
    const route=new URL(request.url,'http://fixture').pathname;
    if(route!=='/cors-denied')response.setHeader('Access-Control-Allow-Origin','*');
    response.setHeader('Access-Control-Allow-Headers','Content-Type,X-Blender-Proof,If-None-Match,If-Modified-Since');
    response.setHeader('Access-Control-Expose-Headers','ETag,Content-Encoding,Content-Length');
    response.setHeader('Cache-Control','no-store');
    response.setHeader('Access-Control-Allow-Methods','GET,POST,OPTIONS');
    if(request.method==='OPTIONS'){response.writeHead(204);return response.end();}
    if(route==='/data'){
      response.setHeader('ETag','"blender-fixture-v1"');
      if(request.headers['if-none-match']==='"blender-fixture-v1"'){response.writeHead(304);return response.end();}
      response.setHeader('Content-Type','application/octet-stream');response.setHeader('Content-Encoding','gzip');
      const compressed=zlib.gzipSync(payload);
      if(new URL(request.url,'http://fixture').searchParams.has('resource')){
        activeBackgroundDownloads++;peakBackgroundDownloads=Math.max(peakBackgroundDownloads,activeBackgroundDownloads);
        setTimeout(()=>{activeBackgroundDownloads--;response.end(compressed);},150);return;
      }
      return response.end(compressed);
    }
    if(route==='/echo'){
      const chunks=[];request.on('data',data=>chunks.push(data));request.on('end',()=>{response.setHeader('Content-Type','application/json');response.end(JSON.stringify({body:Buffer.concat(chunks).toString('base64'),proof:request.headers['x-blender-proof'],userAgent:request.headers['user-agent']}));});return;
    }
    if(route==='/redirect'){response.writeHead(302,{Location:'/data'});return response.end();}
    if(route==='/slow'){setTimeout(()=>response.end('late'),1200);return;}
    response.writeHead(route==='/missing'?404:200);response.end('fixture');
  });
  await new Promise(resolve=>server.listen(Number(process.env.WEB_PYTHON_FIXTURE_PORT||0),'127.0.0.1',resolve));
  server.unref();
  // A public HTTPS page needs a public HTTPS fixture endpoint. Keep CORS and
  // local-network protections enabled when checking the deployed Pages build.
  const networkURL=process.env.WEB_PYTHON_NETWORK_URL||'http://127.0.0.1:'+server.address().port;
  const browser=await chromium.launch({headless:true,args:hardware?[
    '--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--use-vulkan=native','--disable-vulkan-surface',
  ]:[
    '--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader','--enable-features=Vulkan','--use-vulkan=swiftshader',
  ]});
  const context=await browser.newContext({viewport:{width:1280,height:800},recordVideo:{dir:output,size:{width:1280,height:800}}});
  if(fallback)await context.addInitScript(()=>{delete WebAssembly.promising;delete WebAssembly.Suspending;});
  const page=await context.newPage(),errors=[],startupPythonErrors=[],chapters=[],started=Date.now();
  let checking=false;
  let rejectFailure;const failure=new Promise((_,reject)=>rejectFailure=reject);failure.catch(()=>{});
  const fail=text=>{errors.push(text);rejectFailure(Error(text));};
  page.on('console',message=>{const text=message.text();messages.push(text);fs.appendFileSync(path.join(output,'browser.log'),text+'\n');
    if(/Traceback \(most recent call last\)/.test(text)){
      if(checking)fail(text);else startupPythonErrors.push(text);
    }
    if(/PYTHON_FOUNDATION_FAILED:|Aborted\(|Blender startup failed|Blender UI frame failed|Blender GPU job failed|Blender WebGPU (error|device lost)/.test(text))fail(text);
  });
  page.on('pageerror',error=>fail(error.stack||error.message));
  const command=(code,marker)=>Promise.race([failure,consoleCommand(page,code,marker,()=>page.screenshot(),300000)]);
  const chapter=title=>chapters.push({seconds:(Date.now()-started)/1000,title});
  try{
    chapter('Start original Blender with the embedded Python dependencies');
    await page.goto(process.env.WEB_TEST_URL||'http://127.0.0.1:8093/');await ready(page,failure);checking=true;
    const manifest=await page.evaluate(()=>fetch('manifest.json').then(r=>r.json()));
    const adapter=await page.evaluate(async()=>{const a=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});return {vendor:a.info.vendor,architecture:a.info.architecture,device:a.info.device,description:a.info.description};});
    if(hardware)assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
    const cert=path.join(output,'test.cert.pem'),key=path.join(output,'test.key.pem');
    const fixture=spawnSync('openssl',['req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:prime256v1','-nodes','-keyout',key,'-out',cert,'-days','1','-subj','/CN=blender-web.test','-addext','subjectAltName=DNS:blender-web.test']);
    assert.equal(fixture.status,0,fixture.stderr.toString());
    const source=fs.readFileSync(path.join(__dirname,'../native/tests/python-foundation.py'));
    for(const [name,bytes] of [['check.py',source],['test.cert.pem',fs.readFileSync(cert)],['test.key.pem',fs.readFileSync(key)]]){
      await page.evaluate(({name,data,directory})=>{Module.FS.mkdirTree(directory);Module.FS.writeFile(directory+'/'+name,Uint8Array.from(atob(data),c=>c.charCodeAt(0)));},{name,data:bytes.toString('base64'),directory});
    }
    await page.mouse.move(300,250);chapter('Check SQLite, compression, TLS, packages and Cython in Blender');
    await command(`import runpy,json,pathlib,os; pf=runpy.run_path('${directory}/check.py'); pf_report=pf['browser_check']('core','${directory}')`,'PYTHON_FOUNDATION_CORE');
    chapter('Exercise browser HTTP, binary transfers, timeout and CORS rejection');
    await command(`pf_report=pf['browser_check']('network','${directory}',${JSON.stringify(networkURL)})`,'PYTHON_FOUNDATION_NETWORK');
    assert.equal(peakBackgroundDownloads,2,'Both background requests must overlap at the HTTP server');
    const overview='Blender browser Python dependency checks\n\nSQLite: transactions, FTS3/4/5, WAL locks, serialization\nLZMA: XZ, ALONE and raw-filter round trips\nZstandard: two worker threads, streaming API, checksum rejection\nTLS 1.2 / 1.3: handshakes and parallel sessions\nCertificate and hostname rejection: passed\nPBKDF2, scrypt and HMAC known vectors: passed\nCython: original Python compiler generated a C extension\nHTTP: binary GET/POST, gzip, timeout, CORS rejection\n\nInstalled package versions\n';
    await command(`import bpy; bpy.data.texts.new('Browser Python dependency checks').write(json.dumps(pf_report,indent=2)); pf_text=bpy.data.texts.new('Python dependency results'); pf_text.write(${JSON.stringify(overview)}+'\\n'.join(k+' '+v for k,v in pf_report['packages'].items())); pf_text.cursor_set(0); bpy.ops.wm.save_as_mainfile(filepath='${directory}/python-foundation.blend'); os.write(1,b'PYTHON_FOUNDATION_SAVED\\n')`,'PYTHON_FOUNDATION_SAVED');
    const files={};
    for(const name of ['python-foundation.json','python-roundtrip.sqlite','python-roundtrip.xz','python-roundtrip.zst','python-foundation.blend','browser_generated.c','download-direct.bin','download-one.bin','download-two.bin']){
      const bytes=Buffer.from(await page.evaluate(name=>Array.from(Module.FS.readFile(name)),directory+'/'+name));fs.writeFileSync(path.join(output,name),bytes);files[name]={sha256:hash(bytes),bytes:bytes.length};
    }
    const report=JSON.parse(fs.readFileSync(path.join(output,'python-foundation.json')));
    assert(report.tls12&&report.tls13&&report.untrustedRejected&&report.hostnameRejected&&report.cythonCodeGeneration&&report.sqliteWAL&&report.network.corsRejected&&report.network.timeout);
    chapter('Inspect the dependency results in the original Text Editor');
    const display=`def pf_display():\n    area=max(bpy.context.screen.areas,key=lambda a:a.width*a.height)\n    area.type='TEXT_EDITOR'\n    area.spaces.active.text=pf_text\n    area.tag_redraw()\nbpy.app.timers.register(pf_display,first_interval=1.5)`;
    await command(`exec(${JSON.stringify(display)}); os.write(1,b'PYTHON_FOUNDATION_DISPLAY\\n')`,'PYTHON_FOUNDATION_DISPLAY');
    await page.waitForTimeout(4000);await page.screenshot({path:path.join(output,'python-foundation-browser.png')});
    chapter('Reopen saved database and compressed files after an offline reload');
    await context.setOffline(true);checking=false;await page.reload();await ready(page,failure);checking=true;await page.mouse.move(300,250);
    await command(`import runpy; pf=runpy.run_path('${directory}/check.py'); pf['browser_check']('offline','${directory}')`,'PYTHON_FOUNDATION_OFFLINE');
    await page.waitForTimeout(1500);assert.deepEqual(errors,[]);
    const acceptance={bundleVersion:manifest.version,sourceSha256:hash(source),runnerSourceSha256:hash(fs.readFileSync(__filename)),hardware,fallback,adapter,report,files,networkURL,peakBackgroundDownloads,offlineReopen:true,browserErrors:errors,startupPythonErrors,chapters,durationSeconds:(Date.now()-started)/1000,videoPath:path.join(output,'python-foundation.webm')};
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(acceptance,null,2)+'\n');
    assert.deepEqual(startupPythonErrors,[],'Original Blender must start without Python tracebacks');
    console.log(JSON.stringify({bundleVersion:manifest.version,pythonFoundation:true,network:true,offlineReopen:true}));
  }catch(error){await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});throw error;}
  finally{const video=page.video();await context.close();if(video)await video.saveAs(path.join(output,'python-foundation.webm'));await browser.close();server.closeAllConnections();await new Promise(resolve=>server.close(resolve));}
}
if(require.main===module)main().catch(error=>{console.error(error);process.exitCode=1;});
