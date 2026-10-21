/* SPDX-License-Identifier: GPL-2.0-or-later */
if (typeof window !== 'undefined') {
  (() => {
    let context;
    let loaded;
    const devices = new Map();
    globalThis.blenderWebAudioContext = () => context ||= new AudioContext({latencyHint:'interactive'});
    const resume = () => {
      if (context?.state === 'suspended') context.resume().catch(console.warn);
    };
    window.addEventListener('pointerdown', resume, {capture:true});
    window.addEventListener('keydown', resume, {capture:true});
    globalThis.blenderWebAudioAttach = async (buffer, pointer, capacity) => {
      pointer >>>= 0;
      const device = {control:new Int32Array(buffer, pointer, 9), node:null, closed:false, initializing:true};
      devices.set(pointer, device);
      try {
        const audio = globalThis.blenderWebAudioContext();
        loaded ||= audio.audioWorklet.addModule(Module['locateFile']('audio-worklet.js'));
        await loaded;
        if (device.closed) return;
        device.node = new AudioWorkletNode(audio, 'blender-audaspace', {
          numberOfInputs:0, numberOfOutputs:1, outputChannelCount:[2],
          processorOptions:{buffer, pointer, capacity},
        });
        device.node.port.onmessage = event => {
          if (event.data === 'closed') {
            device.node.disconnect();
            devices.delete(pointer);
            Module._blender_web_audio_release(pointer);
          }
        };
        device.node.connect(audio.destination);
      }
      catch (error) {
        Atomics.store(device.control, 2, -1);
        console.error('Blender audio device failed', error);
      }
      finally {
        device.initializing = false;
        if (device.closed && !device.node) {
          devices.delete(pointer);
          Module._blender_web_audio_release(pointer);
        }
      }
    };
    globalThis.blenderWebAudioDetach = pointer => {
      pointer >>>= 0;
      const device = devices.get(pointer);
      if (!device) return;
      device.closed = true;
      if (device.node) device.node.port.postMessage('close');
      else if (!device.initializing) {
        devices.delete(pointer);
        Module._blender_web_audio_release(pointer);
      }
    };
    globalThis.blenderWebAudioStats = () => ({
      state:context?.state || 'uninitialized', sampleRate:context?.sampleRate,
      baseLatency:context?.baseLatency, outputLatency:context?.outputLatency,
      devices:[...devices.values()].map(({control}) => ({
        queuedFrames:(Atomics.load(control, 1) - Atomics.load(control, 0)) >>> 0,
        ready:Atomics.load(control, 2), underruns:Atomics.load(control, 3),
        consumedFrames:Atomics.load(control, 4), peak:Atomics.load(control, 5) / 1000000,
        nonzeroFrames:Atomics.load(control, 6),
      })),
    });
  })();
}
