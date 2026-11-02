# Installed-package acceptance

These tests consume the actual `.tgz` produced by
[the release procedure](../../docs/Releasing.md#npm-package). Each runtime check
installs it in a fresh temporary project with npm scripts disabled and cleans up
that project afterward. No package registry publication is performed.

```sh
npm ci --prefix tests/npm --ignore-scripts --no-audit --no-fund
node tests/npm/smoke.mjs build-npm/dynarmic.js-0.1.0-alpha.0.tgz --report test-results/npm-node.json
node tests/npm/verify_browser.mjs build-npm/dynarmic.js-0.1.0-alpha.0.tgz /path/to/chrome --report test-results/npm-chromium.json
node tests/npm/verify_firefox.mjs build-npm/dynarmic.js-0.1.0-alpha.0.tgz /path/to/firefox --report test-results/npm-firefox.json
node tests/npm/verify_tooling.mjs build-npm/dynarmic.js-0.1.0-alpha.0.tgz /path/to/chrome --report test-results/npm-tooling.json
```

Create the report directory first. Use Node 22 and 24 for the runtime smoke.
Tooling dependencies are exact versions in a committed lockfile and are only
used by maintainers; the published package has zero runtime dependencies.

`cases.mjs` executes ARM, Thumb and A64 machine code with independent known
answers. It covers 64-bit values, stepping, SVC/resume, budget interrupts,
RAM boundaries and guest faults, code invalidation, instance isolation and
disposal. Node additionally checks Wasm file URLs, supplied bytes, failed loads
and memory growth. Browser checks serve a nested URL path and use actual module
Workers with default/relocated/supplied Wasm assets.

`verify_tooling.mjs` runs strict publint and ATTW's ESM-only profile against the
package, strict TypeScript NodeNext and Bundler consumers (including
expected type errors), CommonJS dynamic import, then builds and executes a Vite
production module Worker with an emitted Wasm asset under a nested base path.
ATTW excludes the binary `runtime.wasm` export from JavaScript type resolution;
the Node resolver and real Vite asset import/fetch verify that export instead.
Passing metadata checks never replaces real runtime execution.

For source completeness, extract the bundled `source.tar.gz` without Git or
files from the original checkout, follow `REBUILD.md`, and run the archived API
and `cases.mjs` against its newly linked runtime. The package workflow automates
this and retains reports with the tarball. Hash verification and negative
packaging tests guard against accidentally shipping stale or changed artifacts.
