/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone ownership experiment; not linked into Blender. */
#include <cstddef>
#include <cstdint>
#include <emscripten.h>
// clang-format off
EM_JS_DEPS(owned, "$getDylinkMetadata,$setWasmTableEntry,$addFunction,$getFunctionAddress");
EM_JS(unsigned, owned_create, (const void *bytes, size_t size, unsigned external, uintptr_t memory_arg, uintptr_t table_arg, unsigned capacity_arg), {
  const state = Module.ownedState ||= {programs:new Map(),blocks:[],next:1,liveBytes:0,minDataBase:Infinity,owners:new Map(),rootSlots:new Map()};
  if(external && state.programs.has(external)) {
    const old=state.programs.get(external);
    if(old.memoryBase!==(memory_arg>>>0)||old.block.base!==(table_arg>>>0)||old.block.capacity!==capacity_arg) {
      Module.ownedLastError='Existing program descriptor mismatch';return 0;
    }
    return external;
  }
  const handle=external || state.next++;
  let raw=0, block;
  const clear=()=>{ if(block){for(let i=0;i<block.capacity;i++){const slot=block.base+i;if(state.owners.get(slot)===handle){setWasmTableEntry(slot,null);state.owners.delete(slot);}}block.used=false;}if(raw)_free(raw); };
  try {
    const binary = new Uint8Array(wasmMemory.buffer,bytes>>>0,size>>>0).slice();
    const module = new WebAssembly.Module(binary), meta = getDylinkMetadata(module);
    if(meta.neededDynlibs.length || meta.tlsExports.size)throw new Error('External libraries and TLS definitions are not supported in this experiment');
    const names=WebAssembly.Module.exports(module).filter(e=>e.kind==='function').map(e=>e.name).sort();
    const needed=meta.tableSize+names.length, align=2**meta.tableAlign;
    if(external) {
      block={base:table_arg>>>0,capacity:capacity_arg,used:true};
      if(wasmTable.length<block.base+block.capacity)wasmTable.grow(block.base+block.capacity-wasmTable.length);
    } else {
      block=state.blocks.find(b=>!b.used && b.capacity>=needed && b.base%align===0);
      if(!block) {
        const base=Math.ceil(wasmTable.length/align)*align;
        wasmTable.grow(base+needed-wasmTable.length);
        block={base,capacity:needed,used:false};state.blocks.push(block);
      }
      block.used=true;
    }
    let memoryBase=memory_arg>>>0;
    if(!external && meta.memorySize) {
      const alignment=2**meta.memoryAlign;
      raw=_malloc(meta.memorySize+alignment)>>>0;
      if(!raw)throw new Error('Static data allocation failed');
      memoryBase=Math.ceil(raw/alignment)*alignment;state.minDataBase=Math.min(state.minDataBase,memoryBase);
      new Uint8Array(wasmMemory.buffer,memoryBase,meta.memorySize).fill(0);
    }
    let instance;
    const entries=new Map(names.map((n,i)=>[n,block.base+meta.tableSize+i]));
    const resolve=name=>{
      if(wasmExports[name]!==undefined)return wasmExports[name];
      if(wasmImports[name]!==undefined && !wasmImports[name].stub)return wasmImports[name];
      if(instance && instance.exports[name]!==undefined)return instance.exports[name];
      throw new Error('Unresolved owned-program symbol: '+name);
    };
    const got=new Map();
    const gotProxy=new Proxy({}, {get:(_,name)=>{
      if(!got.has(name))got.set(name,new WebAssembly.Global({value:'i32',mutable:true},0));
      return got.get(name);
    }});
    const env=new Proxy({}, {get:(_,name)=>{
      if(name==='__memory_base')return memoryBase;
      if(name==='__table_base')return block.base;
      if(name==='memory')return wasmMemory;
      if(name==='__indirect_function_table')return wasmTable;
      return resolve(name);
    }});
    instance=new WebAssembly.Instance(module,{env,'GOT.func':gotProxy,'GOT.mem':gotProxy});
    for(let i=0;i<block.capacity;i++)state.owners.set(block.base+i,handle);
    for(let i=0;i<meta.tableSize;i++)setWasmTableEntry(block.base+i,wasmTable.get(block.base+i));
    for(const [name,index]of entries)setWasmTableEntry(index,instance.exports[name]);
    for(const [name,global] of got) {
      if(entries.has(name))global.value=entries.get(name);
      else {
        const value=resolve(name);
        if(typeof value==='function') {
          let address=state.rootSlots.get(value);
          if(address===undefined){address=getFunctionAddress(value);if(!address)address=addFunction(value);state.rootSlots.set(value,address);}
          global.value=address;
        } else if(value instanceof WebAssembly.Global)global.value=value.value+(instance.exports[name]===value?memoryBase:0);
        else if(typeof value==='number')global.value=value;
        else throw new Error('Unsupported GOT symbol: '+name);
      }
    }
    if(!external){instance.exports.__wasm_apply_data_relocs?.();instance.exports.__wasm_call_ctors?.();}
    state.liveBytes+=meta.memorySize;
    state.programs.set(handle,{module,instance,entries,block,raw,memoryBase,bytes:meta.memorySize});
    return handle;
  } catch(error) {clear();Module.ownedLastError=String(error);return 0;}
});
EM_JS(void, owned_describe,(unsigned handle,uint32_t *out),{const p=Module.ownedState.programs.get(handle);new Uint32Array(wasmMemory.buffer,out>>>0,3).set([p.memoryBase,p.block.base,p.block.capacity]);});
EM_JS(uintptr_t, owned_symbol, (unsigned handle,const char *name), {
  const state=Module.ownedState,index=state.programs.get(handle)?.entries.get(UTF8ToString(name));
  return index && state.owners.get(index)===handle ? index : 0;
});
EM_JS(int, owned_retire, (unsigned handle), {
  const state=Module.ownedState,p=state.programs.get(handle);if(!p)return 0;
  for(let i=0;i<p.block.capacity;i++){const slot=p.block.base+i;if(state.owners.get(slot)===handle){setWasmTableEntry(slot,null);state.owners.delete(slot);}}
  if(p.raw)_free(p.raw);
  p.block.used=false;state.liveBytes-=p.bytes;state.programs.delete(handle);return 1;
});
EM_JS(int, owned_clean, (), {
  const s=Module.ownedState;return s.programs.size===0 && s.liveBytes===0 && s.blocks.every(b=>!b.used);
});
// clang-format on
