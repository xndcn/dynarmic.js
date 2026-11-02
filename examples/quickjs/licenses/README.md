# QuickJS runtime and benchmark license texts

The QuickJS ARM guest links newlib and GNU libgcc. `GPL-3.0.txt` and
`GCC-Runtime-Exception-3.1.txt` are the complete GPL version 3 and GCC Runtime
Library Exception version 3.1 texts, retrieved from GCC's
`releases/gcc-10.3.0` source on 2026-10-30:

- https://github.com/gcc-mirror/gcc/blob/releases/gcc-10.3.0/COPYING3
- https://github.com/gcc-mirror/gcc/blob/releases/gcc-10.3.0/COPYING.RUNTIME

The terms also apply to the corresponding runtime libraries in current GNU ARM
GCC toolchains. They do not relicense project code or QuickJS. The ARM compiler
version is recorded with the build. The build collects newlib's copyright and
license inventory from the installed toolchain, retaining its full notices and
source hash in `quickjs-runtime-licenses.txt` and `quickjs-manifest.json`.

For a non-Debian toolchain, set `QUICKJS_NEWLIB_LICENSE` to its complete
`COPYING.NEWLIB` (or equivalent copyright inventory) before building.

`GPL-2.0.txt` is the complete GPL version 2 text for the DeltaBlue benchmark in
QuickJS extras. Its upstream archive retains the GPL-2.0-or-later source notice
but no license copy. The official benchmark build copies this file to
`quickjs-extras-LICENSE.txt`; the Pages packager verifies the complete text's
canonical SHA-256 and requires an exact distributed match. This third-party
license does not change the project's MIT license.
