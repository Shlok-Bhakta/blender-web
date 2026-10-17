/* SPDX-License-Identifier: GPL-2.0-or-later */
// Opt-in instrumentation. API timings measure CPU call time, not GPU execution.
const fs = require('node:fs');
const path = require('node:path');
async function profileRender(browser, page, output) {
  const installs = [];
  let sampler;
  const resources = [];
  const read = file => { try { return fs.readFileSync(file,'utf8').trim(); } catch { return null; } };
  const readNumber = file => { const value = read(file); return value === null ? null : Number(value); };
  // Idle pthreads park in Atomics.wait and cannot service debugger evaluation.
  const responsive = promise => Promise.race([promise, new Promise(resolve => setTimeout(() => resolve(null), 1000))]);
  page.on('worker', worker => installs.push(responsive(worker.evaluate(() => {
    globalThis.renderProfile = {calls:{}, waits:{}};
    const wrap = (proto, names) => {
      for (const name of names) {
        const original = proto?.[name];
        if (!original) continue;
        proto[name] = function(...args) {
          const start = performance.now();
          const result = original.apply(this, args);
          const stats = globalThis.renderProfile;
          const entry = stats.calls[name] ||= {count:0, ms:0};
          entry.count++; entry.ms += performance.now()-start;
          if (name === 'mapAsync') result.then(() => {
            const wait = stats.waits[name] ||= {count:0, ms:0};
            wait.count++; wait.ms += performance.now()-start;
          }, () => {});
          return result;
        };
      }
    };
    wrap(globalThis.GPUDevice?.prototype, ['createShaderModule','createComputePipeline','createRenderPipeline','createBuffer','createTexture','createBindGroup','createCommandEncoder']);
    wrap(globalThis.GPUQueue?.prototype, ['submit','writeBuffer','writeTexture']);
    wrap(globalThis.GPUBuffer?.prototype, ['mapAsync']);
    wrap(globalThis.GPUCommandEncoder?.prototype, ['beginComputePass','beginRenderPass','finish']);
  }).catch(error => ({error:String(error)})))));
  const cdp = await browser.newBrowserCDPSession();
  return {
    async dispose() { clearInterval(sampler); await cdp.detach(); },
    async start() {
      resources.length = 0;
      const installed = await Promise.all(installs);
      if (installed.some(x=>x?.error)) throw new Error(JSON.stringify(installed));
      await Promise.all(page.workers().map(w => responsive(w.evaluate(() => {
        globalThis.renderProfile = {calls:{}, waits:{}};
      }))));
      sampler = setInterval(() => resources.push({time:Date.now(), gpuBusyPercent:readNumber('/sys/class/drm/card0/device/gpu_busy_percent'), gpuMemoryBytes:readNumber('/sys/class/drm/card0/device/mem_info_gtt_used'), cpu:read('/proc/stat')?.split('\n')[0]}), 100);
      await cdp.send('Tracing.start', {categories:'devtools.timeline,v8,disabled-by-default-v8.cpu_profiler,disabled-by-default-devtools.timeline', transferMode:'ReturnAsStream'});
    },
    async stop(run = 0) {
      clearInterval(sampler);
      fs.writeFileSync(path.join(output,`resources-${run}.json`),JSON.stringify(resources,null,2)+'\n');
      const complete = new Promise(resolve => cdp.once('Tracing.tracingComplete', resolve));
      await cdp.send('Tracing.end');
      const {stream} = await complete;
      const file = fs.openSync(path.join(output,`browser-trace-${run}.json`),'w');
      try {
        for (;;) {
          const part = await cdp.send('IO.read', {handle:stream});
          fs.writeSync(file, part.base64Encoded ? Buffer.from(part.data,'base64') : part.data);
          if (part.eof) break;
        }
      } finally { fs.closeSync(file); await cdp.send('IO.close',{handle:stream}); }
      const workers = await Promise.all(page.workers().map(w => responsive(w.evaluate(() => ({...globalThis.renderProfile, hardwareConcurrency:navigator.hardwareConcurrency})).catch(error=>({error:String(error)})))));
      if (!workers.some(w => w?.calls?.submit?.count > 0)) throw new Error('No GPU-owner worker profiling data captured');
      fs.writeFileSync(path.join(output,`webgpu-profile-${run}.json`),JSON.stringify(workers,null,2)+'\n');
      return workers;
    }
  };
}
module.exports = {profileRender};
