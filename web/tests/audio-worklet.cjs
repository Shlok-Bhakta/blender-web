/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
let Processor;
vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../native/site/audio-worklet.js'), 'utf8'), {
  AudioWorkletProcessor:class { constructor() { this.port = {postMessage:message => this.message = message}; } },
  registerProcessor:(name, processor) => { assert.equal(name, 'blender-audaspace'); Processor = processor; },
  Int32Array, Float32Array, Atomics, Math,
});
const capacity = 8192;
const buffer = new SharedArrayBuffer(36 + capacity * 8);
const control = new Int32Array(buffer, 0, 9);
const samples = new Float32Array(buffer, 36);
const processor = new Processor({processorOptions:{buffer, pointer:0, capacity}});
function produce(start, count) {
  for (let i = 0; i < count; i++) {
    const offset = ((start + i) % capacity) * 2;
    samples[offset] = Math.fround((i % 97) / 97);
    samples[offset + 1] = -samples[offset];
  }
  Atomics.store(control, 1, (start + count) | 0);
}
function consume(length) {
  const channels = [new Float32Array(length), new Float32Array(length)];
  assert.equal(processor.process([], [channels]), true);
  return channels;
}
// Both ring wrap and the 32-bit monotonic sample counter wrap.
const start = 0xfffffe00;
Atomics.store(control, 0, start | 0);
Atomics.store(control, 8, 1);
produce(start, 1536);
let index = 0;
for (const size of [64, 128, 256, 512, 512, 64]) {
  const channels = consume(size);
  for (let i = 0; i < size; i++, index++) {
    const expected = Math.fround((index % 97) / 97);
    assert.equal(channels[0][i], expected);
    assert.equal(channels[1][i], -expected);
  }
}
assert.equal(Atomics.load(control, 0) >>> 0, (start + 1536) >>> 0);
assert.equal(Atomics.load(control, 4), 1536);
assert(consume(128).every(channel => channel.every(value => value === 0)));
assert.equal(Atomics.load(control, 3), 1, 'Count a complete active-playback underrun');
Atomics.store(control, 8, 0);
consume(128);
assert.equal(Atomics.load(control, 3), 1, 'Idle silence is not an underrun');
// A short sound must drain even when smaller than the startup watermark.
const position = Atomics.load(control, 0) >>> 0;
produce(position, 32);
const short = consume(128);
assert.equal(short[0][31], Math.fround(31 / 97));
assert(short[0].slice(32).every(value => value === 0));
Atomics.store(control, 7, 1);
assert.equal(processor.process([], [[new Float32Array(128), new Float32Array(128)]]), false);
assert.equal(processor.message, 'closed');
console.log(JSON.stringify({stereoSamples:1536,variableBlockSizes:true,ringAndCounterWrap:true,
  activeUnderrun:true,idleSilence:true,shortSound:true,closeAcknowledged:true}));
