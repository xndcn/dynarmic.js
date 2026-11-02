// Profile exactly one complete CoreMark Run with generated guest-PC names.
// The normal COREMARK_SAMPLE record is retained for workload validation.
import {pathToFileURL} from 'node:url';
import {collectCoremarkSample, emitCoremarkSample} from './sample.mjs';

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [jit, binary, architecture, memoryMode, output, ...extra] = process.argv.slice(2);
  if (!output || extra.length) {
    throw new Error('usage: profile.mjs <arm_jit.js> <binary> <a32|a64> <0|1|2> <new.cpuprofile>');
  }
  emitCoremarkSample(await collectCoremarkSample(jit, binary, architecture, memoryMode, output));
}
