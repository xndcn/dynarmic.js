#!/usr/bin/env python3
"""Build a fixed, traceable CoreMark workload. Upstream sources are not vendored."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=pathlib.Path)
parser.add_argument('output', type=pathlib.Path)
parser.add_argument('--iterations', type=int, default=4000)
args = parser.parse_args()
if not 1 <= args.iterations <= 2147483647:
    parser.error('--iterations must be 1..2147483647 (no runtime auto-calibration)')
source, output = args.source.resolve(), args.output.resolve()
repo = pathlib.Path(__file__).resolve().parents[3]
port = pathlib.Path(__file__).resolve().parent

def run(command):
    subprocess.run(command, check=True)

def git(*command):
    return subprocess.check_output(['git', '-C', str(source), *command], text=True).strip()

# Hashes also bind all consumed files; require clean tracked inputs rather than
# silently identifying locally modified algorithms as an upstream revision.
revision = git('rev-parse', 'HEAD')
if git('status', '--porcelain', '--untracked-files=no'):
    parser.error('CoreMark checkout has tracked modifications')
output.mkdir(parents=True, exist_ok=True)
for name in ['manifest.json', 'a32.elf', 'a64.elf', 'coremark_a32.bin', 'coremark_a64.bin', 'coremark_host', 'ee_printf.c']:
    if (output / name).exists():
        parser.error(f'refusing to overwrite {output / name}')

kernel = ['core_list_join.c', 'core_main.c', 'core_matrix.c', 'core_state.c', 'core_util.c', 'barebones/cvt.c']
inputs = {f'upstream/{name}': source / name for name in kernel + ['barebones/ee_printf.c', 'coremark.h']}
inputs.update({f'port/{name}': port / name for name in ['core_portme.c', 'core_portme.h', 'build.py']})
for arch, directory in [('a32', 'guest'), ('a64', 'guest64')]:
    for name in ['start.S', 'link.ld']:
        inputs[f'{arch}/{name}'] = repo / 'tests' / directory / name
for name in ['string.h', 'math.h']:
    inputs[f'a64/include/{name}'] = repo / 'tests/guest64/include' / name
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
sources = {name: sha(p) for name, p in sorted(inputs.items())}

# Adapt only the documented barebones UART stub, in the output tree. Refuse a
# changed stub shape instead of accidentally replacing a different function.
text = (source / 'barebones/ee_printf.c').read_text()
pattern = r'void\s+uart_send_char\(char c\)\s*\{\s*(?:#error[^\n]*\n)?\s*/\*.*?\*/\s*\}'
text, count = re.subn(pattern, 'void portme_putchar(char c);\nvoid uart_send_char(char c) { portme_putchar(c); }', text, flags=re.S)
if count != 1:
    parser.error('unrecognized barebones UART stub')
(output / 'ee_printf.c').write_text(text)

compilers = {'a32': ['arm-none-eabi-gcc'], 'a64': ['python3', '-m', 'ziglang', 'cc'], 'native': ['cc']}
flags = {'a32': ['-O2', '-mcpu=cortex-a15', '-marm', '-mfpu=vfpv3', '-mfloat-abi=softfp'],
         'a64': ['-target', 'aarch64-freestanding', '-O2'], 'native': ['-O2']}
versions = {key: subprocess.check_output(cmd + ['--version'], text=True).strip() for key, cmd in compilers.items()}
srcs = [str(source / name) for name in kernel] + [str(output / 'ee_printf.c'), str(port / 'core_portme.c')]
defs = [f'-DITERATIONS={args.iterations}', '-DTOTAL_DATA_SIZE=2000', '-DPERFORMANCE_RUN=1', '-DMAIN_HAS_NOARGC=1', f'-I{port}', f'-I{source}']
commands = {}
for arch in ['a32', 'a64', 'native']:
    cmd = compilers[arch] + flags[arch] + [f'-DCOMPILER_FLAGS="{" ".join(flags[arch])}"'] + defs
    if arch == 'native':
        cmd += ['-DHOST'] + srcs + ['-o', str(output / 'coremark_host')]
    else:
        directory = repo / 'tests' / ('guest' if arch == 'a32' else 'guest64')
        cmd += ['-ffreestanding', '-nostdlib', '-Dmain=guest_main']
        if arch == 'a64':
            cmd += [f'-I{repo / "tests/guest64/include"}']
        cmd += ['-T', str(directory / 'link.ld'), str(directory / 'start.S')] + srcs
        if arch == 'a32':
            cmd += ['-lgcc']
        cmd += ['-o', str(output / (arch + '.elf'))]
    commands[arch] = cmd
    run(cmd)
    if arch != 'native':
        objcopy = ['arm-none-eabi-objcopy'] if arch == 'a32' else ['python3', '-m', 'ziglang', 'objcopy']
        run(objcopy + ['-O', 'binary', str(output / (arch + '.elf')), str(output / f'coremark_{arch}.bin')])

if sources != {name: sha(p) for name, p in sorted(inputs.items())} or git('rev-parse', 'HEAD') != revision:
    raise RuntimeError('workload inputs changed during build')
manifest = {'schema': 'dynarmic.js/coremark-build-v1', 'revision': revision,
            'iterations': args.iterations, 'dataSize': 2000, 'contexts': 1,
            'sources': sources, 'compilers': versions, 'flags': {k: ' '.join(v) for k, v in flags.items()},
            'commands': commands, 'uartSourceSha256': sha(output / 'ee_printf.c'),
            'artifacts': {arch: {'file': file, 'sha256': sha(output / file)} for arch, file in
                          [('a32', 'coremark_a32.bin'), ('a64', 'coremark_a64.bin'), ('native', 'coremark_host')]}}
(output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
