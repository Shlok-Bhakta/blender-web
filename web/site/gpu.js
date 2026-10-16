/* SPDX-License-Identifier: GPL-2.0-or-later */
// Preview renderer. Blender's native GPUBackend will replace this drawing path.
const shader = `
struct Uniforms { mvp: mat4x4<f32>, normal: mat4x4<f32>, tint: vec4f }
@group(0) @binding(0) var<uniform> u: Uniforms;
struct Output { @builtin(position) position: vec4f, @location(0) color: vec3f }
@vertex fn solid(@location(0) position: vec3f, @location(1) normal: vec3f) -> Output {
  var out: Output;
  out.position = u.mvp * vec4f(position, 1.0);
  let world_normal = (u.normal * vec4f(normal, 0.0)).xyz;
  let n = world_normal / max(length(world_normal), 0.00001);
  let light = max(dot(n, normalize(vec3f(0.4, -0.6, 0.8))), 0.0);
  out.color = vec3f(0.62, 0.63, 0.65) * (0.45 + light * 0.55);
  return out;
}
@vertex fn line(@location(0) position: vec3f, @location(1) color: vec3f) -> Output {
  var out: Output;
  out.position = u.mvp * vec4f(position, 1.0);
  out.color = color * u.tint.rgb;
  return out;
}
@fragment fn fragment(in: Output) -> @location(0) vec4f { return vec4f(in.color, 1.0); }
`;

export async function createRenderer(canvas, fail) {
  if (!navigator.gpu) throw new Error('WebGPU is unavailable. Safari requires iOS/iPadOS 26 or newer with WebGPU support.');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('The browser could not find a WebGPU adapter.');
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
  const bindLayout = device.createBindGroupLayout({ entries: [{ binding: 0,
    visibility: GPUShaderStage.VERTEX, buffer: { type: 'uniform', minBindingSize: 144 } }] });
  const layout = device.createPipelineLayout({ bindGroupLayouts: [bindLayout] });
  const buffers = [{ arrayStride: 24, attributes: [
    { shaderLocation: 0, offset: 0, format: 'float32x3' },
    { shaderLocation: 1, offset: 12, format: 'float32x3' },
  ] }];
  const pipeline = await device.createRenderPipelineAsync({ layout,
    vertex: { module, entryPoint: 'solid', buffers },
    fragment: { module, entryPoint: 'fragment', targets: [{ format }] },
    // Negative scales remain visible, as in the solid Blender viewport.
    primitive: { topology: 'triangle-list', cullMode: 'none' },
    depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' },
  });
  const lines = await device.createRenderPipelineAsync({ layout,
    vertex: { module, entryPoint: 'line', buffers },
    fragment: { module, entryPoint: 'fragment', targets: [{ format }] },
    primitive: { topology: 'line-list' },
    depthStencil: { format: 'depth24plus', depthWriteEnabled: false, depthCompare: 'less-equal' },
  });
  const upload = data => {
    const values = new Float32Array(data);
    const buffer = device.createBuffer({ size: values.byteLength, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(buffer, 0, values);
    return buffer;
  };
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
  const vertexBuffer = upload(vertices);
  const edgeVertices = [];
  for (let axis = 0; axis < 3; ++axis) {
    for (const a of [-1, 1]) for (const b of [-1, 1]) {
      for (const end of [-1, 1]) {
        const point = [0, 0, 0];
        point[axis] = end * 1.002;
        point[(axis + 1) % 3] = a * 1.002;
        point[(axis + 2) % 3] = b * 1.002;
        edgeVertices.push(...point, 1, 1, 1);
      }
    }
  }
  const edgeBuffer = upload(edgeVertices);
  const gridVertices = [];
  for (let i = -20; i <= 20; ++i) {
    const gray = i % 5 === 0 ? 0.27 : 0.19;
    const xColor = i === 0 ? [0.55, 0.14, 0.17] : [gray, gray, gray];
    const yColor = i === 0 ? [0.18, 0.46, 0.23] : [gray, gray, gray];
    // Short segments avoid driver differences when a long line crosses the near plane.
    for (let j = -20; j < 20; ++j) {
      gridVertices.push(j, i, 0, ...xColor, j + 1, i, 0, ...xColor);
      gridVertices.push(i, j, 0, ...yColor, i, j + 1, 0, ...yColor);
    }
  }
  const gridBuffer = upload(gridVertices);
  const makeUniform = () => {
    const buffer = device.createBuffer({ size: 144, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    const values = new Float32Array(36);
    values.set([1, 1, 1, 1], 32);
    return { buffer, values, group: device.createBindGroup({ layout: bindLayout, entries: [{ binding: 0, resource: { buffer } }] }) };
  };
  const object = makeUniform(), grid = makeUniform();
  const selectedTint = new Float32Array([1, 0.48, 0.08, 1]);
  const neutralTint = new Float32Array([0.12, 0.12, 0.12, 1]);
  let depth, depthWidth = 0, depthHeight = 0;
  return {
    maxDimension: device.limits.maxTextureDimension2D,
    draw(matrix, view, normal, state) {
      if (!depth || depthWidth !== canvas.width || depthHeight !== canvas.height) {
        depth?.destroy();
        depth = device.createTexture({ size: [canvas.width, canvas.height], format: 'depth24plus', usage: GPUTextureUsage.RENDER_ATTACHMENT });
        depthWidth = canvas.width; depthHeight = canvas.height;
      }
      // Reuse CPU arrays and GPU resources. Wasm sends matrices only on redraw.
      object.values.set(matrix); object.values.set(normal, 16);
      object.values.set(state.selected ? selectedTint : neutralTint, 32);
      grid.values.set(view);
      device.queue.writeBuffer(object.buffer, 0, object.values);
      device.queue.writeBuffer(grid.buffer, 0, grid.values);
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginRenderPass({
        colorAttachments: [{ view: context.getCurrentTexture().createView(), clearValue: {r:0.105,g:0.108,b:0.115,a:1}, loadOp: 'clear', storeOp: 'store' }],
        depthStencilAttachment: { view: depth.createView(), depthClearValue: 1, depthLoadOp: 'clear', depthStoreOp: 'discard' },
      });
      if (state.visible) {
        pass.setPipeline(pipeline); pass.setBindGroup(0, object.group);
        pass.setVertexBuffer(0, vertexBuffer); pass.draw(36);
      }
      pass.setPipeline(lines);
      if (state.grid) {
        pass.setBindGroup(0, grid.group); pass.setVertexBuffer(0, gridBuffer); pass.draw(gridVertices.length / 6);
      }
      if (state.visible && state.edges) {
        pass.setBindGroup(0, object.group); pass.setVertexBuffer(0, edgeBuffer); pass.draw(24);
      }
      pass.end(); device.queue.submit([encoder.finish()]);
    },
  };
}
