import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {writeFileSync} from 'node:fs';

export function createBenchmarkEnvironment() {
  return {
    PATH: '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
    LC_ALL: 'C',
    NODE_OPTIONS: '',
  };
}

export function parseCollectArguments(args) {
  assert(args.length >= 4 && args.length <= 6,
    'usage: collect.mjs LINUX_MANIFEST DYNARMIC_HOST A64_GUEST NEW_OUTPUT_DIR [RUNS=5] [WASM_BUILD_DIR]');
  const runs = args[4] === undefined ? 5 : Number(args[4]);
  assert(Number.isInteger(runs) && runs >= 1 && runs <= 20, 'invalid run count');
  return {runs, wasmBuildDir: args[5] ?? null};
}

const prohibitedWasmMacros = [
  'DYNARMIC_JS_A64_REGION_FOCUS',
  'DYNARMIC_WASM_PROFILE',
  'DYNARMIC_WASM_REGION_DIAGNOSTICS',
];

function splitCompilerFlags(flags, key) {
  const tokens = [];
  let token = '';
  let quote = null;
  let escaped = false;
  let started = false;
  for (const character of flags) {
    if (escaped) {
      token += character;
      escaped = false;
    } else if (character === '\\' && quote !== "'") {
      escaped = true;
    } else if (quote !== null) {
      if (character === quote) quote = null;
      else token += character;
    } else if (character === '"' || character === "'") {
      quote = character;
    } else if (/\s/.test(character)) {
      if (started) tokens.push(token);
      token = '';
      started = false;
      continue;
    } else {
      token += character;
    }
    started = true;
  }
  assert(quote === null && !escaped, `invalid Wasm ${key} quoting`);
  if (started) tokens.push(token);
  return tokens;
}

export function validateWasmProductionFlags(build) {
  for (const key of ['cxxFlags', 'configCxxFlags']) {
    const tokens = splitCompilerFlags(build[key], key);
    for (let index = 0; index < tokens.length; ++index) {
      const token = tokens[index];
      assert(!token.startsWith('@') && !['-Xclang', '-Xpreprocessor'].includes(token) &&
        !token.startsWith('-Wp,') && !/^-(?:include|imacros)/.test(token) &&
        !/^--(?:include|imacros|config)(?:=|$)/.test(token) && !/^-config(?:=|$)/.test(token),
      'Wasm production flags must not use indirect compiler input');
      const separate = ['-D', '/D', '--define-macro'].includes(token);
      const attached = separate ? null : /^(?:-D|\/D|--define-macro=)(.*)$/.exec(token);
      if (!separate && !attached) continue;
      const definition = separate ? tokens[index + 1] || '' : attached[1];
      for (const macro of prohibitedWasmMacros) {
        assert(!new RegExp(`^${macro}(?![A-Za-z0-9_])`).test(definition),
          `Wasm production flags must not define ${macro}`);
      }
    }
  }
  return true;
}

export function validateWasmBuildEnvironment(buildEnvironment) {
  assert.deepEqual(buildEnvironment, {configureEmccCflags: '', buildEmccCflags: ''},
    'Wasm configure and build EMCC_CFLAGS must be empty');
  return true;
}

export function validateWasmProductionBuild(info, features, source) {
  assert.equal(info.build.type, 'Release', 'Wasm host must be Release');
  assert.equal(info.build.profile, false, 'Wasm profiling must be OFF');
  assert.equal(info.build.a64RegionFocus, '', 'Wasm region focus must be empty');
  validateWasmProductionFlags(info.build);
  assert.equal(features.regionDiagnostics, false, 'Wasm region diagnostics must be OFF');
  assert.equal(features.focusTestExceptions, false, 'Wasm focus test exceptions must be OFF');
  assert.equal(features.examples, true, 'Wasm browser examples must be ON');
  assert.equal(source.dirty, false, 'Wasm source must be clean');
  return true;
}

export function validateWasmBuildManifest(manifest, identity) {
  validateWasmBuildEnvironment(manifest.buildEnvironment);
  assert.deepEqual(manifest, {
    schema: 'dynarmic.js/wasm-coremark-host-build-v1',
    sourceRoot: identity.info.sourceRoot,
    build: identity.info.build,
    configuration: identity.features,
    buildEnvironment: manifest.buildEnvironment,
    source: identity.source,
    artifacts: identity.artifacts,
    buildSources: identity.buildSources,
  }, 'Wasm post-link manifest differs from current build, source, or artifacts');
  return true;
}

export function runWithFailureEvidence(output, body) {
  try {
    return body();
  } catch (error) {
    writeFileSync(`${output}/failure.json`, `${JSON.stringify({
      failedAt: new Date().toISOString(), failure: error.stack,
    }, null, 2)}\n`, {flag: 'wx'});
    throw error;
  }
}

export function streamIdentity(bytes) {
  return {sha256: createHash('sha256').update(bytes).digest('hex'), bytes: bytes.length};
}
