/* SPDX-License-Identifier: GPL-2.0-or-later */
class BlenderAudaspace extends AudioWorkletProcessor {
  constructor({processorOptions:{buffer, pointer, capacity}}) {
    super();
    this.control = new Int32Array(buffer, pointer, 9);
    this.samples = new Float32Array(buffer, pointer + 36, capacity * 2);
    this.capacity = capacity;
    this.closing = false;
    this.primed = false;
    this.port.onmessage = event => { if (event.data === 'close') this.close(); };
  }
  close() {
    if (this.closing) return;
    this.closing = true;
    this.port.postMessage('closed');
  }
  process(inputs, outputs) {
    if (this.closing) return false;
    if (Atomics.load(this.control, 7)) {
      this.close();
      return false;
    }
    Atomics.store(this.control, 2, 1);
    const output = outputs[0];
    const length = output[0].length;
    const read = Atomics.load(this.control, 0) >>> 0;
    const write = Atomics.load(this.control, 1) >>> 0;
    if (!this.primed) {
      const available = (write - read) >>> 0;
      if (available === 0 || (available < 1024 && Atomics.load(this.control, 8))) return true;
      this.primed = true;
    }
    const count = Math.min(length, (write - read) >>> 0);
    let peak = 0, nonzero = 0;
    for (let i = 0; i < count; i++) {
      const offset = ((read + i) % this.capacity) * 2;
      const left = this.samples[offset], right = this.samples[offset + 1];
      output[0][i] = left;
      output[1][i] = right;
      peak = Math.max(peak, Math.abs(left), Math.abs(right));
      if (left !== 0 || right !== 0) nonzero++;
    }
    Atomics.store(this.control, 0, (read + count) | 0);
    Atomics.add(this.control, 4, count);
    Atomics.add(this.control, 6, nonzero);
    if (peak * 1000000 > Atomics.load(this.control, 5)) Atomics.store(this.control, 5, Math.round(peak * 1000000));
    if (count < length && Atomics.load(this.control, 8)) Atomics.add(this.control, 3, 1);
    if (count === 0 && !Atomics.load(this.control, 8)) this.primed = false;
    return true;
  }
}
registerProcessor('blender-audaspace', BlenderAudaspace);
