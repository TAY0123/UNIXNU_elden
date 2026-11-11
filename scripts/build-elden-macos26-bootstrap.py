#!/usr/bin/env python3
"""Build the Elden 4 KiB ARM64e XNU candidate on the macOS build host."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys


def stage_recovery_usb(source: Path, device: Path, core: Path) -> dict:
    core_files = {
        "Dwc3.c": "16681b8e405b0d3862cb3289005f210b80b5bea2cc33adc26b2258d18ab9cea6",
        "Dwc3.h": "7731dddfcecb5bf2b7fe44054bae4db786924d20f767ece43d84f82e8c84d690",
        "Dwc3Regs.h": "5e9a362cfa6385f55a793b48965963559fc2c1f167cab9d3afa3845603328e4b",
        "LICENSE.m1n1": "11ca423a564b52d3b7f260398baa20d016c25ae180fbdd0067c27b7d1ed7b420",
    }
    for name, expected in core_files.items():
        if hashlib.sha256((core / name).read_bytes()).hexdigest() != expected:
            raise ValueError(f"Recovery USB core differs from the pinned input: {name}")
    adapter = device / "scripts/driver-patches"
    iokit = source / "iokit/Kernel"
    files = {iokit / name: adapter / name for name in (
        "EldenRecoveryUsb.cpp", "EldenUsbDebugXnu.inc", "EldenUfsIdentity.h")}
    files.update({iokit / name: core / name for name in ("Dwc3.h", "Dwc3Regs.h", "LICENSE.m1n1")})
    files[iokit / "EldenUsbDwc3.c"] = core / "Dwc3.c"
    files[source / "pexpert/arm/pe_elden_usb_log.h"] = adapter / "EldenRecoveryUsbLog.h"
    files[iokit / "EldenUsbResources.h"] = device / "soc/sm8750/include/EldenUsbResources.h"
    for destination, path in files.items():
        shutil.copyfile(path, destination)
    overlay = device / "patches/macos-recovery/elden-macos26-usb-console.patch"
    subprocess.run(["git", "-C", source, "apply", overlay], check=True)
    log = source / "pexpert/arm/pe_sheng_log.h"
    code = log.read_text()
    marker = '#include "pe_sheng_boot_progress_kernel.h"\n'
    if code.count(marker) != 1:
        raise ValueError("expected one SHENG boot-progress header")
    panic_end = '    sheng_log_clean(r, sizeof(*r));\n}\n#endif\n'
    if code.count(panic_end) != 1:
        raise ValueError("expected one end of the SHENG panic recorder")
    code = code.replace(marker, marker + '#include "pe_elden_usb_log.h"\n')
    log.write_text(code.replace(panic_end, '    sheng_log_clean(r, sizeof(*r));\n'
                                '    elden_usb_capture_panic(message);\n}\n#endif\n'))
    return {str(path.relative_to(source)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (*files, log)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sheng-root", type=Path, required=True)
    parser.add_argument("--kdk-root", type=Path, required=True)
    parser.add_argument("--output-name", default="elden-macos26-4k-bootstrap",
                        help="build directory name under the reference checkout's out directory")
    parser.add_argument("--elden-ufs-device", type=Path,
                        help="also compile the read-only UFS companion from this device checkout")
    parser.add_argument("--build-gop-display", action="store_true",
                        help="include the Elden framebuffer in the UFS companion")
    parser.add_argument("--adreno830-gmu-firmware", type=Path,
                        help="include the opt-in Elden GMU image validation probe")
    parser.add_argument("--recovery-usb-core", type=Path,
                        help="directory containing the pinned DWC3 core for the Recovery USB console")
    parser.add_argument("-j", "--jobs", type=int, default=8, choices=range(1, 33))
    args = parser.parse_args()
    if args.build_gop_display and args.elden_ufs_device is None:
        parser.error("--build-gop-display requires --elden-ufs-device")

    root = args.sheng_root.resolve(strict=True)
    sys.path.insert(0, str(root / "scripts"))
    import build_macos26_xnu as build

    if Path(args.output_name).name != args.output_name or args.output_name in ("", ".", ".."):
        parser.error("--output-name must be a single directory name")
    output = root / "out" / args.output_name
    output.mkdir(parents=True, exist_ok=True)
    patches = Path(__file__).resolve().parents[1] / "patches/macos-recovery"
    staged_patches = []
    for name in ("elden-macos26-4k-user-va.patch", "elden-macos26-small-kmem.patch",
                 "elden-macos26-cpu-affinity.patch"):
        staged_patch = output / name
        shutil.copyfile(patches / name, staged_patch)
        staged_patches.append(staged_patch)
    prepare = build.prepare
    run = build.run
    armcpu_patch = output / "elden-armcpu-inline-asm-and-affinity.patch"
    armcpu_patch.write_bytes(build.CPU_ASM_PATCH.read_bytes() + b"\n" +
                            (patches / "elden-macos26-armcpu-affinity.patch").read_bytes())
    build.CPU_ASM_PATCH = armcpu_patch

    def prepare_elden():
        build.OUT = output
        patch = root / "platform/sheng/macos26/patches/0007-native-macos-page-layout.patch"
        original = patch.read_text()
        start = original.index("--- a/pexpert/pexpert/arm64/SHENG.h\n")
        end = original.index("--- a/", start + 1)
        if "+#define __ARM_16K_PG__" not in original[start:end]:
            raise ValueError("reference SHENG page layout has changed")
        filtered = output / "0007-elden-4k-page-layout.patch"
        filtered.write_text(original[:start] + original[end:])
        build.PATCHES[build.PATCHES.index(patch)] = filtered
        build.PATCHES.extend(staged_patches)

        record = prepare()
        header = output / "source/pexpert/pexpert/arm64/SHENG.h"
        if "__ARM_16K_PG__" in header.read_text():
            raise ValueError("staged SHENG source still requests the 16 KiB granule")
        trace_overlay = patches / "elden-macos26-bootstrap-trace.patch"
        pid1_overlay = patches / "elden-macos26-pid1-trace.patch"
        ignition_overlay = patches / "elden-macos26-recovery-ignition.patch"
        source = output / "source"
        subprocess.run(["git", "-C", source, "apply", trace_overlay, pid1_overlay], check=True)
        subprocess.run(["git", "-C", source, "apply", ignition_overlay], check=True)
        if args.recovery_usb_core is not None:
            record["elden_recovery_usb_sources"] = stage_recovery_usb(
                source, patches.parents[1], args.recovery_usb_core.resolve(strict=True))
        if args.adreno830_gmu_firmware is not None:
            image = args.adreno830_gmu_firmware.read_bytes()
            digest = hashlib.sha256(image).hexdigest()
            if digest != "1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5":
                raise ValueError("expected the pinned Elden gen80000_gmu.bin")
            driver = patches.parents[1] / "drivers/Adreno830"
            kit = root / "UniXNU/kits/AdrenoFirmware"
            pexpert = source / "pexpert/arm"
            inputs = [kit / name for name in ("AdrenoFirmware.c", "AdrenoFirmware.h")]
            inputs.extend(driver / name for name in
                          ("EldenAdreno830.c", "EldenAdreno830.h", "EldenAdreno830Boot.inc"))
            for item in inputs:
                shutil.copyfile(item, pexpert / item.name)
            rows = (", ".join(f"0x{byte:02x}" for byte in image[i:i + 16])
                    for i in range(0, len(image), 16))
            (pexpert / "EldenAdreno830GmuImage.h").write_text(
                "static const uint8_t elden_gmu_image[] = {\n    " + ",\n    ".join(rows) + "\n};\n")
            pe_init = pexpert / "pe_init.c"
            code = pe_init.read_text()
            marker = "void\nPE_lockdown_iokit(void)\n{\n"
            if code.count(marker) != 1:
                raise ValueError("expected one PE_lockdown_iokit definition")
            pe_init.write_text(code.replace(marker, '#include "EldenAdreno830Boot.inc"\n\n'
                                               + marker + "\telden_adreno830_probe();\n"))
            record["elden_adreno830_probe"] = {
                "firmware_sha256": digest,
                "sources_sha256": {item.name: build.sha(item) for item in inputs},
            }
        source_diff = subprocess.check_output(["git", "-C", source, "diff", "HEAD", "--binary"])
        record["source_diff_sha256"] = hashlib.sha256(source_diff).hexdigest()
        record["elden_bootstrap"] = {
            "page_bytes": 4096,
            "arm64_large_memory": False,
            "link_address": "0xfffffff007004000",
            "bootstrap_trace_overlay_sha256": build.sha(trace_overlay),
            "bootstrap_trace_header_sha256": build.sha(
                source / "osfmk/arm64/sheng_bootstrap_trace.h"),
            "pid1_trace_overlay_sha256": build.sha(pid1_overlay),
            "recovery_ignition_overlay_sha256": build.sha(ignition_overlay),
            "hardware_verified": False,
        }
        build.save(record)
        return record

    def run_elden(command, *run_args, **kwargs):
        # Change only the make configuration for this isolated build.
        flags = {
            "ARM64_LARGE_MEMORY=1": "ARM64_LARGE_MEMORY=0",
            "ARM64_LINK_ADDR=0xfffffe0007004000": "ARM64_LINK_ADDR=0xfffffff007004000",
        }
        return run([flags.get(item, item) for item in command], *run_args, **kwargs)

    build.prepare = prepare_elden
    build.run = run_elden
    sys.argv = [sys.argv[0], "--arch", "arm64e", "--link-kernel",
                "--kdk-root", str(args.kdk_root.resolve(strict=True)), "-j", str(args.jobs)]
    if args.elden_ufs_device is not None:
        device = args.elden_ufs_device.resolve(strict=True)
        sys.argv.extend(["--build-ufs-driver", "--elden-ufs-device", str(device)])
        if args.build_gop_display:
            sys.argv.extend(["--build-gop-display", "--elden-gop-board", str(device / "board.toml")])
    build.main()


if __name__ == "__main__":
    main()
