#!/usr/bin/env python3
"""Build the isolated GPL UFS application without rebuilding accepted firmware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / 'drivers/EldenUfsStage2'
MU = ROOT / 'third_party/mu_wide_platforms'
BASE = MU / 'MU_BASECORE'
TOOLS = BASE / 'BaseTools'


def prepare(usb_core=None):
    provenance = json.loads((MODULE / 'provenance.json').read_text())
    for name, expected in provenance['files'].items():
        data = (MODULE / name).read_bytes()
        # Git normalizes the imported CRLF files to LF in this checkout.
        variants = (data, data.replace(b'\n', b'\r\n')) if b'\r' not in data else (data,)
        if not any(len(value) == expected['bytes'] and hashlib.sha256(value).hexdigest() == expected['sha256']
                   for value in variants):
            raise SystemExit('vendor provenance mismatch: ' + name)
    generated = MODULE / 'Generated'
    generated.mkdir(exist_ok=True)
    sources = [*sorted((MODULE / 'Core').glob('*.[ch]')),
               MODULE / 'EldenUfsSession.c', MODULE / 'EldenUfsSession.h',
               MODULE / 'EldenBundle.c', MODULE / 'EldenBundle.h',
               MODULE / 'EldenSlotSwitch.c', MODULE / 'EldenSlotSwitch.h']
    report = {}
    patch = MODULE / 'EldenUfsBootLun.patch'
    patched = ('storage_ufs.c', 'storage_ufs.h')
    with tempfile.TemporaryDirectory(prefix='elden-ufs-source-') as directory:
        stage = Path(directory)
        for name in patched:
            (stage / name).write_text((MODULE / 'Core' / name).read_text(), newline='\n')
        subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch)],
                       cwd=stage, check=True, capture_output=True)
        for source in sources:
            original = source.read_bytes()
            content = (stage / source.name).read_text() if source.name in patched else original.decode()
            content = re.sub(r'^#include <(?:stdint|stddef|stdbool|string)\.h>\s*$',
                             '', content, flags=re.MULTILINE)
            content = re.sub(r'#include <(storage_\w+\.h)>', r'#include "\1"', content)
            content = re.sub(r'"Core/(storage_\w+\.h)"', r'"\1"', content)
            target = generated / source.name
            target.write_text('#include "../EfiCompat.h"\n' + content, newline='\n')
            report[source.name] = dict(source=str(source.relative_to(ROOT)),
                sha256=hashlib.sha256(original).hexdigest(),
                compiled_sha256=hashlib.sha256(target.read_bytes()).hexdigest())
            if source.name in patched:
                report[source.name]['overlay_sha256'] = hashlib.sha256(patch.read_bytes()).hexdigest()
    for name in ('EldenFdtHandoff.c', 'EldenFdtHandoff.h'):
        source = ROOT / 'drivers/EldenPayloadLoader' / name
        shutil.copyfile(source, generated / name)
        report[name] = dict(source=str(source.relative_to(ROOT)),
            sha256=hashlib.sha256(source.read_bytes()).hexdigest())
    if usb_core is not None:
        usb_files = {
            'Dwc3.c': '16681b8e405b0d3862cb3289005f210b80b5bea2cc33adc26b2258d18ab9cea6',
            'Dwc3.h': '7731dddfcecb5bf2b7fe44054bae4db786924d20f767ece43d84f82e8c84d690',
            'Dwc3Regs.h': '5e9a362cfa6385f55a793b48965963559fc2c1f167cab9d3afa3845603328e4b',
            'LICENSE.m1n1': '11ca423a564b52d3b7f260398baa20d016c25ae180fbdd0067c27b7d1ed7b420',
        }
        for name, expected in usb_files.items():
            data = (usb_core / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise SystemExit('USB core differs from pinned input: ' + name)
            if name.endswith(('.c', '.h')):
                content = re.sub(rb'^#include <(?:stdint|stddef)\.h>\s*$', b'', data, flags=re.MULTILINE)
                (generated / name).write_bytes(b'#include "../EfiCompat.h"\n' + content)
            else:
                (generated / name).write_bytes(data)
            report[name] = dict(sha256=expected,
                compiled_sha256=hashlib.sha256((generated / name).read_bytes()).hexdigest())
        shutil.copyfile(ROOT / 'soc/sm8750/include/EldenUsbResources.h', generated / 'EldenUsbResources.h')
        shutil.copyfile(ROOT / 'soc/sm8750/include/EldenPonResources.h', generated / 'EldenPonResources.h')
        board = tomllib.loads((ROOT / 'board.toml').read_text())
        repeater = board['board']['hardware']['usb_repeater']
        constants = {
            'PTN_GENI_BASE': repeater['geni_base'],
            'PTN_I2C_ADDRESS': repeater['i2c_address'],
            'PTN_RESET_PPID': repeater['reset_ppid'],
            'PTN_MUX_GPIO': repeater['port_mux_gpio'],
        }
        tuning = ', '.join(f'{{{reg:#04x}, {value:#04x}}}'
                           for reg, value in repeater['long_edge_tuning'])
        (generated / 'EldenUsbRepeaterConfig.h').write_text(
            '// Generated from board.toml\n#pragma once\n' +
            ''.join(f'#define {name} {value:#x}U\n' for name, value in constants.items()) +
            f'#define PTN_LONG_EDGE_TUNING {{{tuning}}}\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', type=Path, default=Path.home()/'toolchains/llvm-20/bin')
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--usb-core', type=Path, help='also build EldenUsbMenu.efi with the pinned DWC3 sources')
    args = parser.parse_args()
    work = ROOT / 'out/ufs-stage2-20261106'
    work.mkdir(parents=True, exist_ok=True)
    (work/'sources.json').write_text(json.dumps(prepare(args.usb_core), indent=2)+'\n')
    if args.prepare_only:
        return
    llvm = args.toolchain.resolve()
    for name in ('clang', 'lld-link', 'llvm-lib', 'llvm-rc'):
        if not (llvm/name).is_file():
            raise SystemExit('missing tool: '+str(llvm/name))
    conf = work/'Conf'
    conf.mkdir(exist_ok=True)
    for name in ('target', 'tools_def', 'build_rule'):
        shutil.copyfile(TOOLS/'Conf'/(name+'.template'), conf/(name+'.txt'))
    env = dict(os.environ)
    env.update(WORKSPACE=str(MU), PACKAGES_PATH=os.pathsep.join(map(str, [
        ROOT/'drivers', MU, BASE, MU/'Common/MU', MU/'Common/MU_TIANO',
        MU/'Silicon/Arm/MU_TIANO'])), EDK_TOOLS_PATH=str(TOOLS), CONF_PATH=str(conf),
        PYTHON_COMMAND=sys.executable, CLANG_BIN=str(llvm)+os.sep)
    env['PATH'] = os.pathsep.join([str(llvm), str(TOOLS/'BinWrappers/PosixLike'),
                                  str(TOOLS/'Source/C/bin'), env['PATH']])
    command = [str(TOOLS/'BinWrappers/PosixLike/build'), '-p', 'EldenUfsStage2/EldenUfsStage2.dsc',
               '-a', 'AARCH64', '-b', 'DEBUG', '-t', 'CLANGPDB', '-n', '4']
    if args.usb_core is not None:
        command += ['-D', 'ELDEN_USB_MENU=1']
    (work/'command.json').write_text(json.dumps(command, indent=2)+'\n')
    with (work/'build.log').open('w') as log:
        result = subprocess.run(command, cwd=MU, env=env, stdout=log, stderr=subprocess.STDOUT)
    (work/'result.json').write_text(json.dumps(dict(returncode=result.returncode))+'\n')
    if result.returncode:
        print((work/'build.log').read_text()[-10000:])
        raise SystemExit(result.returncode)
    source = MU/'Build/EldenUfsStage2/DEBUG_CLANGPDB/AARCH64/EldenUfsProbe.efi'
    destination = work/source.name
    shutil.copyfile(source, destination)
    for name in ('EldenBootSlot.efi', 'EldenStage2.efi', 'EldenStage2Test.efi',
                 'EldenStage2Consumer.efi', 'EldenExternalTest.efi'):
        shutil.copyfile(source.with_name(name), work/name)
    if args.usb_core is not None:
        shutil.copyfile(source.with_name('EldenUsbMenu.efi'), work/'EldenUsbMenu.efi')
    report = dict(path=str(destination), bytes=destination.stat().st_size,
        sha256=hashlib.sha256(destination.read_bytes()).hexdigest(), hardware_tested=False)
    report['applications'] = {p.name: dict(bytes=p.stat().st_size,
        sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(work.glob('*.efi'))}
    (work/'artifact.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
