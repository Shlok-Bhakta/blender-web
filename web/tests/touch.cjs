/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Drive Blender Web with real multi-touch (CDP touch events) and check the
 * gestures from the 3D Modeling Thingy guide: cursor aiming and tap, undo,
 * orbit, pinch, the Shift belt and the text sheet. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require('playwright');
const {ready, consoleCommand, messages} = require('./native.cjs');

const url = process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/';
const software = process.env.WEB_TEST_HARDWARE !== '1';
const output = path.resolve(__dirname, '../../build/native/touch-evidence');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function main() {
  fs.mkdirSync(output, {recursive:true});
  fs.writeFileSync(path.join(output, 'browser.log'), '');
  const browser = await chromium.launch({headless:true, args:software ? [
    '--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-angle=swiftshader',
    '--enable-features=Vulkan', '--use-vulkan=swiftshader',
  ] : [
    '--enable-unsafe-webgpu', '--use-angle=vulkan', '--enable-features=Vulkan', '--use-vulkan=native',
    '--disable-vulkan-surface',
  ]});
  const context = await browser.newContext({viewport:{width:1280, height:800}, hasTouch:true,
    recordVideo:{dir:output, size:{width:1280, height:800}}});
  const page = await context.newPage();
  const errors = [];
  page.on('console', message => {
    const text = message.text();
    messages.push(text);
    if (!text.startsWith('TOUCH_STATE:') && !text.startsWith('WGPU_')) {
      fs.appendFileSync(path.join(output, 'browser.log'), text + '\n');
    }
    if (/Aborted\(|Blender startup failed|Blender WebGPU error/.test(text)) errors.push(text);
  });
  page.on('pageerror', error => errors.push(error.stack));
  const report = {checks:{}};
  try {
    await page.goto(url);
    await ready(page, new Promise(() => {}));
    await page.mouse.move(400, 300);
    await page.evaluate(source => Module.FS.writeFile('/tmp/touch_state.py', source),
      fs.readFileSync(path.join(__dirname, 'touch_state.py'), 'utf8'));
    await consoleCommand(page, "exec(open('/tmp/touch_state.py').read())", 'TOUCH_STATE:');

    const ratio = await page.evaluate(() => devicePixelRatio);
    const state = async () => {
      const since = messages.length;
      const deadline = Date.now() + 10000;
      for (;;) {
        const line = messages.slice(since).find(text => text.startsWith('TOUCH_STATE:'));
        if (line) return JSON.parse(line.slice('TOUCH_STATE:'.length));
        if (Date.now() > deadline) throw new Error('No state report');
        await sleep(50);
      }
    };
    const screen = point => point.map(value => value / ratio);
    const debug = () => page.evaluate(() => Module.webTouchDebug());
    const cdp = await context.newCDPSession(page);
    const send = (type, points) => cdp.send('Input.dispatchTouchEvent', {type,
      touchPoints:points.map(([x, y], id) => ({x, y, id, radiusX:4, radiusY:4, force:1}))});
    const release = () => cdp.send('Input.dispatchTouchEvent', {type:'touchEnd', touchPoints:[]});
    /* Fingers move in parallel; `to` may differ per finger (pinch). */
    const gesture = async (from, to, steps, interval) => {
      await send('touchStart', from);
      for (let i = 1; i <= steps; i++) {
        await sleep(interval);
        await send('touchMove', from.map(([x, y], f) => [x + (to[f][0] - x) * i / steps, y + (to[f][1] - y) * i / steps]));
      }
      await sleep(interval);
      await release();
    };
    const tap = async (fingers = [[640, 450]]) => {
      await send('touchStart', fingers);
      await sleep(40);
      await release();
    };
    /* Slow one-finger drags move the cursor one-to-one; repeat until it is on target. */
    const aim = async ([x, y]) => {
      for (let attempt = 0; attempt < 4; attempt++) {
        const now = await debug();
        const dx = x - now.x, dy = y - now.y;
        if (Math.hypot(dx, dy) <= 1.5) return;
        const distance = Math.hypot(dx, dy), steps = Math.max(4, Math.ceil(distance / 4));
        const start = [[640 - Math.sign(dx) * Math.min(Math.abs(dx), 300), 450 - Math.sign(dy) * Math.min(Math.abs(dy), 250)]];
        const end = [[start[0][0] + dx, start[0][1] + dy]];
        await gesture(start, end, steps, 40);
        await sleep(150);
      }
      const now = await debug();
      assert(Math.hypot(x - now.x, y - now.y) <= 3, JSON.stringify({target:[x, y], now}));
    };
    const settle = () => sleep(600);

    /* Touch mode: the first drag shows the cursor and the belt but never clicks. */
    await gesture([[600, 450]], [[640, 450]], 10, 30);
    await settle();
    let info = await debug();
    assert.equal(info.lastInput, 'touch');
    assert(info.cursorShown && info.beltShown, JSON.stringify(info));
    let scene = await state();
    assert.deepEqual(scene.selected, []);
    report.checks.touchMode = true;

    /* Aim the cursor at the cube, then tap to select it. */
    await aim(screen(scene.Cube));
    await tap();
    await settle();
    scene = await state();
    assert.deepEqual(scene.selected, ['Cube']);
    report.checks.aimAndTap = true;

    /* Two-finger tap is Undo. */
    await tap([[600, 450], [680, 450]]);
    await settle();
    scene = await state();
    assert.deepEqual(scene.selected, []);
    report.checks.twoFingerUndo = true;

    /* Three-finger tap is Redo. */
    await tap([[560, 450], [640, 450], [720, 450]]);
    await settle();
    scene = await state();
    assert.deepEqual(scene.selected, ['Cube']);
    report.checks.threeFingerRedo = true;

    /* The cursor is over the 3D Viewport: two fingers orbit, pinch zooms. */
    const before = await state();
    await gesture([[560, 400], [680, 400]], [[700, 400], [820, 400]], 20, 25);
    await settle();
    let after = await state();
    const turned = Math.acos(Math.min(1, Math.abs(before.rotation.reduce((sum, v, i) => sum + v * after.rotation[i], 0)))) * 2;
    assert(turned > 0.2, `orbit turned ${turned} rad`);
    assert.deepEqual(after.selected, ['Cube']);
    report.checks.twoFingerOrbit = turned;
    await gesture([[600, 400], [680, 400]], [[450, 400], [830, 400]], 20, 25);
    await settle();
    const zoomed = await state();
    assert(zoomed.distance < after.distance * 0.9, `pinch ${after.distance} -> ${zoomed.distance}`);
    report.checks.pinchZoom = [after.distance, zoomed.distance];

    /* A tapped Shift on the belt adds the next click to the selection. */
    const handle = await page.locator('#blender-touch-belt .handle').boundingBox();
    await tap([[handle.x + handle.width / 2, handle.y + handle.height / 2]]);
    await sleep(300);
    assert((await debug()).beltExpanded);
    const shift = await page.locator('#blender-touch-belt .key').first().boundingBox();
    await tap([[shift.x + shift.width / 2, shift.y + shift.height / 2]]);
    await sleep(200);
    assert.equal((await debug()).modifiers & 3, 2, 'Shift latched');
    scene = await state();
    await aim(screen(scene.Light));
    await tap();
    await settle();
    scene = await state();
    assert.deepEqual(scene.selected, ['Cube', 'Light']);
    assert.equal((await debug()).modifiers & 3, 0, 'latch cleared after the click');
    report.checks.shiftBelt = true;

    /* Triple tap on the Outliner's Cube row is a double click: rename through the sheet. */
    await aim([1152, 121]);
    for (let i = 0; i < 3; i++) {
      await tap();
      await sleep(90);
    }
    await page.waitForFunction(() => Module.webTouchDebug()?.sheetOpen, null, {timeout:10000});
    info = await debug();
    assert.equal(info.sheetValue, 'Cube');
    await page.locator('#blender-text-sheet input').fill('TouchCube');
    const done = await page.locator('#blender-text-sheet button[type=submit]').boundingBox();
    await tap([[done.x + done.width / 2, done.y + done.height / 2]]);
    await settle();
    scene = await state();
    assert(scene.names.includes('TouchCube') && !scene.names.includes('Cube'), JSON.stringify(scene.names));
    assert(!(await debug()).sheetOpen);
    report.checks.textSheetRename = true;

    await page.screenshot({path:path.join(output, 'touch.png')});
    assert.deepEqual(errors, []);
    const manifest = await page.evaluate(async () => (await (await fetch('manifest.json')).json()));
    Object.assign(report, {bundleVersion:manifest.version, browserVersion:browser.version(), software, errors});
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify(report));
  }
  catch (error) {
    await page.screenshot({path:path.join(output, 'failure.png')}).catch(() => {});
    fs.writeFileSync(path.join(output, 'partial.json'), JSON.stringify({...report, error:String(error)}, null, 2));
    throw error;
  }
  finally {
    await context.close();
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
