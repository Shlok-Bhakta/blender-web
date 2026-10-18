/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {PNG} = require('pngjs');

// Exercise the running native application. A CSS-sized screenshot alone cannot
// prove that GHOST, Blender's editors and the GPU drawing buffers all resized.
async function checkResize(page, messages, consoleCommand, output) {
  const cdp = await page.context().newCDPSession(page);
  // Playwright's screenshot helper reapplies its context's original emulation
  // settings. Capture directly while testing live changes to device density.
  const capture = async()=>{
    const {visualViewport}=await cdp.send('Page.getLayoutMetrics');
    return Buffer.from((await cdp.send('Page.captureScreenshot',{
      format:'png',captureBeyondViewport:false,
      clip:{x:visualViewport.pageX,y:visualViewport.pageY,
        width:visualViewport.clientWidth,height:visualViewport.clientHeight,scale:visualViewport.scale},
    })).data,'base64');
  };
  const checks = [];
  async function settled(width, height, scale) {
    const expected = `${Math.trunc(width*scale)}x${Math.trunc(height*scale)}`;
    const until = Date.now()+60000;
    while (!messages.findLast(text=>text.includes('WGPU_PRESENT backbuffer '))?.includes(`backbuffer ${expected}`)) {
      assert(Date.now()<until, `WebGPU backbuffer did not reach ${expected}`);
      await page.waitForTimeout(100);
    }
    await page.waitForFunction(({width,height})=>{
      const rect=document.querySelector('canvas').getBoundingClientRect();
      return rect.x===0 && rect.y===0 && Math.trunc(rect.width)===width && Math.trunc(rect.height)===height;
    }, {width,height});
    // Verify that resizing produced a drawn frame before any mouse movement or
    // editor switch can trigger another redraw and hide a blank-frame bug.
    await page.waitForTimeout(500);
    const image=await capture();
    if (output) fs.writeFileSync(path.join(output,`resize-${checks.length}-${scale}.png`),image);
    const png=PNG.sync.read(image), colors=new Set();
    for (let i=0;i<png.data.length;i+=64) colors.add(png.data.readUInt32LE(i));
    assert(colors.size>100,'Resize must present the native UI without waiting for input');
    let cubePixels=0;
    for (let y=Math.floor(png.height*.2);y<png.height*.75;y++) {
      for (let x=Math.floor(png.width*.3);x<png.width*.65;x++) {
        const i=(y*png.width+x)*4;
        if (png.data[i]>210 && png.data[i+1]>100 && png.data[i+1]<200 && png.data[i+2]<100) cubePixels++;
      }
    }
    assert(cubePixels>30*scale,'Selected cube must survive resizing');
    await page.mouse.move(Math.min(300,width*.3),Math.min(250,height*.4));
    const marker=`RESIZE_STATE_${checks.length}:`;
    await consoleCommand(page,
      `import bpy, json, os; os.write(1, ('${marker}'+json.dumps({'window':[bpy.context.window.width,bpy.context.window.height], 'editors':[max(a.x+a.width for a in bpy.context.screen.areas),max(a.y+a.height for a in bpy.context.screen.areas)], 'uiScale':bpy.context.preferences.system.ui_scale})+'\\n').encode())`, marker, capture);
    const state=JSON.parse(messages.findLast(text=>text.startsWith(marker)).slice(marker.length));
    assert.deepEqual(state.window,[width,height], 'GHOST logical window size');
    // Screen.areas also excludes the native borders, whose pixel width scales.
    assert(Math.abs(state.editors[0]-Math.trunc(width*scale))<=2*scale+1,
      `Native editor width ${state.editors} must cover ${expected}`);
    // Screen.areas excludes the window's global top/status bars. Its top edge
    // must approach the physical window height, with room for those native bars.
    const pixelsHigh=Math.trunc(height*scale);
    assert(state.editors[1]<=pixelsHigh+2 && state.editors[1]>=pixelsHigh-64*scale,
      `Native editor height ${state.editors} must follow ${expected}`);
    const visible=await page.evaluate(()=>[...document.body.children].filter(element=>{
      const rect=element.getBoundingClientRect();
      return rect.width>0 && rect.height>0 && getComputedStyle(element).visibility!=='hidden';
    }).map(element=>element.tagName));
    assert.deepEqual(visible,['CANVAS'],'The entire running page must be Blender');
    checks.push({css:[width,height],scale,pixels:expected,colors:colors.size,cubePixels,...state});
  }
  try {
    // Rapid grow/shrink notifications must settle at the final dimensions.
    for (const size of [[900,600],[1440,900],[800,600],[1280,800],[1040,720]]) {
      await page.setViewportSize({width:size[0],height:size[1]});
    }
    await settled(1040,720,1);
    // Layout-only changes don't produce a browser window resize event.
    await page.evaluate(()=>Object.assign(document.querySelector('canvas').style,{width:'960px',height:'640px'}));
    await settled(960,640,1);
    const start=messages.length;
    await page.evaluate(()=>Object.assign(document.querySelector('canvas').style,{width:'960px',height:'0px'}));
    await page.waitForTimeout(300);
    assert(!messages.slice(start).some(text=>/WGPU_PRESENT backbuffer (0x|\d+x0\b)/.test(text)));
    await page.evaluate(()=>document.querySelector('canvas').removeAttribute('style'));
    await settled(1040,720,1);
    // Keep CSS dimensions constant while changing display density.
    for (const scale of [2,1.25,1]) {
      await cdp.send('Emulation.setDeviceMetricsOverride',{
        width:1040,height:720,deviceScaleFactor:scale,mobile:false,
      });
      await page.waitForFunction(scale=>devicePixelRatio===scale,scale);
      await settled(1040,720,scale);
    }
    assert(checks[3].uiScale>checks[0].uiScale,'Blender must increase its UI scale on a high-DPI display');
    return checks;
  } finally {
    await cdp.send('Emulation.clearDeviceMetricsOverride');
    await cdp.detach();
    await page.setViewportSize({width:1041,height:721});
    await page.setViewportSize({width:1040,height:720});
    await page.waitForFunction(()=>innerWidth===1040 && innerHeight===720 && devicePixelRatio===1);
  }
}
module.exports = {checkResize};
