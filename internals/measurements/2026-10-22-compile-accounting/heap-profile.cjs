const inspector = require('node:inspector');
const fs = require('node:fs');
const path = require('node:path');
const [hostArg, output] = process.argv.slice(2);
const host = path.resolve(hostArg);
const session = new inspector.Session(); session.connect();
session.post('HeapProfiler.startSampling', {samplingInterval: 128,
  includeObjectsCollectedByMajorGC: true, includeObjectsCollectedByMinorGC: true}, error => {
  if (error) throw error;
  process.argv = [process.execPath, host, '0', '64'];
  require(host);
});
process.once('beforeExit', () => session.post('HeapProfiler.stopSampling', (error, result) => {
  if (error) throw error;
  fs.writeFileSync(output, JSON.stringify(result.profile));
  session.disconnect();
}));
