/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const { PNG } = require('pngjs');
const url = process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/';
const uiFallback = process.env.WEB_TEST_UI_FALLBACK === '1';
const output = path.resolve(__dirname, uiFallback ? '../../build/native/evidence-ui' : '../../build/native/evidence');
const messages = [];

async function ready(page, failure) {
  await Promise.race([failure, page.waitForFunction(() => document.documentElement.dataset.blenderReady === 'true', null, {timeout:300000})]);
  assert.equal(await page.evaluate(() => crossOriginIsolated), true);
}
async function consoleCommand(page, command, marker) {
  await page.locator('canvas').focus();
  await page.keyboard.press('Shift+F4');
  // Blender initializes the console language when its first region is drawn.
  // Wait for the original green Python banner before sending an Enter event.
  const bannerDeadline = Date.now() + 90000;
  for (;;) {
    const png = PNG.sync.read(await page.screenshot());
    let bannerPixels = 0;
    for (let y = Math.floor(png.height / 2); y < png.height - 80; y++) {
      for (let x = 10; x < Math.min(700, png.width - 200); x++) {
        const i = (y * png.width + x) * 4;
        if (png.data[i+1] > 180 && png.data[i+1] > png.data[i] * 1.2 && png.data[i+2] < 40) bannerPixels++;
      }
    }
    // A reused console may have scrolled its banner out of view. Its dark
    // body is drawn only after console_main_region_draw initializes language.
    let dark = 0, area = 0;
    for (let y=100; y<Math.min(300,png.height-100); y++) {
      for (let x=100; x<Math.min(600,png.width-200); x++) {
        const i=(y*png.width+x)*4; area++;
        if (png.data[i]>=20&&png.data[i]<=35&&png.data[i]===png.data[i+1]&&png.data[i]===png.data[i+2]) dark++;
      }
    }
    if (bannerPixels > 200 || dark > area*.95) break;
    if (Date.now() > bannerDeadline) throw new Error('Original Python console banner did not draw');
    await page.waitForTimeout(250);
  }
  const previous = messages.length;
  await page.keyboard.type(command, {delay:5});
  await page.keyboard.press('Enter');
  const until = Date.now() + 90000;
  while (!messages.slice(previous).some(text => text.includes(marker))) {
    if (Date.now() > until) throw new Error(`Native console did not report ${marker}`);
    await page.waitForTimeout(100);
  }
  await page.keyboard.press('Shift+F5');
  await page.waitForTimeout(1000);
}
async function main() {
  fs.mkdirSync(output, {recursive:true});
  fs.writeFileSync(path.join(output,'browser.log'),'');
  fs.writeFileSync(path.join(output,'errors.log'),'');
  const browser = await chromium.launch({headless:true, args:[
    '--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-angle=swiftshader',
    '--enable-features=Vulkan', '--use-vulkan=swiftshader',
  ]});
  const errors = [];
  let page;
  try {
    const context = await browser.newContext({viewport:{width:1280,height:800},deviceScaleFactor:1});
    if (uiFallback) await context.addInitScript(() => {
      delete WebAssembly.promising;
      delete WebAssembly.Suspending;
    });
    page = await context.newPage();
    let rejectFailure;
    const failure = new Promise((_, reject) => { rejectFailure = reject; });
    failure.catch(() => {});
    page.on('console', message => {
      const text = message.text(); messages.push(text);
      if (text.startsWith('Aborted(') || text.includes('Blender startup failed') || text.startsWith('Blender WebGPU error') || text.startsWith('Blender WebGPU device lost')) rejectFailure(new Error(text));
      fs.appendFileSync(path.join(output,'browser.log'), text+'\n');
    });
    page.on('pageerror', error => { rejectFailure(error); errors.push(error.message); fs.appendFileSync(path.join(output,'errors.log'),error.stack+'\n'); });
    await page.goto(url);
    await ready(page, failure);
    assert.equal(await page.evaluate(() => document.documentElement.dataset.blenderVariant), uiFallback ? 'ui' : 'render');
    const image = await page.screenshot({path:path.join(output,'original-ui.png')});
    const png = PNG.sync.read(image);
    const colors = new Set();
    for (let i=0;i<png.data.length;i+=16) colors.add(png.data.readUInt32LE(i));
    let cubeOutlinePixels = 0;
    for (let y = Math.floor(png.height * .2); y < png.height * .75; y++) {
      for (let x = Math.floor(png.width * .3); x < png.width * .65; x++) {
        const i = (y * png.width + x) * 4;
        if (png.data[i] > 210 && png.data[i+1] > 100 && png.data[i+1] < 200 && png.data[i+2] < 100) cubeOutlinePixels++;
      }
    }
    assert(cubeOutlinePixels > 80, 'Expected the selected startup cube in the viewport');
    assert(colors.size > 100, `Expected rendered original UI, got ${colors.size} colors`);
    assert(messages.some(text => text.includes('real device acquired')), 'Native backend must acquire a real WebGPU device');
    assert(page.workers().length >= 2, 'Application must start browser workers');

    await page.setViewportSize({width:1040,height:720});
    await page.waitForTimeout(2000);
    await page.screenshot({path:path.join(output,'original-ui-resized.png')});
    await page.mouse.move(300,250);
    await consoleCommand(page,
      "import pathlib, os; pathlib.Path('/user/blender-web/home/browser-native-proof.txt').write_text('persisted'); os.write(1, b'NATIVE_STORAGE_WRITTEN\\n')",
      'NATIVE_STORAGE_WRITTEN');
    await page.screenshot({path:path.join(output,'original-ui-after-console.png')});

    await context.setOffline(true);
    await page.reload();
    await ready(page, failure);
    await page.mouse.move(300,250);
    await consoleCommand(page,
      "import pathlib, os; os.write(1, ('NATIVE_STORAGE_READ:' + pathlib.Path('/user/blender-web/home/browser-native-proof.txt').read_text() + '\\n').encode())",
      'NATIVE_STORAGE_READ:persisted');
    await page.screenshot({path:path.join(output,'original-ui-offline.png')});
    assert.deepEqual(errors, [], 'Browser runtime errors');
    const report = {variant:uiFallback ? 'ui' : 'render', originalUI:true, startupCube:true, webgpu:true, isolated:true, workers:page.workers().length,
      colors:colors.size, resize:true, nativePythonConsole:true, persistentStorage:true, offlineReload:true};
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report));
    await context.close();
  } catch (error) {
    if (page) await page.screenshot({path:path.join(output,'failure.png')}).catch(() => {});
    throw error;
  } finally {
    await browser.close();
  }
}
module.exports = {ready, consoleCommand, messages};
if (require.main === module) main().catch(error => { console.error(error); process.exitCode=1; });
