/* SPDX-License-Identifier: GPL-2.0-or-later */
const fs = require('node:fs');
const {performance} = require('node:perf_hooks');
const readNumber = file => {
  try { return Number(fs.readFileSync(file,'utf8').trim()); } catch { return null; }
};
// Lightweight sampling, without CPU stack tracing or WebGPU API wrappers.
async function sampleBrowserResources(browser) {
  const cdp = await browser.newBrowserCDPSession();
  const samples = [], processes = [], failures = [];
  let gpuTimer, cpuTimer, pending;
  const start = performance.now();
  async function cpu() {
    try {
      const {processInfo} = await cdp.send('SystemInfo.getProcessInfo');
      processes.push({seconds:(performance.now()-start)/1000, processInfo:processInfo.map(p => {
        let rssBytes = null;
        try { rssBytes = Number(fs.readFileSync(`/proc/${p.id}/status`,'utf8').match(/VmRSS:\s+(\d+)/)[1])*1024; } catch {}
        return {...p, rssBytes};
      })});
    } catch(error) { failures.push(String(error)); }
  }
  await cpu();
  const sample = () => samples.push({seconds:(performance.now()-start)/1000,
    gpuBusyPercent:readNumber('/sys/class/drm/card0/device/gpu_busy_percent'),
    gpuSharedMemoryBytes:readNumber('/sys/class/drm/card0/device/mem_info_gtt_used')});
  sample();
  gpuTimer = setInterval(sample,100);
  cpuTimer = setInterval(() => { if (!pending) pending=cpu().finally(()=>{pending=null;}); },1000);
  return {
    async stop() {
      clearInterval(gpuTimer); clearInterval(cpuTimer);
      if (pending) await pending;
      await cpu(); sample();
      await cdp.detach();
      const busy=samples.map(s=>s.gpuBusyPercent).filter(x=>x!==null);
      const first=processes[0], last=processes.at(-1);
      const initial=new Map((first?.processInfo || []).map(p=>[p.id,p.cpuTime]));
      const final=new Map();
      for (const snapshot of processes) for (const p of snapshot.processInfo) final.set(p.id,p.cpuTime);
      let cpuSeconds=0;
      for (const [id,time] of final) cpuSeconds+=Math.max(0,time-(initial.get(id)||0));
      const duration=last.seconds-first.seconds;
      const peakRssBytes=Math.max(...processes.map(s=>s.processInfo.reduce((sum,p)=>sum+(p.rssBytes||0),0)));
      const memory=samples.map(s=>s.gpuSharedMemoryBytes).filter(x=>x!==null);
      return {summary:{sampleSeconds:duration, browserCpuSeconds:cpuSeconds,
        averageCpuCores:cpuSeconds/duration, peakAggregateRssBytes:peakRssBytes,
        gpuBusyMeanPercent:busy.length ? busy.reduce((a,b)=>a+b,0)/busy.length : null,
        gpuBusyPeakPercent:busy.length ? Math.max(...busy) : null,
        gpuBusySamples:busy.length, peakGpuSharedMemoryBytes:memory.length ? Math.max(...memory) : null, failures},samples,processes};
    }
  };
}
module.exports = {sampleBrowserResources};
