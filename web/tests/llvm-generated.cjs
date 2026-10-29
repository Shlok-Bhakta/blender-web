/* SPDX-License-Identifier: GPL-2.0-or-later */
const assert = require('node:assert/strict');

async function checkGenerated(bytes, expectedConstant = 42) {
  const assert = {equal(actual, expected) {
    if (!Object.is(actual, expected)) throw new Error(`Generated code: ${actual} != ${expected}`);
  }};
  const {instance} = await WebAssembly.instantiate(bytes, {
    env: {host_scale: x => Math.fround(x * 0.5)},
  });
  const {folded, mix, wide, shade, vector4, memory} = instance.exports;
  assert.equal(folded(), expectedConstant);
  const floats = new Float32Array(memory.buffer), input = 80000, output = 100000;
  let random = 0x12345678;
  const count = 4096;
  for (let i = 0; i < count; i++) {
    random = (Math.imul(random, 1664525) + 1013904223) | 0;
    assert.equal(mix(random), (Math.imul(random, 1664525) + 1013904223) ^
      ((Math.imul(random, 1664525) + 1013904223) >>> 13));
    const integer = BigInt(random) * 0x100000001n;
    assert.equal(wide(integer), BigInt.asIntN(64,
      integer * 6364136223846793005n + 1442695040888963407n));
    floats[input / 4 + i] = random / 2097152;
    floats[output / 4 + i] = -123;
  }
  shade(output, input, 0);
  assert.equal(floats[output / 4], -123);
  shade(output, input, count);
  for (let i = 0; i < count; i++) {
    const x = floats[input / 4 + i];
    assert.equal(floats[output / 4 + i], Math.fround(
      Math.fround(Math.fround(x * x) * 0.5) + 0.25));
  }
  for (let i = 0; i < count; i += 4) vector4(output + i * 4, input + i * 4);
  for (let i = 0; i < count; i++) {
    assert.equal(floats[output / 4 + i], Math.fround(
      Math.fround(floats[input / 4 + i] * 0.5) + 0.25));
  }
  return {integerCases: count, wideIntegerCases: count, shaderValues: count,
    simdValues: count, emptyLoop: true, hostFunctionImport: true, constantFold: true};
}
module.exports = {checkGenerated};

if (require.main === module) (async () => {
  const path = require('node:path'), fs = require('node:fs');
  const directory = path.resolve(process.argv[2]);
  const create = require(path.join(directory, 'llvm-compiler.js'));
  const lines = [];
  const compiler = await create({noInitialRun: true, print: line => lines.push(line)});
  compiler.callMain([]);
  const report = JSON.parse(lines.find(line => line.startsWith('{')));
  assert.equal(report.llvmVersion, '20.1.8');
  assert.equal(report.compiledModules, 3);
  assert(report.invalidIRRejected && report.invalidLinkRecovered);
  const generated = [];
  for (let i = 0; i < 3; i++) {
    const bytes = compiler.FS.readFile(`/shader-${i}.wasm`);
    const ir = new TextDecoder().decode(compiler.FS.readFile(`/shader-${i}.ll`));
    assert(ir.includes(`ret i32 ${42 + i}`));
    generated.push({...await checkGenerated(bytes, 42 + i), bytes: bytes.length});
    fs.writeFileSync(path.join(directory, `generated-shader-${i}.wasm`), bytes);
  }
  console.log(JSON.stringify({...report, generated}));
  compiler.PThread.terminateAllThreads();
})().catch(error => { console.error(error); process.exit(1); });
