/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {chromium} = require('playwright');
const {ready, consoleCommand, messages} = require('./native.cjs');
const uiFallback = process.env.WEB_TEST_UI_FALLBACK === '1';
const hardware = process.env.WEB_TEST_HARDWARE === '1';
const output = path.resolve(process.env.WEB_FEATURE_OUTPUT || (hardware ? 'build/native/feature-evidence-hardware' : uiFallback ? 'build/native/feature-evidence-ui' : 'build/native/feature-evidence'));

async function main() {
  fs.mkdirSync(output, {recursive:true});
  messages.length = 0;
  const browser = await chromium.launch({headless:true, args:hardware ? [
    '--enable-unsafe-webgpu', '--use-angle=vulkan', '--enable-features=Vulkan',
    '--use-vulkan=native', '--disable-vulkan-surface',
  ] : [
    '--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-angle=swiftshader',
    '--enable-features=Vulkan', '--use-vulkan=swiftshader',
  ]});
  const errors = [];
  const context = await browser.newContext({viewport:{width:1280,height:800}});
  if (uiFallback) await context.addInitScript(() => {
    delete WebAssembly.promising;
    delete WebAssembly.Suspending;
  });
  const page = await context.newPage();
  const tracing = process.env.WEB_FEATURE_PROFILE === '1' ? await browser.newBrowserCDPSession() : null;
  const log = fs.openSync(path.join(output,'browser.log'),'w');
  let report, failure, tracingStarted=false;
  page.on('console', message => {
    const text = message.text(); messages.push(text); fs.writeSync(log,text+'\n');
    if (text.startsWith('FEATURE_COMPLETE:')) report = JSON.parse(text.slice(17));
    if (text.startsWith('FEATURE_FAILED:')) failure = text;
    if (/Aborted\(|Blender WebGPU error|Blender WebGPU device lost|Blender startup failed|Blender GPU job failed|WGPU_SH .*FAIL|WGPU_SHADER translate FAILED/.test(text)) errors.push(text);
  });
  page.on('pageerror', e => errors.push(e.stack));
  try {
    await page.goto(process.env.WEB_TEST_URL || 'http://127.0.0.1:8093/');
    await ready(page, new Promise(()=>{}));
    assert.equal(await page.evaluate(()=>document.documentElement.dataset.blenderVariant),uiFallback ? 'ui' : 'render');
    const manifest = await page.evaluate(async()=>fetch('manifest.json').then(r=>r.json()));
    // Packaged-build CI has no compiler prefix. Match its notices against the
    // committed report that was checked against the real installed sources.
    let recordedNotices;
    if (process.env.WEB_FEATURE_NOTICES_REPORT) {
      const evidence=JSON.parse(fs.readFileSync(process.env.WEB_FEATURE_NOTICES_REPORT,'utf8'));
      assert.equal(evidence.bundleVersion,manifest.version,'Notices must belong to this tested build');
      recordedNotices=evidence.featureVariants.find(result=>result.variant===(uiFallback?'ui':'render') && !result.hardware);
      assert(recordedNotices,'Missing recorded notice checks');
    }
    for (const flag of ['WITH_IMAGE_WEBP','WITH_IMAGE_OPENJPEG','WITH_IMAGE_CINEON','WITH_IO_GREASE_PENCIL','WITH_PUGIXML','WITH_HARU','WITH_POTRACE','WITH_FREESTYLE','WITH_OPENVDB','WITH_NANOVDB','WITH_MOD_FLUID','WITH_FFTW3','WITH_MOD_OCEANSIM','WITH_ALEMBIC','WITH_GMP','WITH_MANIFOLD','WITH_MOD_REMESH','WITH_UV_SLIM','WITH_QUADRIFLOW','WITH_PYTHON_NUMPY','WITH_CYCLES','WITH_OPENSUBDIV','WITH_BULLET','WITH_IK_SOLVER','WITH_IK_ITASC','WITH_IO_WAVEFRONT_OBJ','WITH_IO_STL','WITH_IO_PLY','WITH_IO_FBX']) {
      assert.equal(manifest.buildOptions[flag],true,flag);
    }
    await page.evaluate(source=>Module.FS.writeFile('/tmp/web-features.py',source),
      fs.readFileSync(path.join(__dirname,'features.py'),'utf8'));
    await page.mouse.move(300,250);
    if (tracing) {
      await tracing.send('Tracing.start', {
        categories:'v8,disabled-by-default-v8.cpu_profiler',transferMode:'ReturnAsStream',
      });
      tracingStarted=true;
    }
    // Always emit a completion marker so a Python assertion reports immediately.
    await consoleCommand(page,"exec(compile(open('/tmp/web-features.py').read(), '/tmp/web-features.py', 'exec'))",'FEATURE_DONE:');
    assert(!failure,failure);
    assert(report,'Native Python must finish all feature checks');
    report.dependencyNotices = {};
    for (const [dependency,names] of Object.entries({
      opensubdiv:['LICENSE.txt','NOTICE.txt'], gmp:['COPYINGv2','COPYING.LESSERv3'],
      manifold:['LICENSE'], numpy:['LICENSE.txt','LICENSES_bundled.txt'],
      fftw:['COPYING'], alembic:['LICENSE.txt'],
      openvdb:['LICENSE'], blosc:['LICENSES/BLOSC.txt','LICENSES/LZ4.txt','LICENSES/SNAPPY.txt'],
      webp:['COPYING','PATENTS'], openjpeg:['LICENSE'], pugixml:['LICENSE.md'],
      haru:['LICENSE'], potrace:['COPYING'],
    })) {
      const notices=report.dependencyNotices[dependency]={};
      for (const name of names) {
        const response=await page.request.get(new URL(`licenses/dependencies/${dependency}/${name}`,page.url()).href);
        assert.equal(response.status(),200,`Published ${dependency} ${name}`);
        const bytes=await response.body();
        const digest=crypto.createHash('sha256').update(bytes).digest('hex');
        if (recordedNotices) assert.equal(digest,recordedNotices.dependencyNotices[dependency][name]);
        else {
          const installed=fs.readFileSync(path.resolve('build/native/deps/prefix/share/blender-web/licenses',dependency,name));
          assert(bytes.equals(installed),'Dependency notices must survive a cached-prefix build');
        }
        notices[name]=digest;
      }
    }
    const cyclesLicense=await page.request.get(new URL('licenses/cycles/Apache2-license.txt',page.url()).href);
    assert.equal(cyclesLicense.status(),200,'Publish Cycles license');
    const license=await cyclesLicense.body();
    report.cyclesLicense=crypto.createHash('sha256').update(license).digest('hex');
    if (recordedNotices) assert.equal(report.cyclesLicense,recordedNotices.cyclesLicense);
    else assert(license.equals(fs.readFileSync('blender/intern/cycles/doc/license/Apache2-license.txt')));
    report.opensubdivNotices=report.dependencyNotices.opensubdiv;
    const base64 = await page.evaluate(file=>{
      const bytes=Module.FS.readFile(file), pieces=[];
      for(let offset=0;offset<bytes.length;offset+=32768) pieces.push(String.fromCharCode(...bytes.subarray(offset,offset+32768)));
      return btoa(pieces.join(''));
    },'/tmp/web-features/feature-scene.blend');
    const blend=Buffer.from(base64,'base64');
    assert.equal(blend.length,report.blendFile.bytes);
    const zstd=blend.readUInt32LE(0)===0xfd2fb528;
    assert(zstd || blend.subarray(0,7).toString()==='BLENDER','Expected a native or Zstd-compressed blend file');
    report.blendFile.compression=zstd ? 'zstd' : 'none';
    report.blendFile.sha256=crypto.createHash('sha256').update(blend).digest('hex');
    fs.writeFileSync(path.join(output,'feature-scene.blend'),blend);
    await page.screenshot({path:path.join(output,'physics-scene.png')});
    // Start a new browser page while preserving OPFS, then read the saved native file.
    await page.reload();
    await ready(page,new Promise(()=>{}));
    await page.mouse.move(300,250);
    await consoleCommand(page,"import bpy; bpy.ops.wm.open_mainfile(filepath='/user/blender-web/home/web-feature-scene.blend'); __import__('os').write(1,b'FEATURE_BLEND_OPEN\\n')",'FEATURE_BLEND_OPEN');
    await page.mouse.move(300,250);
    await consoleCommand(page,"import bpy, json, os; os.write(1, ('FEATURE_REOPEN:'+json.dumps({'objects':sorted(o.name for o in bpy.context.scene.objects),'bullet':bpy.context.scene.rigidbody_world is not None,'active':bpy.data.objects['WebFallingCube'].rigid_body.type,'passive':bpy.data.objects['WebFloor'].rigid_body.type})+'\\n').encode())",'FEATURE_REOPEN:');
    const reopened = JSON.parse(messages.findLast(t=>t.startsWith('FEATURE_REOPEN:')).slice(15));
    assert.deepEqual(reopened.objects,report.blendFile.expectedObjects);
    assert(reopened.bullet);
    assert.equal(reopened.active,'ACTIVE'); assert.equal(reopened.passive,'PASSIVE');
    assert.deepEqual(errors,[]);
    report.blendFile.reopenedAfterBrowserReload = true;
    report.bundleVersion = manifest.version;
    report.browserVersion = browser.version();
    report.measuredAt = new Date().toISOString();
    report.variant = uiFallback ? 'ui' : 'render';
    report.hardware = hardware;
    report.adapter = await page.evaluate(async()=>{
      const adapter=await navigator.gpu.requestAdapter({powerPreference:'high-performance'});
      return {vendor:adapter.info.vendor,architecture:adapter.info.architecture,
        device:adapter.info.device,description:adapter.info.description,
        maxStorageBuffersPerShaderStage:adapter.limits.maxStorageBuffersPerShaderStage};
    });
    if (hardware) assert(!/swiftshader|llvmpipe|lavapipe|software/i.test(JSON.stringify(report.adapter)),
      'Hardware feature acceptance must use a hardware WebGPU adapter');
    report.wasmBytes = manifest.files.find(file=>file.name.endsWith(uiFallback ? '/blender-ui.wasm' : '/blender.wasm')).bytes;
    report.browserErrors = errors.length;
    await page.screenshot({path:path.join(output,'reopened-scene.png')});
    await page.mouse.move(300,250);
    await consoleCommand(page,"import bpy, os; bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete(use_global=False); bpy.ops.mesh.primitive_cube_add(); bpy.context.preferences.system.use_gpu_subdivision=True; bpy.context.object.modifiers.new('WebSubdivision', 'SUBSURF').levels=2; os.write(1,b'FEATURE_SUBDIVISION_VIEWPORT\\n')",'FEATURE_SUBDIVISION_VIEWPORT');
    await page.screenshot({path:path.join(output,'subdivision-scene.png')});
    assert.deepEqual(errors,[],'Subdivision viewport must draw without WebGPU errors');
    report.subdivisionEvaluator='CPU';
    report.subdivisionViewport=true;
    await page.evaluate(source=>Module.FS.writeFile('/tmp/modeling.py',source),
      fs.readFileSync(path.join(__dirname,'modeling.py'),'utf8'));
    await consoleCommand(page,"exec(compile(open('/tmp/modeling.py').read(), '/tmp/modeling.py', 'exec'))",'MODELING_DONE');
    const modelingFailure=messages.find(text=>text.includes('MODELING_FAILED:'));
    assert(!modelingFailure,modelingFailure);
    const completed=messages.findLast(text=>text.includes('MODELING_COMPLETE:'));
    assert(completed,'Native modeling and NumPy checks must complete');
    report.modeling=JSON.parse(completed.slice(completed.indexOf('MODELING_COMPLETE:')+'MODELING_COMPLETE:'.length));
    await page.screenshot({path:path.join(output,'quadriflow-scene.png')});
    assert.deepEqual(errors,[],'New modeling features must draw without WebGPU errors');
    fs.writeFileSync(path.join(output,'acceptance.json'),JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report));
  } catch (e) {
    await page.screenshot({path:path.join(output,'failure.png')}).catch(()=>{});
    throw new Error(failure || e.stack);
  } finally {
    if (tracingStarted) {
      const completed=new Promise(resolve=>tracing.once('Tracing.tracingComplete',resolve));
      await tracing.send('Tracing.end');
      const {stream}=await completed;
      const file=fs.openSync(path.join(output,'browser-trace.json'),'w');
      for (;;) {
        const part=await tracing.send('IO.read',{handle:stream});
        fs.writeSync(file,part.base64Encoded ? Buffer.from(part.data,'base64') : part.data);
        if (part.eof) break;
      }
      fs.closeSync(file); await tracing.send('IO.close',{handle:stream});
    }
    if (tracing) await tracing.detach();
    await browser.close(); fs.closeSync(log);
  }
}
main().catch(e=>{console.error(e);process.exitCode=1;});
