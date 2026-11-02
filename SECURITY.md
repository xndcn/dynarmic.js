# Security policy

dynarmic.js is an experimental ARM-to-WebAssembly compiler. Security fixes are
made on the default branch; there are no maintained release branches yet.

## Reporting a vulnerability

Use [GitHub private vulnerability reporting](https://github.com/xndcn/dynarmic.js/security/advisories/new)
when it is available. If GitHub does not offer a private reporting form, contact
the maintainer through a private contact method listed on
[their profile](https://github.com/xndcn). Do not put exploit details or sensitive
guest data in a public issue while arranging a private report.

Include the affected commit, Emscripten and engine versions, guest architecture,
memory mode, a minimal reproducer, and the expected and observed behavior.
For generated-code failures, include the emitted module if it can be shared
safely. Maintainers will investigate and coordinate a fix and disclosure; no
response-time guarantee is currently offered.

## Trust boundaries

WebAssembly bounds checks protect the engine's linear memory boundary, not
individual C++ objects or the guest RAM allocation within that memory. Host
callbacks, page tables and flat-memory mappings must enforce their own bounds.
The example host is a bare-metal demonstration, not a sandbox for hostile code.

Run guests in a dedicated Worker with an execution timeout, and terminate the
Worker when the limit is exceeded. Do not expose filesystem, network, or other
privileged callbacks to untrusted guests. Node test hosts use `NODERAWFS` for
local test files and must not be deployed as a service accepting arbitrary code.
See [the embedding guide](docs/Embedding.md) for callback and memory contracts.
