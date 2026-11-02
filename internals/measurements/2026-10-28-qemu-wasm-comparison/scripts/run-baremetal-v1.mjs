import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {performance} from 'node:perf_hooks';
import {fileURLToPath} from 'node:url';

const scriptsDir = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(scriptsDir, '..');
const runtimeDir = path.join(root, 'downloads', 'raspi3ap');
const outputPath = process.argv[2];
if (!outputPath) {
  throw new Error('usage: node run-baremetal.mjs OUTPUT_JSON');
}
if (fs.existsSync(outputPath)) {
  throw new Error(`refusing to overwrite ${outputPath}`);
}

const sha256 = (data) => crypto.createHash('sha256').update(data).digest('hex');
const readArtifact = (file) => {
  const data = fs.readFileSync(file);
  return {data, sha256: sha256(data), bytes: data.length};
};
const runtime = {
  loader: readArtifact(path.join(runtimeDir, 'out.js')),
  wasm: readArtifact(path.join(runtimeDir, 'qemu-system-aarch64.wasm')),
  worker: readArtifact(path.join(runtimeDir, 'qemu-system-aarch64.worker.js')),
};
const guest = readArtifact('/tmp/dynarmic-coremark-long/a64.elf');
const adapter = readArtifact(path.join(root, 'guest-adapter.elf'));
const qemuArguments = [
  '-nic', 'none',
  '-M', 'raspi3ap',
  '-display', 'none',
  '-serial', 'stdio',
  '-m', '512M',
  '-accel', 'tcg,tb-size=500',
  '-smp', '4',
  '-semihosting',
  '-device', 'loader,file=/pack/a64.elf',
  '-device', 'loader,file=/pack/adapter.elf,cpu-num=0',
];
const startedAt = new Date().toISOString();
const started = performance.now();
const decoder = new TextDecoder();
let serial = '';
let diagnostics = '';
let finished = false;
let timeout;

const finish = (status, exitCode, failure = null) => {
  if (finished) return;
  finished = true;
  clearTimeout(timeout);
  const result = {
    schema: 'dynarmic.js/qemu-wasm-baremetal-coremark-run-v1',
    status,
    startedAt,
    finishedAt: new Date().toISOString(),
    wallMilliseconds: performance.now() - started,
    exitCode,
    failure,
    node: process.version,
    platform: process.platform,
    arch: process.arch,
    arguments: qemuArguments,
    artifacts: {
      guest: {path: '/tmp/dynarmic-coremark-long/a64.elf', sha256: guest.sha256, bytes: guest.bytes},
      adapter: {sha256: adapter.sha256, bytes: adapter.bytes},
      qemuLoader: {sha256: runtime.loader.sha256, bytes: runtime.loader.bytes},
      qemuWasm: {sha256: runtime.wasm.sha256, bytes: runtime.wasm.bytes},
      qemuWorker: {sha256: runtime.worker.sha256, bytes: runtime.worker.bytes},
    },
    serial,
    diagnostics,
  };
  fs.writeFileSync(outputPath, `${JSON.stringify(result, null, 2)}\n`, {flag: 'wx'});
  process.exitCode = status === 'completed' ? 0 : 1;
};

const fail = (error) => {
  const message = error instanceof Error ? `${error.name}: ${error.message}\n${error.stack}` : String(error);
  finish('failed', null, message);
};
process.on('uncaughtException', fail);
process.on('unhandledRejection', fail);

timeout = setTimeout(() => {
  finish('timeout', null, 'timeout after 900000 ms');
  process.exit(124);
}, 900000);

const pty = {
  readable: false,
  writable: true,
  onReadable() {
    return {dispose() {}};
  },
  onSignal() {
    return {dispose() {}};
  },
  ioctl(command) {
    if (command === 'TCGETS') {
      return {iflag: 0, oflag: 0, cflag: 0, lflag: 0, cc: Array(20).fill(0)};
    }
    if (command === 'TCSETS') return 0;
    if (command === 'TIOCGWINSZ') return [80, 24];
    throw new Error(`unsupported PTY ioctl: ${command}`);
  },
  read() {
    return [];
  },
  write(values) {
    const text = decoder.decode(Uint8Array.from(values), {stream: true});
    serial += text;
    process.stdout.write(text);
  },
};

try {
  const {default: createQemu} = await import(path.join(runtimeDir, 'out.mjs'));
  const moduleConfig = {
    arguments: qemuArguments,
    mainScriptUrlOrBlob: path.join(runtimeDir, 'out.mjs'),
    pty,
    preRun() {
      moduleConfig.FS_createPath('/', 'pack', true, true);
      moduleConfig.FS_createDataFile('/pack', 'a64.elf', guest.data, true, true, true);
      moduleConfig.FS_createDataFile('/pack', 'adapter.elf', adapter.data, true, true, true);
    },
    print(text) {
      serial += `${text}\n`;
      process.stdout.write(`${text}\n`);
    },
    printErr(text) {
      diagnostics += `${text}\n`;
      process.stderr.write(`${text}\n`);
    },
    onAbort(reason) {
      finish('failed', null, `QEMU abort: ${reason}`);
    },
    onExit(code) {
      const crcValid = serial.includes('crcfinal      : 0x4983');
      const correct = serial.includes('Correct operation validated.');
      const duration = /^Total time \(secs\):\s*([0-9.]+)$/m.exec(serial);
      const validDuration = duration !== null && Number(duration[1]) >= 10;
      const valid = crcValid && correct && validDuration;
      finish(valid ? 'completed' : 'failed', code,
        valid ? null : `invalid CoreMark evidence: crc=${crcValid}, correct=${correct}, duration=${validDuration}`);
    },
  };
  await createQemu(moduleConfig);
} catch (error) {
  fail(error);
}
