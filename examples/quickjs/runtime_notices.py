"""Collect notices for the newlib and libgcc code linked into the ARM guest."""
import hashlib
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
LICENSE_INPUTS = ["licenses/GPL-3.0.txt", "licenses/GCC-Runtime-Exception-3.1.txt"]


def write_notices(dist, arm_cc):
    explicit = os.environ.get("QUICKJS_NEWLIB_LICENSE")
    candidates = [Path(explicit)] if explicit else [
        Path(arm_cc).resolve().parents[1] / "share/doc/libnewlib-dev/copyright",
        Path("/usr/share/doc/libnewlib-dev/copyright"),
        Path("/usr/share/doc/libnewlib-arm-none-eabi/copyright"),
    ]
    source = next((file for file in candidates if file.is_file()), None)
    if source is None:
        raise SystemExit("Cannot find newlib's runtime notices. Set QUICKJS_NEWLIB_LICENSE to the complete COPYING.NEWLIB / copyright file from the ARM toolchain.")
    newlib = source.read_text()
    if len(newlib) < 1000 or "copyright" not in newlib.lower():
        raise SystemExit("The selected newlib runtime notice is incomplete")
    version = subprocess.check_output([arm_cc, "--version"], text=True).splitlines()[0]
    text = ["QuickJS ARM32 guest — linked runtime license notices\n\n",
            "This guest links newlib and GNU libgcc from the ARM toolchain:\n" + version + "\n\n",
            "The newlib copyright inventory is retained below in full. Some notices\n"
            "refer to other targets and do not imply that all listed code is linked.\n"
            "GNU libgcc is distributed under GPL-3.0-or-later with the GCC Runtime\n"
            "Library Exception 3.1. The compiler itself is not distributed here.\n\n",
            "This product includes software developed by the University of California,\n"
            "Berkeley and its contributors.\n\n",
            "=== newlib notices supplied by the ARM toolchain ===\n", newlib]
    inputs = [{"component": "newlib", "origin": "QUICKJS_NEWLIB_LICENSE" if explicit else "system ARM newlib package",
               "sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "bytes": source.stat().st_size}]
    for name in LICENSE_INPUTS:
        file = HERE / name
        body = file.read_text()
        text.extend(["\n=== " + name + " ===\n", body])
        inputs.append({"component": "libgcc license", "origin": "examples/quickjs/" + name,
                       "sha256": hashlib.sha256(file.read_bytes()).hexdigest(), "bytes": file.stat().st_size})
    (Path(dist) / "quickjs-runtime-licenses.txt").write_text("".join(text))
    return {"path": "quickjs-runtime-licenses.txt", "toolchain": version, "inputs": inputs}
