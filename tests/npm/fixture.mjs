import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';

export function install(tarball) {
  tarball = path.resolve(tarball);
  const checksum = () => createHash('sha256').update(fs.readFileSync(tarball)).digest('hex');
  const tarballSha256 = checksum();
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-npm-consumer-'));
  try {
    fs.writeFileSync(path.join(root, 'package.json'), JSON.stringify({private: true, type: 'module'}));
    execFileSync('npm', ['install', '--ignore-scripts', '--no-audit', '--no-fund', path.resolve(tarball)],
      {cwd: root, encoding: 'utf8', timeout: 120000, stdio: 'pipe'});
    for (const name of ['cases.mjs', 'consumer.mjs'])
      fs.copyFileSync(fileURLToPath(new URL(name, import.meta.url)), path.join(root, name));
    return {root, tarballSha256, cleanup: () => {
      fs.rmSync(root, {recursive: true, force: true});
      if (checksum() !== tarballSha256) throw new Error('The package tarball changed during consumer validation');
    }};
  } catch (error) { fs.rmSync(root, {recursive: true, force: true}); throw error; }
}
