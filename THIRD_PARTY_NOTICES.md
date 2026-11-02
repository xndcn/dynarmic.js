# Third-party notices

This repository distributes several upstream components alongside the
dynarmic.js WebAssembly backend. The files under each listed path retain their
upstream copyright and license terms. The root [`LICENSE`](LICENSE) applies to
the dynarmic.js project-owned backend and integration code marked
`MIT`; it does not replace the notices below.

The exact upstream files are included in the recursive checkout. This table is
an inventory for release review, not a relicensing statement.

Native installs also copy the licenses for the components linked by the static
library to `share/licenses/dynarmic.js/third-party/`. The Dynarmic, fmt, mcl and
robin-map files are renamed with their component prefix there. The Boost license belongs
to the system Boost installation and is not copied by this project.

| Component | Path | License evidence | Used by the wasm build |
| --- | --- | --- | --- |
| Dynarmic submodule | `externals/dynarmic/` | `externals/dynarmic/LICENSE.txt` (0BSD) | Yes, as an unmodified submodule |
| fmt | `externals/dynarmic/externals/fmt/` | `LICENSE.rst` (MIT, with the fmt exception) | Yes |
| mcl | `externals/dynarmic/externals/mcl/` | `LICENSE` (MIT) | Yes |
| robin-map | `externals/dynarmic/externals/robin-map/` | `LICENSE` (MIT) | Yes |
| Boost headers | system `Boost >= 1.57` | Boost Software License, Version 1.0 | Yes, header-only use |
| QuickJS 2025-09-13 | Downloaded by `examples/quickjs/build.py`; complete source is shipped as `quickjs-source.tar.xz` with the page | Upstream `LICENSE` (MIT), copied to `quickjs-LICENSE.txt` | Separate ARM32 guest and direct Wasm comparison runtime |
| V8 benchmark suite version 7 from QuickJS extras | `official-v8-upstream.js`, generated `official-v8.js`; full extras source in `quickjs-extras-source.tar.xz` | Original per-file copyright and license headers in the complete combined source and archive, including Google, Sun Microsystems and other contributors. DeltaBlue is GPL-2.0-or-later; the complete GPL 2.0 text is distributed as `quickjs-extras-LICENSE.txt` | Official JavaScript benchmark in all three runtimes |
| newlib and GNU libgcc | Linked from the ARM cross toolchain into the QuickJS guest | Full newlib notice inventory and GPL-3.0-or-later with GCC Runtime Library Exception 3.1 in `quickjs-runtime-licenses.txt` | ARM32 QuickJS guest only |
| v86 0.5.473+g315b2b0 | Checksum-pinned official npm runtime, copied by `examples/c-bench/build.py` | v86 BSD-2-Clause plus bundled runtime dependency notices in `c-bench-v86-LICENSE.txt`; retained source notices under `examples/c-bench/licenses/` | Separate x86 execution path in the C comparison |
| Prism 1.30.0 | Core and C/JavaScript grammars in `examples/browser/vendor/prism-1.30.0/` | `LICENSE` (MIT); unchanged source and checksums retained alongside it | On-demand browser highlighting of benchmark source |
| oaknut | `externals/dynarmic/externals/oaknut/` | `LICENSE` (MIT) | No for `ARCHITECTURE=wasm`; retained in the submodule |
| biscuit | `externals/dynarmic/externals/biscuit/` | `LICENSE.md` (MIT) | No for the A32/A64 wasm build |
| Catch2 | `externals/dynarmic/externals/catch/` | `LICENSE.txt` (Boost Software License 1.0) | No upstream tests; available to native dependency trees |
| Xbyak | `externals/dynarmic/externals/xbyak/` | upstream `readme.md` (BSD-3-Clause) | No for wasm |
| Zycore | `externals/dynarmic/externals/zycore/` | `LICENSE` (MIT) | No for wasm |
| Zydis | `externals/dynarmic/externals/zydis/` | `LICENSE` (MIT) | No for wasm |

The Emscripten SDK, Node.js, Chromium/Firefox, ARM GNU toolchain and Zig are
build or test tools supplied by the user or CI environment; the SDKs and tools
themselves are not redistributed by this repository. A prebuilt browser page
does contain generated Emscripten runtime and system-library code. Its Pages
artifact therefore includes `runtime-licenses.txt`, generated from the exact
Emscripten SDK used for that build. That file retains the SDK, musl, libc++,
libc++abi, compiler-rt, libunwind/LLVM libc (when present), dlmalloc and Boost
license texts and their source digests. It complements this source notice; it
does not make the Emscripten SDK part of the source archive or grant any
additional rights to the project code.

The private root npm tooling uses Vite and parse5 (both MIT), with exact versions
and dependency integrity recorded in `package-lock.json`. They run only during
local review and CI, retain their licenses in their installed npm packages, and
are not bundled into the Pages UI or the published dynarmic.js runtime. The
corresponding-source archive includes their dependency declarations and lockfile,
not `node_modules`.

The QuickJS example pins upstream archive `quickjs-2025-09-13-2.tar.xz` to SHA-256
`996c6b5018fc955ad4d06426d0e9cb713685a00c825aa5c0418bd53f7df8b0b4`.
Keep `quickjs-source.tar.xz`, `quickjs-LICENSE.txt`, `quickjs-runtime-licenses.txt` and `quickjs-manifest.json`
beside its generated runtime files. The source archive contains the complete
unmodified upstream release; project-owned harness and build code are included
in `source.tar.gz`. The manifest binds both runtime builds to that archive and
their local inputs. Source packaging verifies these hashes before distribution.
Newlib notices come from the installed ARM toolchain (or `QUICKJS_NEWLIB_LICENSE`
for an explicitly selected notice file). The build retains their complete text
and hash, along with the GNU libgcc license and runtime exception.

The official JavaScript suite pins `quickjs-extras-2025-09-13.tar.xz` to SHA-256
`7e5aa54376aad91c952b6a6f44f150856073da71973c8914f041932969e9f4e8`.
The distributed original and adapted JavaScript retain their complete upstream
headers; the adaptation does not replace those terms with the QuickJS engine's
license. Keep `quickjs-extras-source.tar.xz`, `official-v8-upstream.js`,
`official-v8-adapter.js`, `official-benchmarks.json` and
`quickjs-extras-LICENSE.txt` with the browser distribution.

The matching `source.tar.gz`, `source-manifest.json`, `LICENSE.txt`,
`THIRD_PARTY_NOTICES.md` and `runtime-licenses.txt` files must stay with a
prebuilt `arm_jit.js`/`arm_jit.wasm` pair. The manifest records the Emscripten
version, build settings, source revisions and artifact hashes. Rebuilds from a
source archive require installing the toolchains named in the build guide;
they must not assume that the SDK is bundled. CoreMark is downloaded by the
user for the optional benchmark and is intentionally not vendored. The current
reproducible workload uses EEMBC revision
`1f483d5b8316753a742cbf5590caf5bd0a4e4777`.
Source headers state Apache-2.0; upstream `LICENSE.md` also contains trademark
and acceptable-use terms. Only the documented UART stub is adapted in a build
directory. See [the workload protocol](tests/bench/coremark/README.md) and
upstream reporting rules before publishing CoreMark scores. Duration-qualified
local observations are not certification.

Release archives must include the submodule sources and their license files.
The C comparison's `c-bench-v86-LICENSE.txt` must accompany both its v86 JS and
Wasm files. It includes the main v86 notice and notices for runtime dependencies
from the pinned upstream revision; BIOS and upstream test images are not shipped.
The three C kernel binaries have no libc or compiler-runtime dependencies.
Verify this inventory when changing the pinned Dynarmic revision or adding a
dependency; see [the release procedure](docs/Releasing.md).
