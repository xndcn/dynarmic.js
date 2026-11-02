import fs from 'node:fs';

const [qemuPath, dynarmicPath, outputPath] = process.argv.slice(2);
if (!qemuPath || !dynarmicPath || !outputPath) {
  throw new Error('usage: node analyze.mjs QEMU_JSON DYNARMIC_PARTIAL_JSON OUTPUT_JSON');
}
if (fs.existsSync(outputPath)) throw new Error(`refusing to overwrite ${outputPath}`);
const qemu = JSON.parse(fs.readFileSync(qemuPath, 'utf8'));
const dynarmic = JSON.parse(fs.readFileSync(dynarmicPath, 'utf8'));
const serial = qemu.serial;
const number = (pattern) => {
  const match = pattern.exec(serial);
  if (!match) throw new Error(`missing serial field: ${pattern}`);
  return Number(match[1]);
};
const hex = (pattern) => {
  const match = pattern.exec(serial);
  if (!match) throw new Error(`missing serial field: ${pattern}`);
  return Number.parseInt(match[1], 16);
};
const durationSeconds = number(/^Total time \(secs\):\s*([0-9.]+)$/m);
const iterations = number(/^Iterations\s*:\s*([0-9]+)$/m);
const crcfinal = hex(/^\[0\]crcfinal\s*:\s*0x([0-9a-f]+)$/m);
const qemuCoreMarkPerSecond = number(/^Iterations\/Sec\s*:\s*([0-9.]+)$/m);
const calculatedCoreMarkPerSecond = iterations / durationSeconds;
if (Math.abs(qemuCoreMarkPerSecond - calculatedCoreMarkPerSecond) > 0.000001) {
  throw new Error('qemu-wasm throughput does not match iterations/duration');
}
const qemuExecutionValid = crcfinal === 0x4983 && iterations === 200000 &&
  durationSeconds >= 10 && serial.includes('Correct operation validated.');
const dynarmicReferences = {
  baseline: {kernelMedianMs: 60415.299, coreMarkPerSecond: 200000000 / 60415.299},
  candidate: {kernelMedianMs: 59844.082, coreMarkPerSecond: 200000000 / 59844.082},
};
const result = {
  schema: 'dynarmic.js/qemu-wasm-coremark-comparison-v1',
  qemuSource: {
    repository: 'https://github.com/ktock/qemu-wasm',
    revision: '0ef7b4e2814b231705d8371dd7997f5b72e70baf',
    runtime: 'official prebuilt raspi3ap runtime; QEMU emulator version 8.2.0',
  },
  workload: {
    architecture: 'aarch64',
    iterations,
    expectedCrcfinal: 0x4983,
    guestSha256: qemu.artifacts.guest.sha256,
    adapterSha256: qemu.artifacts.adapter.sha256,
  },
  qemu: {
    runnerStatus: qemu.status,
    runnerFailure: qemu.failure,
    exitCode: qemu.exitCode,
    wallMilliseconds: qemu.wallMilliseconds,
    durationSeconds,
    crcfinal,
    coreMarkPerSecond: qemuCoreMarkPerSecond,
    executionValid: qemuExecutionValid,
  },
  dynarmicReference: {
    source: dynarmicPath,
    completedPairs: dynarmic.completedPairs,
    declaredPairs: 10,
    loadedHostPartialDiagnostic: true,
    sides: dynarmicReferences,
  },
  observedRateRatiosForDiagnosisOnly: Object.fromEntries(
    Object.entries(dynarmicReferences).map(([side, value]) => [
      side, value.coreMarkPerSecond / qemuCoreMarkPerSecond,
    ]),
  ),
  performanceComparisonProduced: false,
  formalControlledComparison: false,
  reason: qemuExecutionValid
    ? 'The dynarmic reference is a partial loaded-host diagnostic, not a controlled paired comparison.'
    : 'qemu-wasm produced an incorrect final CRC; its observed execution rate is not a valid CoreMark performance result.',
};
fs.writeFileSync(outputPath, `${JSON.stringify(result, null, 2)}\n`, {flag: 'wx'});
