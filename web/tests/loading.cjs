/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const {chromium} = require('playwright');
const url = process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/';
const output = path.resolve(__dirname, process.env.WEB_TEST_UI_FALLBACK === '1' ?
  '../../build/native/loading-evidence-ui' : '../../build/native/loading-evidence');
async function main() {
  fs.mkdirSync(output, {recursive:true});
  const profile=fs.mkdtempSync(path.join(os.tmpdir(),'blender-web-cache-'));
  let context,page,networkAssets=0;
  const errors=[];
  async function startBrowser() {
    context = await chromium.launchPersistentContext(profile,{headless:true,
      viewport:{width:1280,height:800},args:[
        '--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--use-angle=swiftshader',
        '--enable-features=Vulkan','--use-vulkan=swiftshader',
      ]});
    if (process.env.WEB_TEST_UI_FALLBACK === '1') await context.addInitScript(() => {
      delete WebAssembly.promising; delete WebAssembly.Suspending;
    });
    await context.addInitScript(() => {
      window.downloads = [];
      navigator.serviceWorker.addEventListener('message', event => {
        if (event.data?.type === 'blender-download') window.downloads.push(event.data);
      });
    });
    context.on('request', request => {
      if (request.serviceWorker() && /\/assets\//.test(request.url())) networkAssets++;
    });
    page = context.pages()[0] || await context.newPage();
    page.on('pageerror', error => errors.push(error.message));
    page.on('console', message => {
      if (/Blender startup failed|Aborted\(|Blender WebGPU error/.test(message.text())) errors.push(message.text());
    });
  }
  try {
    await startBrowser();
    const runs = [];
    for (const mode of ['cold','warm','offline','restarted-offline']) {
      if (mode==='restarted-offline') {
        await context.close();
        await startBrowser();
      }
      const before = networkAssets;
      if (mode.endsWith('offline')) await context.setOffline(true);
      await page.goto(url);
      if (mode === 'cold') {
        await page.waitForFunction(() => document.querySelector('progress')?.value > 0 &&
          document.querySelector('progress')?.value < 1, null, {timeout:60000});
        await page.screenshot({path:path.join(output,'loading.png')});
      }
      await page.waitForFunction(() => document.documentElement.dataset.blenderReady === 'true', null, {timeout:300000});
      const result = await page.evaluate(async () => {
        const manifest = await (await fetch('manifest.json')).json();
        const variant = document.documentElement.dataset.blenderVariant;
        const wanted = manifest.files.filter(file => file.name.endsWith('/blender.data') ||
          file.name.endsWith(variant === 'ui' ? '/blender-ui.js' : '/blender.js') ||
          file.name.endsWith(variant === 'ui' ? '/blender-ui.wasm' : '/blender.wasm'));
        // Cache writes can finish after the last GPU startup message.
        const cache = await caches.open(`blender-native:${new URL('./',location.href).href}:content-v1`);
        for (const file of wanted) {
          const key = new URL(`__content__/${file.sha256}`,location.href).href;
          const deadline = Date.now()+30000;
          while (!await cache.match(key)) {
            if (Date.now()>deadline) throw Error('Missing cached asset '+file.name);
            await new Promise(resolve=>setTimeout(resolve,50));
          }
        }
        return {version:manifest.version,downloads:window.downloads,
          totalBytes:wanted.reduce((sum,file)=>sum+file.bytes,0),
          children:[...document.body.children].filter(el=>el.tagName!=='SCRIPT').map(el=>el.tagName)};
      });
      assert.deepEqual(result.children,['CANVAS']);
      const completed = new Map(result.downloads.map(event=>[event.url,event]));
      assert.equal(completed.size,3);
      assert.equal([...completed.values()].reduce((sum,event)=>sum+event.loaded,0),result.totalBytes);
      if (mode !== 'cold') {
        assert.equal(networkAssets-before,0,'Warm/offline startup must issue no asset network requests');
        assert([...completed.values()].every(event=>event.cached),'Every runtime file must come from cache');
      } else {
        assert.equal(networkAssets-before,3);
        assert(result.downloads.some(event=>event.loaded>0 && event.loaded<100000000 && !event.cached));
      }
      runs.push({mode,networkRequests:networkAssets-before,totalBytes:result.totalBytes,
        cacheBytes:[...completed.values()].filter(event=>event.cached).reduce((sum,event)=>sum+event.loaded,0),
        progressUpdates:result.downloads.length,canvasOnly:true,bundleVersion:result.version});
    }
    assert.deepEqual(errors,[]);
    await page.screenshot({path:path.join(output,'ready.png')});
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify({runs,browserErrors:errors},null,2)+'\n');
    console.log(JSON.stringify(runs));
  } finally {
    await context?.close();
    fs.rmSync(profile,{recursive:true,force:true});
  }
}
main().catch(error=>{console.error(error);process.exitCode=1;});
