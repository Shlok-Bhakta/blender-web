/* SPDX-License-Identifier: GPL-2.0-or-later */
// Browser acceptance checks against a static server without isolation headers.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const { PNG } = require('pngjs');

const base = process.env.WEB_TEST_URL || 'http://127.0.0.1:8080/';
const evidence = path.resolve(__dirname, '../../build/web/evidence');
const browserArgs = ['--enable-unsafe-webgpu', '--enable-unsafe-swiftshader',
  '--use-angle=swiftshader', '--enable-features=Vulkan', '--use-vulkan=swiftshader'];

async function ready(page, url) {
  await page.goto(url);
  await page.waitForFunction(() => window.blenderWeb?.host.diagnostics.frames > 0, null, { timeout: 45000 });
  assert.equal(await page.locator('#loading').isHidden(), true, await page.locator('#loading').textContent());
}

async function main() {
  fs.mkdirSync(evidence, { recursive: true });
  const browser = await chromium.launch({ headless: true, args: browserArgs });
  try {
    const context = await browser.newContext({ viewport: { width: 1280, height: 800 }, deviceScaleFactor: 1 });
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    page.on('console', message => { if (message.type() === 'error') errors.push(message.text()); });
    const url = new URL('?threads=2', base).href;
    await ready(page, url);
    const startup = await page.evaluate(() => ({
      isolated: crossOriginIsolated, variant: blenderWeb.variant,
      workers: blenderWeb.workerCount, storage: blenderWeb.storageAvailable,
      diagnostics: blenderWeb.host.diagnostics,
    }));
    assert.equal(startup.isolated, true, 'Service worker must enable isolation on a Pages-style host');
    assert.equal(startup.variant, 'threaded');
    assert.equal(startup.workers, 2);
    assert.equal(startup.storage, true);
    assert.equal(startup.diagnostics.backend, 'WEB');

    const screenshot = await page.locator('canvas').screenshot();
    fs.writeFileSync(path.join(evidence, 'cube.png'), screenshot);
    const png = PNG.sync.read(screenshot);
    const color = (x, y) => Array.from(png.data.subarray((y * png.width + x) * 4, (y * png.width + x) * 4 + 3));
    assert.notDeepEqual(color(Math.floor(png.width / 2), Math.floor(png.height / 2)), color(10, 10), 'Cube pixels must differ from the clear color');
    const framesBeforeIdle = await page.evaluate(() => blenderWeb.host.diagnostics.frames);
    await page.waitForTimeout(150);
    assert.equal(await page.evaluate(() => blenderWeb.host.diagnostics.frames), framesBeforeIdle, 'Idle viewport must not redraw continuously');

    const before = await page.evaluate(() => blenderWeb.host.diagnostics.resizes);
    await page.setViewportSize({ width: 820, height: 620 });
    await page.waitForFunction(previous => blenderWeb.host.diagnostics.resizes > previous, before);
    const dimensions = await page.locator('canvas').evaluate(canvas => ({ width: canvas.width, height: canvas.height, cssWidth: canvas.clientWidth, cssHeight: canvas.clientHeight }));
    assert.equal(dimensions.width, dimensions.cssWidth);
    assert.equal(dimensions.height, dimensions.cssHeight);

    const canvas = page.locator('canvas');
    const bounds = await canvas.boundingBox();
    const startYaw = await page.evaluate(() => blenderWeb.module._web_yaw());
    await page.mouse.move(bounds.x + 250, bounds.y + 200);
    await page.mouse.down();
    await page.mouse.move(bounds.x + 330, bounds.y + 240, { steps: 4 });
    await page.mouse.up();
    await page.waitForFunction(previous => blenderWeb.module._web_yaw() !== previous, startYaw);
    const savedYaw = await page.evaluate(() => blenderWeb.module._web_yaw());
    await page.getByRole('button', { name: 'Save view', exact: true }).click();
    await page.waitForFunction(() => document.querySelector('#status').textContent.includes('View saved'));

    await page.getByRole('button', { name: 'Check CPU workers', exact: true }).click();
    await page.waitForFunction(() => blenderWeb.host.job?.done);
    const job = await page.evaluate(() => blenderWeb.host.job);
    // Independently calculate the workload in JavaScript, rather than reuse Wasm's result.
    let expected = 0;
    for (let i = 0; i < 1048576; ++i) expected += (i * i) % 1009;
    assert.equal(job.checksum, expected);
    assert.equal(job.workers, 2);

    // Start a new module to prove that saved data survives Wasm memory destruction.
    await ready(page, url);
    await page.getByRole('button', { name: 'Restore view', exact: true }).click();
    await page.waitForFunction(() => document.querySelector('#status').textContent.includes('View restored'));
    assert.ok(Math.abs(await page.evaluate(() => blenderWeb.module._web_yaw()) - savedYaw) < 0.00001);

    await context.setOffline(true);
    await ready(page, page.url());
    assert.equal(await page.evaluate(() => blenderWeb.variant), 'threaded');
    await page.getByRole('button', { name: 'Check CPU workers', exact: true }).click();
    await page.waitForFunction(() => blenderWeb.host.job?.done);
    assert.equal(await page.evaluate(() => blenderWeb.host.job.checksum), expected);
    await context.setOffline(false);
    assert.deepEqual(errors, []);
    await context.close();

    // A fresh context with no service worker and no COOP/COEP must still boot.
    const fallback = await browser.newContext({ serviceWorkers: 'block', viewport: { width: 390, height: 844 }, deviceScaleFactor: 3, hasTouch: true, isMobile: true });
    const fallbackPage = await fallback.newPage();
    await ready(fallbackPage, url);
    assert.equal(await fallbackPage.evaluate(() => crossOriginIsolated), false);
    assert.equal(await fallbackPage.evaluate(() => blenderWeb.variant), 'single');
    const pixelScale = await fallbackPage.evaluate(() => blenderWeb.host.pixelRatio);
    assert.equal(pixelScale, 2, 'Retina raster resolution should be bounded');
    await fallbackPage.getByRole('button', { name: 'Check CPU workers', exact: true }).click();
    await fallbackPage.waitForFunction(() => blenderWeb.host.job?.done);
    assert.equal(await fallbackPage.evaluate(() => blenderWeb.host.job.checksum), expected);
    assert.equal(await fallbackPage.locator('#loading').isHidden(), true);
    await fallbackPage.screenshot({ path: path.join(evidence, 'mobile-chromium.png') });
    await fallback.close();
    fs.writeFileSync(path.join(evidence, 'results.json'), JSON.stringify({ startup, dimensions, job, fallback: 'single', pixelScale, browser: browser.version() }, null, 2));
    console.log('PASS: WebGPU pixels, GHOST resize/input, idle redraw, CPU workers, IndexedDB persistence, offline reload, single-thread fallback and mobile dimensions.');
  } finally {
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
