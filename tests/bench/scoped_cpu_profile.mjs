// Capture one synchronous or asynchronous operation with V8's CPU profiler.
// Diagnostic tooling only: sampling changes timing and is not a benchmark.
import fs from 'node:fs';
import inspector from 'node:inspector';

let captureActive = false;

function post(session, method, params = {}) {
  return new Promise((resolve, reject) => {
    session.post(method, params, (error, result) => {
      if (error) reject(error);
      else resolve(result);
    });
  });
}

async function assertNewDestination(output) {
  try {
    await fs.promises.lstat(output);
  } catch (error) {
    if (error.code === 'ENOENT') return;
    throw error;
  }
  const error = new Error(`profile output already exists: ${output}`);
  error.code = 'EEXIST';
  throw error;
}

export async function captureCpuProfile(output, callback, {interval = 100} = {}) {
  if (typeof output !== 'string' || !output) throw new TypeError('profile output must be a nonempty path');
  if (typeof callback !== 'function') throw new TypeError('profile callback must be a function');
  if (!Number.isSafeInteger(interval) || interval <= 0) {
    throw new RangeError('sampling interval must be a positive safe integer');
  }
  if (captureActive) throw new Error('a CPU profile capture is already active');
  captureActive = true;
  try {
    await assertNewDestination(output);
    const session = new inspector.Session();
    let connected = false, started = false, primaryFailed = false, cleanupFailed = false;
    let value, primaryError, cleanupError;
    try {
      session.connect();
      connected = true;
      await post(session, 'Profiler.enable');
      await post(session, 'Profiler.setSamplingInterval', {interval});
      await post(session, 'Profiler.start');
      started = true;
      value = await callback();
    } catch (error) {
      primaryFailed = true;
      primaryError = error;
    }
    if (started) {
      try {
        const {profile} = await post(session, 'Profiler.stop');
        await fs.promises.writeFile(output, JSON.stringify(profile), {flag: 'wx'});
      } catch (error) {
        cleanupFailed = true;
        cleanupError = error;
      }
    }
    if (connected) {
      try {
        session.disconnect();
      } catch (error) {
        cleanupError = cleanupFailed
          ? new AggregateError([cleanupError, error], 'profile cleanup and disconnect failed') : error;
        cleanupFailed = true;
      }
    }
    if (primaryFailed && cleanupFailed) {
      throw new AggregateError([primaryError, cleanupError], 'profile callback and cleanup failed');
    }
    if (primaryFailed) throw primaryError;
    if (cleanupFailed) throw cleanupError;
    return value;
  } finally {
    captureActive = false;
  }
}
