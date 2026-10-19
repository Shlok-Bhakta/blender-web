/* SPDX-License-Identifier: GPL-2.0-or-later */
// Serialized into page/worker realms only during opt-in profiling.
function installGpuTimestamps() {
  const state = globalThis.gpuTimestamps = {device:null, records:[], active:false, overflow:0};
  const request = globalThis.GPUAdapter?.prototype.requestDevice;
  if (request) GPUAdapter.prototype.requestDevice = function(descriptor={}) {
    const features = new Set(descriptor.requiredFeatures || []);
    if (this.features.has('timestamp-query')) features.add('timestamp-query');
    return request.call(this, {...descriptor,requiredFeatures:[...features]});
  };
  const create = globalThis.GPUDevice?.prototype.createCommandEncoder;
  if (!create) return;
  GPUDevice.prototype.createCommandEncoder = function(...args) {
    state.device=this;
    return create.apply(this,args);
  };
  const records=new WeakMap();
  for (const [method,proto,kind] of [
    ['beginComputePass',GPUComputePassEncoder.prototype,'compute'],
    ['beginRenderPass',GPURenderPassEncoder.prototype,'render'],
  ]) {
    const begin=GPUCommandEncoder.prototype[method];
    GPUCommandEncoder.prototype[method]=function(descriptor={}) {
      if (!state.active) return begin.call(this,descriptor);
      const index=state.records.length*2;
      if(index+1>=16384) {state.overflow++;return begin.call(this,descriptor);}
      const chunk=Math.floor(index/4096),local=index%4096;
      state.queries[chunk] ||= state.device.createQuerySet({type:'timestamp',count:4096});
      const record={kind,label:descriptor.label||'',pipelines:[],index};
      state.records.push(record);
      const pass=begin.call(this,{...descriptor,timestampWrites:{querySet:state.queries[chunk],
        beginningOfPassWriteIndex:local,endOfPassWriteIndex:local+1}});
      records.set(pass,record);
      return pass;
    };
    const pipeline=proto.setPipeline;
    proto.setPipeline=function(value) {
      const record=records.get(this);
      if(record && !record.pipelines.includes(value.label)) record.pipelines.push(value.label);
      return pipeline.call(this,value);
    };
  }
  state.start=()=>{
    if(!state.device) return false;
    if(!state.device.features.has('timestamp-query')) throw new Error('GPU timestamp feature was not enabled');
    state.records=[];state.overflow=0;
    state.queries=[];
    state.active=true;
    return true;
  };
  state.stop=async()=>{
    if(!state.active) return null;
    state.active=false;
    const device=state.device,count=state.records.length*2;
    if(!count) throw new Error('No GPU timestamp samples captured');
    const resolve=device.createBuffer({size:count*8,usage:GPUBufferUsage.QUERY_RESOLVE|GPUBufferUsage.COPY_SRC});
    const read=device.createBuffer({size:count*8,usage:GPUBufferUsage.MAP_READ|GPUBufferUsage.COPY_DST});
    try {
      const encoder=device.createCommandEncoder();
      for(let offset=0;offset<count;offset+=4096)
        encoder.resolveQuerySet(state.queries[offset/4096],0,Math.min(count-offset,4096),resolve,offset*8);
      encoder.copyBufferToBuffer(resolve,0,read,0,count*8);
      device.queue.submit([encoder.finish()]);
      await read.mapAsync(GPUMapMode.READ);
      const values=new BigUint64Array(read.getMappedRange());
      const groups={};
      let invalid=0;
      for(const record of state.records) {
        const start=values[record.index],end=values[record.index+1];
        if(!start || end<start) {invalid++;continue;}
        const key=`${record.kind}:${record.pipelines.join(',')||record.label||'clear'}`;
        const group=groups[key] ||= {count:0,milliseconds:0};
        group.count++;group.milliseconds+=Number(end-start)/1e6;
      }
      return {passes:count/2,overflow:state.overflow,invalid,
        groups:Object.fromEntries(Object.entries(groups).sort((a,b)=>b[1].milliseconds-a[1].milliseconds))};
    } finally {
      read.destroy();resolve.destroy();
      // Blender may still hold an unsubmitted UI encoder from this interval.
      // Keep query sets alive until this profiling browser context is closed.
      (state.retiredQueries ||= []).push(...state.queries);
    }
  };
}
module.exports={installGpuTimestamps};
