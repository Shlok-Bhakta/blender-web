/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {PNG} = require('pngjs');
const {chromium} = require('playwright');
const {ready, consoleCommand, messages} = require('./native.cjs');
const fallback = process.env.WEB_TEST_UI_FALLBACK === '1';
const hardware = process.env.WEB_TEST_HARDWARE === '1';
const benchmark = process.env.WEB_MEDIA_BENCHMARK === '1';
const output = path.resolve(process.env.WEB_MEDIA_OUTPUT || `build/native/media-evidence${hardware ? '-hardware' : ''}${fallback ? '-ui' : ''}`);
async function main() {
  fs.mkdirSync(output, {recursive:true});
  messages.length = 0;
  fs.writeFileSync(path.join(output, 'browser.log'), '');
  const browser = await chromium.launch({headless:true, args:hardware ? [
    '--enable-unsafe-webgpu', '--use-angle=vulkan', '--enable-features=Vulkan',
    '--use-vulkan=native', '--disable-vulkan-surface',
  ] : [
    '--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-angle=swiftshader',
    '--enable-features=Vulkan', '--use-vulkan=swiftshader',
  ]});
  const context = await browser.newContext({viewport:{width:1280,height:800},
    ...(benchmark ? {} : {recordVideo:{dir:output,size:{width:1280,height:800}}})});
  if (fallback) await context.addInitScript(() => { delete WebAssembly.promising; delete WebAssembly.Suspending; });
  // Capture the actual Web Audio output, including both channels. This hook is
  // confined to the test browser; the application has no recording UI.
  await context.addInitScript(() => {
    const connect = AudioNode.prototype.connect;
    AudioNode.prototype.connect = function(destination, ...args) {
      const result = connect.call(this, destination, ...args);
      if (this instanceof AudioWorkletNode && destination instanceof AudioDestinationNode) {
        const sink = this.context.createMediaStreamDestination();
        connect.call(this, sink);
        const recorder = new MediaRecorder(sink.stream, {mimeType:'audio/webm;codecs=opus'});
        const chunks = [];
        recorder.ondataavailable = event => chunks.push(event.data);
        window.mediaAudioCapture = {recorder, chunks, started:Date.now()};
        recorder.start();
      }
      return result;
    };
  });
  const page = await context.newPage(), errors = [], chapters = [], started = Date.now();
  const profile = process.env.WEB_MEDIA_PROFILE === '1' ?
    await require('./render-profile.cjs').profileRender(browser, page, output) : null;
  let rejectFailure;
  const failure = new Promise((_, reject) => rejectFailure = reject);
  failure.catch(() => {});
  const fail = text => { errors.push(text); rejectFailure(Error(text)); };
  page.on('console', message => {
    const text = message.text();
    messages.push(text);
    fs.appendFileSync(path.join(output, 'browser.log'), text + '\n');
    if (/MEDIA_FAILED:|Aborted\(|Blender startup failed|Blender audio device failed|Blender WebGPU error|Blender WebGPU device lost|Blender GPU job failed|Blender UI frame failed|MEDIA_WORKER_EXCEPTION/.test(text)) fail(text);
  });
  page.on('pageerror', error => fail(error.stack || error.message));
  page.on('worker', worker => worker.evaluate(() => self.addEventListener('error', event => {
    console.error('MEDIA_WORKER_EXCEPTION', event.message, event.error?.stack);
  })).catch(() => {}));
  const command = (code, marker) => Promise.race([failure,
    consoleCommand(page, code, marker, () => page.screenshot(), 240000)]);
  const chapter = title => chapters.push({seconds:(Date.now() - started) / 1000,title});
  try {
    chapter('Open original Blender with FFmpeg and Audaspace');
    await page.goto(process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/');
    await ready(page, failure);
    const manifest = await page.evaluate(() => fetch('manifest.json').then(response => response.json()));
    const adapter = await page.evaluate(async () => {
      const adapter = await navigator.gpu.requestAdapter({powerPreference:'high-performance'});
      return {vendor:adapter.info.vendor,architecture:adapter.info.architecture,
        device:adapter.info.device,description:adapter.info.description};
    });
    if (hardware) assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(adapter)));
    if (!fallback) {
      await page.mouse.move(300, 250);
      await command("import bpy,os; bpy.ops.object.select_all(action='DESELECT'); os.write(1,b'MEDIA_PICK_CLEAR\\n')", 'MEDIA_PICK_CLEAR');
      await page.mouse.click(520, 360);
      await command("import json; os.write(1,('MEDIA_PICKED:'+json.dumps([o.name for o in bpy.context.selected_objects])+'\\n').encode())", 'MEDIA_PICKED:');
      const picked = JSON.parse(messages.find(text => text.startsWith('MEDIA_PICKED:')).slice('MEDIA_PICKED:'.length));
      assert.deepEqual(picked, ['Cube'], 'Native mouse selection must read back the cube ID');
    }
    for (const flag of ['WITH_CODEC_FFMPEG', 'WITH_CODEC_SNDFILE', 'WITH_AUDASPACE']) assert.equal(manifest.buildOptions[flag], true, flag);
    const source = fs.readFileSync(path.join(__dirname, 'media.py'), 'utf8');
    const sourceSha256 = crypto.createHash('sha256').update(source).digest('hex');
    const fixtures = path.join(__dirname, 'fixtures/media');
    const reference = JSON.parse(fs.readFileSync(path.join(fixtures, 'reference.json')));
    assert.equal(sourceSha256, reference.sourceSha256, 'Regenerate desktop media references');
    const fixtureHashes = {};
    for (const name of ['reference.json', 'tone.wav', 'tone.flac', 'tone.ogg', 'tone.mp3',
      'reference.mkv', 'reference.mp4', 'reference.webm', 'frame-12.png', 'check.py']) {
      const bytes = name === 'check.py' ? Buffer.from(source) : fs.readFileSync(path.join(fixtures, name));
      fixtureHashes[name] = crypto.createHash('sha256').update(bytes).digest('hex');
      await page.evaluate(({name, base64}) => {
        const bytes = Uint8Array.from(atob(base64), character => character.charCodeAt(0));
        for (const directory of ['/tmp/media', '/user/blender-web/home/media']) {
          Module.FS.mkdirTree(directory);
          Module.FS.writeFile(directory + '/' + name, bytes);
        }
      }, {name,base64:bytes.toString('base64')});
    }
    chapter('Decode native fixtures, seek video, export audio and movies');
    await page.mouse.move(300, 250);
    if (profile) await profile.start();
    await command(`exec(compile(__import__('pathlib').Path('/tmp/media/check.py').read_text(),'media.py','exec')); browser_check('/tmp/media',render=${fallback ? 'False' : 'True'})`, 'MEDIA_FINISHED');
    if (profile) await profile.stop();
    const report = JSON.parse(messages.find(text => text.startsWith('MEDIA_DONE:')).slice('MEDIA_DONE:'.length));
    report.resources = await page.evaluate(() => ({hardwareConcurrency:navigator.hardwareConcurrency,
      wasmHeapCapacityBytes:typeof wasmMemory === 'undefined' ? null : wasmMemory.buffer.byteLength}));
    for (const name of await page.evaluate(() => Module.FS.readdir('/tmp/media').filter(name => /^(export\.|frame-|media\.blend|results\.json)/.test(name)))) {
      const bytes = await page.evaluate(name => Array.from(Module.FS.readFile('/tmp/media/' + name)), name);
      fs.writeFileSync(path.join(output, name), Buffer.from(bytes));
    }
    chapter('Play the original Video Sequencer with browser audio');
    await page.evaluate(() => { blenderWebAudioContext(); });
    await page.keyboard.press('Shift');
    await page.waitForFunction(() => blenderWebAudioContext().state === 'running', null, {timeout:10000});
    const playback = `def media_playback():
    window=bpy.context.window_manager.windows[0]
    scene=bpy.context.scene
    window.workspace.sequencer_scene=scene
    scene.render.resolution_x=128
    scene.render.resolution_y=96
    scene.render.resolution_percentage=100
    scene.view_settings.view_transform='Standard'
    scene.view_settings.look='None'
    editor=scene.sequence_editor
    movie=next(s for s in editor.strips if s.type=='MOVIE' and not s.mute)
    sound=next(s for s in editor.strips if s.type=='SOUND')
    for index in range(1,4):
        editor.strips.new_movie('Video '+str(index),movie.filepath,1,1+24*index)
        editor.strips.new_sound('Stereo tone '+str(index),sound.sound.filepath,4,1+24*index)
    scene.frame_end=96
    area=max((a for a in window.screen.areas if a.type=='VIEW_3D'),key=lambda a:a.width*a.height)
    area.type='SEQUENCE_EDITOR'
    area.spaces.active.view_type='SEQUENCER_PREVIEW'
    scene.frame_set(12)
    media_playback.area=area
    def sample():
        elapsed=__import__('time').monotonic()-media_playback.started
        __import__('os').write(1,('MEDIA_PLAYHEAD:'+__import__('json').dumps([elapsed,scene.frame_current])+'\\n').encode())
        if elapsed>=3:
            with bpy.context.temp_override(window=window,area=area):
                bpy.ops.screen.animation_cancel(restore_frame=False)
            scene.frame_set(12)
            regions=[r for r in area.regions if r.type=='PREVIEW']
            if regions:
                with bpy.context.temp_override(window=window,area=area,region=regions[0]):
                    bpy.ops.sequencer.view_all_preview()
                __import__('os').write(1,('MEDIA_PREVIEW_REGION:'+__import__('json').dumps([regions[0].x,regions[0].y,regions[0].width,regions[0].height])+'\\n').encode())
            __import__('os').write(1,b'MEDIA_PLAYBACK_DONE\\n')
            return None
        return 0.1
    def begin():
        scene.frame_set(1)
        media_playback.started=__import__('time').monotonic()
        with bpy.context.temp_override(window=window,area=area):
            assert bpy.ops.screen.animation_play()=={'FINISHED'}
        bpy.app.timers.register(sample,first_interval=0.1)
        return None
    bpy.app.timers.register(begin,first_interval=2)
    return None
bpy.app.timers.register(media_playback,first_interval=2)`;
    await command(`exec(${JSON.stringify(playback)}); __import__('os').write(1,b'MEDIA_PLAYBACK_SCHEDULED\\n')`, 'MEDIA_PLAYBACK_SCHEDULED');
    await page.waitForTimeout(1800);
    await page.keyboard.press('Shift');
    const deadline = Date.now() + 30000;
    while (!messages.includes('MEDIA_PLAYBACK_DONE')) {
      if (Date.now() > deadline) throw Error('Timeline playback did not finish');
      await Promise.race([failure, page.waitForTimeout(100)]);
    }
    await page.waitForTimeout(150);
    report.playback = await page.evaluate(() => blenderWebAudioStats());
    report.audioCaptureOffsetSeconds = ((await page.evaluate(() => window.mediaAudioCapture.started)) - started) / 1000;
    assert.equal(report.playback.state, 'running');
    assert(report.playback.devices.some(device => device.nonzeroFrames > report.playback.sampleRate * 2.7), JSON.stringify(report.playback));
    report.playhead = messages.filter(text => text.startsWith('MEDIA_PLAYHEAD:')).map(text => JSON.parse(text.slice('MEDIA_PLAYHEAD:'.length)));
    assert(report.playhead.length >= 5, 'Sample the timeline throughout playback');
    const advance = report.playhead.slice(1).reduce((sum, sample, index) =>
      sum + (sample[1] - report.playhead[index][1] + 96) % 96, 0);
    report.playback.timelineFps = advance / (report.playhead.at(-1)[0] - report.playhead[0][0]);
    assert(report.playback.timelineFps > 22 && report.playback.timelineFps < 26, JSON.stringify(report.playback));
    const screenshot = await page.screenshot({path:path.join(output, 'media-sequencer.png')});
    const png = PNG.sync.read(screenshot);
    const region = JSON.parse(messages.find(text => text.startsWith('MEDIA_PREVIEW_REGION:')).slice('MEDIA_PREVIEW_REGION:'.length));
    const expected = PNG.sync.read(fs.readFileSync(path.join(fixtures, 'frame-12.png')));
    const center = (48 * expected.width + 64) * 4;
    report.previewColor = Array.from(expected.data.slice(center, center + 3));
    let matched = 0;
    for (let y = png.height - region[1] - region[3]; y < png.height - region[1]; y++) {
      for (let x = region[0]; x < region[0] + region[2]; x++) {
        const offset = (y * png.width + x) * 4;
        if (report.previewColor.every((value, channel) => Math.abs(png.data[offset + channel] - value) <= 4)) matched++;
      }
    }
    report.previewPixelsMatched = matched;
    assert(matched > 1000, `Video preview must display decoded pixels: ${matched}`);
    const capture = await page.evaluate(async () => {
      const {recorder,chunks} = window.mediaAudioCapture;
      await new Promise(resolve => { recorder.onstop = resolve; recorder.stop(); });
      const bytes = new Uint8Array(await new Blob(chunks).arrayBuffer());
      return Array.from(bytes);
    });
    fs.writeFileSync(path.join(output, 'playback-audio.webm'), Buffer.from(capture));
    chapter('Reopen cached Blender offline and repeat media decoding');
    await context.setOffline(true);
    await page.reload();
    await ready(page, failure);
    await page.mouse.move(300, 250);
    const before = messages.length;
    await command("exec(compile(__import__('pathlib').Path('/user/blender-web/home/media/check.py').read_text(),'media.py','exec')); browser_check('/user/blender-web/home/media',render=False)", 'MEDIA_FINISHED');
    const offline = JSON.parse(messages.slice(before).find(text => text.startsWith('MEDIA_DONE:')).slice('MEDIA_DONE:'.length));
    assert.equal(offline.sourceSha256, sourceSha256);
    await command("media_offline_device=aud.Device('Web Audio'); media_offline_handle=media_offline_device.play(aud.Sound('/user/blender-web/home/media/tone.wav')); __import__('os').write(1,b'MEDIA_OFFLINE_PLAYING\\n')", 'MEDIA_OFFLINE_PLAYING');
    await page.keyboard.press('Shift');
    await page.waitForFunction(() => blenderWebAudioStats().devices.some(device => device.nonzeroFrames > 24000), null, {timeout:15000});
    report.offlinePlayback = await page.evaluate(() => blenderWebAudioStats());
    report.offlineReopen = true;
    Object.assign(report, {bundleVersion:manifest.version,sourceSha256,fixtureHashes,fallback,hardware,adapter,
      interactiveSelectionReadback:!fallback,
      browserVersion:browser.version(),browserErrors:errors,chapters,profiled:!!profile,
      playbackWarmupSeconds:2,
      playbackTimelineFrames:96,playbackClipCopies:4,
      videoCaptured:!benchmark,videoPath:benchmark ? null : path.join(output,'media.webm')});
    assert.deepEqual(errors, []);
    fs.writeFileSync(path.join(output, 'acceptance.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify({bundleVersion:manifest.version,formats:Object.keys(report.audio),
      playback:report.playback,offlineReopen:true}));
  }
  catch (error) {
    await page.screenshot({path:path.join(output, 'failure.png')}).catch(() => {});
    throw error;
  }
  finally {
    if (profile) await profile.dispose();
    const video = page.video();
    await context.close();
    if (video) await video.saveAs(path.join(output, 'media.webm'));
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
