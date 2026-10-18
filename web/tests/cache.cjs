/* SPDX-License-Identifier: GPL-2.0-or-later */
// Exercise deployments without rebuilding or downloading a second Blender.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const {createHash} = require('node:crypto');
const {chromium} = require('playwright');
const template = fs.readFileSync(path.join(__dirname,'../native/site/sw.js'),'utf8');
const versions = ['a'.repeat(20),'b'.repeat(20)];
const shared = Buffer.alloc(65536,42);
const changed = [Buffer.from('first build'),Buffer.from('second build')];
const manifests = versions.map((version,index)=>({version,files:[['shared.wasm',shared],['changed.data',changed[index]]].map(([name,body])=>({
  name:`assets/${version}/${name}`,bytes:body.length,sha256:createHash('sha256').update(body).digest('hex'),
}))}));
async function main() {
  let deployment=0, networkAssets=0;
  const server=http.createServer((req,res)=>{
    res.setHeader('Cache-Control','no-store');
    const pathname=new URL(req.url,'http://localhost').pathname.slice(1);
    if (pathname==='sw.js') {
      res.setHeader('Content-Type','text/javascript');
      res.end(template.replace('__VERSION__',versions[deployment]).replace('__ASSETS__',JSON.stringify(
        Object.fromEntries(manifests[deployment].files.map(file=>[file.name,file])))));
    } else if (pathname==='manifest.json') res.end(JSON.stringify(manifests[deployment]));
    else if (pathname==='boot.js') res.end('');
    else if (pathname===''||pathname==='index.html') {
      res.setHeader('Content-Type','text/html');res.end('<!doctype html><title>Cache deployment fixture</title>');
    } else if (pathname.startsWith('assets/')) {
      networkAssets++;
      const file=manifests[deployment].files.find(file=>file.name===pathname);
      if (!file) {res.writeHead(404);res.end();}
      else res.end(pathname.endsWith('shared.wasm')?shared:changed[deployment]);
    } else {res.writeHead(404);res.end();}
  });
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const url=`http://127.0.0.1:${server.address().port}/`;
  const browser=await chromium.launch({headless:true});
  try {
    const context=await browser.newContext();
    const page=await context.newPage();
    await page.goto(url);
    await page.evaluate(async()=>{
      await navigator.serviceWorker.register('sw.js');await navigator.serviceWorker.ready;
      if (!navigator.serviceWorker.controller) await new Promise(resolve=>navigator.serviceWorker.addEventListener('controllerchange',resolve,{once:true}));
    });
    const read=async file=>page.evaluate(async file=>{
      const response=await fetch(file.name);
      if (!response.ok) throw Error('HTTP '+response.status);
      return [...new Uint8Array(await response.arrayBuffer())];
    },file);
    for (const file of manifests[0].files) await read(file);
    await page.waitForFunction(async()=>{
      const names=await caches.keys();const cache=await caches.open(names.find(name=>name.endsWith('content-v1')));
      return (await cache.keys()).length===2;
    });
    assert.equal(networkAssets,2);
    deployment=1;
    await page.evaluate(async()=>{
      const changed=new Promise(resolve=>navigator.serviceWorker.addEventListener('controllerchange',resolve,{once:true}));
      await (await navigator.serviceWorker.getRegistration()).update();await changed;
    });
    await context.setOffline(true);
    assert.deepEqual(Buffer.from(await read(manifests[1].files[0])),shared,'New deployment must reuse unchanged bytes offline');
    assert.deepEqual(Buffer.from(await read(manifests[0].files[1])),changed[0],'Old tabs must retain their original changed data');
    assert.equal(networkAssets,2);
    await context.setOffline(false);
    assert.deepEqual(Buffer.from(await read(manifests[1].files[1])),changed[1]);
    assert.equal(networkAssets,3,'Only the changed file should download');
    await page.waitForFunction(async()=>{
      const cache=await caches.open((await caches.keys()).find(name=>name.endsWith('content-v1')));
      return (await cache.keys()).length===3;
    });
    await context.setOffline(true);
    assert.deepEqual(Buffer.from(await read(manifests[1].files[1])),changed[1]);
    // Simulate a quota failure in this worker, then request uncached content.
    // A failed write must never turn a successful download into a failed load.
    const worker=context.serviceWorkers()[0];
    await worker.evaluate(()=>{Cache.prototype.put=async()=>{throw new DOMException('Quota full','QuotaExceededError');};});
    await context.setOffline(false);
    await page.evaluate(async file=>{
      const name=(await caches.keys()).find(name=>name.endsWith('content-v1'));
      await (await caches.open(name)).delete(new URL(`__content__/${file.sha256}`,location.href));
    },manifests[1].files[1]);
    assert.deepEqual(Buffer.from(await read(manifests[1].files[1])),changed[1]);
    const output=path.resolve(__dirname,'../../build/native/cache-evidence');fs.mkdirSync(output,{recursive:true});
    const report={crossDeploymentReuse:true,oldTabOffline:true,changedFileDownloads:1,offline:true,quotaFailureLoads:true};
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report));
  } finally {await browser.close();await new Promise(resolve=>server.close(resolve));}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
