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
  await page.waitForFunction(() => window.blenderWeb?.editor && window.blenderWeb.host.diagnostics.frames > 0, null, { timeout: 45000 });
  assert.equal(await page.locator('#loading').isHidden(), true, await page.locator('#loading').textContent());
}

async function menuAction(page, menu, id) {
  await page.locator('nav summary').filter({ hasText: new RegExp(`^${menu}$`) }).click();
  await page.locator(`#${id}`).click();
}

async function transform(page, name, value) {
  const input = page.getByRole('spinbutton', { name, exact: true });
  await input.fill(String(value)); await input.press('Tab');
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
    await page.screenshot({ path: path.join(evidence, 'editor-desktop.png') });
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
    await transform(page, 'Location X', 1.5);
    await transform(page, 'Rotation Z', 30);
    await transform(page, 'Scale Z', 1.25);
    assert.equal(await page.evaluate(() => blenderWeb.module._web_transform(0)), 1.5);
    assert.equal(await page.evaluate(() => blenderWeb.module._web_transform(5)), 30);
    assert.equal(await page.evaluate(() => blenderWeb.module._web_transform(8)), 1.25);
    await menuAction(page, 'Edit', 'undo');
    assert.equal(await page.evaluate(() => blenderWeb.module._web_transform(8)), 1);
    await menuAction(page, 'Edit', 'redo');
    assert.equal(await page.evaluate(() => blenderWeb.module._web_transform(8)), 1.25);
    await transform(page, 'Scale X', -1);
    assert.equal(await page.evaluate(() => blenderWeb.module._web_transform(6)), -1);
    await page.locator('#select-object').click();
    assert.equal(await page.getByRole('spinbutton', { name: 'Location X', exact: true }).isDisabled(), true);
    await page.locator('#select-object').click();
    assert.equal(await page.getByRole('spinbutton', { name: 'Location X', exact: true }).isDisabled(), false);
    await page.getByRole('textbox', { name: 'Object name', exact: true }).fill('Study Cube');
    await page.getByRole('textbox', { name: 'Object name', exact: true }).press('Tab');
    assert.equal(await page.locator('#outliner-name').textContent(), 'Study Cube');
    await page.getByRole('button', { name: 'Grid', exact: true }).click();
    const shownPixels = await canvas.screenshot();
    await page.getByRole('checkbox', { name: 'Object visible', exact: true }).uncheck();
    await page.waitForFunction(() => !blenderWeb.editor.capture().object.visible);
    const hiddenPixels = await canvas.screenshot();
    assert.notEqual(Buffer.compare(shownPixels, hiddenPixels), 0, 'Visibility must change rendered pixels');
    await page.getByRole('checkbox', { name: 'Object visible', exact: true }).check();
    await page.getByRole('button', { name: 'Top', exact: true }).click();
    assert.equal(await page.locator('#view-label').textContent(), 'Top orthographic');
    assert.ok(Math.abs(await page.evaluate(() => blenderWeb.module._web_pitch()) - Math.PI / 2) < 0.0001);
    await page.getByRole('button', { name: 'Reset view', exact: true }).click();
    await page.getByRole('button', { name: 'Frame object', exact: true }).click();
    assert.equal(await page.evaluate(() => blenderWeb.module._web_target(0)), 1.5);
    const resizesBeforePanel = await page.evaluate(() => blenderWeb.host.diagnostics.resizes);
    await page.getByRole('separator', { name: 'Resize inspector', exact: true }).press('ArrowLeft');
    await page.waitForFunction(previous => blenderWeb.host.diagnostics.resizes > previous, resizesBeforePanel);
    assert.equal(await page.getByRole('separator', { name: 'Resize inspector', exact: true }).getAttribute('aria-valuenow'), '296');
    const savedCameraYaw = await page.evaluate(() => blenderWeb.module._web_yaw());
    await menuAction(page, 'File', 'save');
    await page.waitForFunction(() => document.querySelector('#status').textContent.includes('Preview saved'));

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
    await menuAction(page, 'File', 'restore');
    await page.waitForFunction(() => document.querySelector('#status').textContent.includes('Preview restored'));
    assert.ok(Math.abs(await page.evaluate(() => blenderWeb.module._web_yaw()) - savedCameraYaw) < 0.00001);
    assert.equal(await page.evaluate(() => blenderWeb.module._web_transform(0)), 1.5);
    assert.equal(await page.evaluate(() => blenderWeb.module._web_transform(8)), 1.25);
    assert.equal(await page.locator('#outliner-name').textContent(), 'Study Cube');
    const downloadPromise = page.waitForEvent('download');
    await menuAction(page, 'File', 'download');
    const download = await downloadPromise;
    const downloaded = JSON.parse(fs.readFileSync(await download.path(), 'utf8'));
    assert.equal(downloaded.format, 'blender-web-preview');
    assert.equal(downloaded.object.transform[0], 1.5);
    assert.equal(downloaded.object.transform[6], -1);
    const invalid = structuredClone(downloaded);
    invalid.object.transform[0] = null;
    await page.locator('#project-file').setInputFiles({ name: 'invalid.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify(invalid)) });
    await page.waitForFunction(() => document.querySelector('#status').textContent.includes('Cannot open preview'));
    assert.equal(await page.evaluate(() => blenderWeb.module._web_transform(0)), 1.5, 'Invalid imports must leave the scene untouched');
    const imported = structuredClone(downloaded);
    imported.object.transform[0] = -2.5;
    imported.object.name = 'Imported cube';
    await page.locator('#project-file').setInputFiles({ name: 'imported.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify(imported)) });
    await page.waitForFunction(() => blenderWeb.module._web_transform(0) === -2.5);
    assert.equal(await page.locator('#outliner-name').textContent(), 'Imported cube');
    await page.getByRole('button', { name: 'Frame object', exact: true }).click();
    const divider = page.getByRole('separator', { name: 'Resize inspector', exact: true });
    const dividerBounds = await divider.boundingBox();
    await page.mouse.move(dividerBounds.x + 3, dividerBounds.y + 60);
    await page.mouse.down(); await page.mouse.move(dividerBounds.x - 120, dividerBounds.y + 60, { steps: 4 }); await page.mouse.up();
    await page.setViewportSize({ width: 710, height: 620 });
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false, 'Inspector must fit when the window shrinks');
    await page.setViewportSize({ width: 820, height: 620 });
    await page.getByRole('button', { name: 'Frame object', exact: true }).click();
    await page.screenshot({ path: path.join(evidence, 'editor-edited.png') });

    await context.setOffline(true);
    await ready(page, page.url());
    assert.equal(await page.evaluate(() => blenderWeb.variant), 'threaded');
    await page.getByRole('button', { name: 'Check CPU workers', exact: true }).click();
    await page.waitForFunction(() => blenderWeb.host.job?.done);
    assert.equal(await page.evaluate(() => blenderWeb.host.job.checksum), expected);
    await menuAction(page, 'File', 'restore');
    await page.waitForFunction(() => blenderWeb.module._web_transform(0) === 1.5);
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
    assert.equal(await fallbackPage.getByRole('button', { name: 'Inspector', exact: true }).getAttribute('aria-expanded'), 'false');
    const mobileDistance = await fallbackPage.evaluate(() => blenderWeb.module._web_distance());
    await fallbackPage.getByRole('button', { name: 'Zoom in', exact: true }).tap();
    assert.ok(await fallbackPage.evaluate(() => blenderWeb.module._web_distance()) < mobileDistance);
    await fallbackPage.getByRole('button', { name: 'Inspector', exact: true }).tap();
    await transform(fallbackPage, 'Location Y', -1.25);
    assert.equal(await fallbackPage.evaluate(() => blenderWeb.module._web_transform(1)), -1.25);
    await fallbackPage.getByRole('button', { name: 'Frame object', exact: true }).tap();
    assert.equal(await fallbackPage.evaluate(() => document.documentElement.scrollWidth > innerWidth), false, 'Mobile UI must fit the viewport');
    await fallbackPage.screenshot({ path: path.join(evidence, 'mobile-chromium.png') });
    await fallback.close();
    fs.writeFileSync(path.join(evidence, 'results.json'), JSON.stringify({ startup, dimensions, job, fallback: 'single', pixelScale, browser: browser.version() }, null, 2));
    console.log('PASS: editor transforms/undo/visibility/views/panel resizing/import/export, persisted preview, WebGPU pixels, GHOST input, CPU workers, offline UI and mobile fallback.');
  } finally {
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
