// SPDX-License-Identifier: GPL-2.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path');

async function run(directory, expected) {
  const lines = [];
  const create = require(path.resolve(directory, 'osl-runtime.js'));
  const runtime = await create({noInitialRun: true, print: line => lines.push(line)});
  try {
    assert.equal(runtime.callMain(['control']), 0);
    const result = JSON.parse(lines.find(line => line.startsWith('{')));
    assert.deepEqual(result, {registrationAccepted: expected, publicStringBytes: 4, hashBytes: 8});
    return result;
  } finally {
    runtime.PThread.terminateAllThreads();
  }
}
(async () => {
  const report = {original: await run(process.argv[2], false),
    corrected: await run(process.argv[3], true)};
  fs.writeFileSync(process.argv[4], JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report));
})().catch(error => {console.error(error); process.exit(1);});
