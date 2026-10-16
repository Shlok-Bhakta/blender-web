/* SPDX-License-Identifier: GPL-2.0-or-later */
// A renderer for the GHOST integration test. This is not Blender's GPUBackend.
const shader = `
struct Uniforms { mvp: mat4x4<f32> }
@group(0) @binding(0) var<uniform> u: Uniforms;
struct Output { @builtin(position) position: vec4f, @location(0) color: vec3f }
@vertex fn vertex(@location(0) position: vec3f, @location(1) normal: vec3f) -> Output {
  var out: Output;
  out.position = u.mvp * vec4f(position, 1.0);
  let light = max(dot(normal, normalize(vec3f(0.4, 0.6, 0.8))), 0.0);
  out.color = vec3f(0.56, 0.58, 0.61) * (0.4 + light * 0.6);
  return out;
}
@fragment fn fragment(in: Output) -> @location(0) vec4f { return vec4f(in.color, 1.0); }
`;

export async function createRenderer(canvas, fail) {
  if (!navigator.gpu) throw new Error('WebGPU is unavailable. Use a browser and device with WebGPU enabled. Safari requires iOS/iPadOS 26 or newer.');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('The browser could not find a WebGPU adapter.');
  // Request only baseline capabilities. Do not assume desktop limits on iOS.
  const device = await adapter.requestDevice();
  device.lost.then(info => fail(new Error(`GPU device lost: ${info.message || info.reason}. Reload to reconnect.`)));
  device.addEventListener('uncapturederror', event => fail(event.error));
  const context = canvas.getContext('webgpu');
  if (!context) throw new Error('The browser could not create a WebGPU canvas context.');
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: 'opaque' });
  const module = device.createShaderModule({ code: shader });
  const messages = await module.getCompilationInfo();
  const errors = messages.messages.filter(message => message.type === 'error');
  if (errors.length) throw new Error(errors.map(message => message.message).join('\n'));
  const pipeline = await device.createRenderPipelineAsync({
    layout: 'auto',
    vertex: { module, entryPoint: 'vertex', buffers: [{ arrayStride: 24, attributes: [
      { shaderLocation: 0, offset: 0, format: 'float32x3' },
      { shaderLocation: 1, offset: 12, format: 'float32x3' },
    ] }] },
    fragment: { module, entryPoint: 'fragment', targets: [{ format }] },
    primitive: { topology: 'triangle-list', cullMode: 'back' },
    depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' },
  });
  const vertices = [];
  const faces = [
    [[0,0,1], [[-1,-1,1],[1,-1,1],[1,1,1],[-1,1,1]]],
    [[0,0,-1], [[1,-1,-1],[-1,-1,-1],[-1,1,-1],[1,1,-1]]],
    [[1,0,0], [[1,-1,1],[1,-1,-1],[1,1,-1],[1,1,1]]],
    [[-1,0,0], [[-1,-1,-1],[-1,-1,1],[-1,1,1],[-1,1,-1]]],
    [[0,1,0], [[-1,1,1],[1,1,1],[1,1,-1],[-1,1,-1]]],
    [[0,-1,0], [[-1,-1,-1],[1,-1,-1],[1,-1,1],[-1,-1,1]]],
  ];
  for (const [normal, points] of faces) {
    for (const index of [0,1,2,0,2,3]) vertices.push(...points[index], ...normal);
  }
  const vertexData = new Float32Array(vertices);
  const vertexBuffer = device.createBuffer({ size: vertexData.byteLength, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  device.queue.writeBuffer(vertexBuffer, 0, vertexData);
  const uniforms = device.createBuffer({ size: 64, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const bindGroup = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: uniforms } }] });
  let depth, depthWidth = 0, depthHeight = 0;
  return {
    maxDimension: device.limits.maxTextureDimension2D,
    draw(matrix) {
      if (!depth || depthWidth !== canvas.width || depthHeight !== canvas.height) {
        depth?.destroy();
        depth = device.createTexture({ size: [canvas.width, canvas.height], format: 'depth24plus', usage: GPUTextureUsage.RENDER_ATTACHMENT });
        depthWidth = canvas.width;
        depthHeight = canvas.height;
      }
      // Only 64 bytes cross the Wasm/JS boundary per redraw. Geometry stays on the GPU.
      device.queue.writeBuffer(uniforms, 0, matrix);
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginRenderPass({
        colorAttachments: [{ view: context.getCurrentTexture().createView(), clearValue: {r:0.105,g:0.112,b:0.125,a:1}, loadOp: 'clear', storeOp: 'store' }],
        depthStencilAttachment: { view: depth.createView(), depthClearValue: 1, depthLoadOp: 'clear', depthStoreOp: 'discard' },
      });
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, bindGroup);
      pass.setVertexBuffer(0, vertexBuffer);
      pass.draw(36);
      pass.end();
      device.queue.submit([encoder.finish()]);
    },
  };
}
