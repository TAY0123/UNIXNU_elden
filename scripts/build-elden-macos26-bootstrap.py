#!/usr/bin/env python3
"""Build the Elden 4 KiB ARM64e XNU candidate on the macOS build host."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tomllib

import elden_recovery_graphics


DEVICE = Path(__file__).resolve().parents[1]
PROFILE = DEVICE / "configs/elden-macos26-4k.toml"
PATCHES = DEVICE / "patches/macos-recovery"

# These reference headers are staged before the Elden overlays modify them.
OVERLAID_HEADERS = {
    "osfmk/arm64/sheng_bootstrap_trace.h",
    "osfmk/arm64/sheng_early_exception.h",
    "pexpert/arm/pe_sheng_log.h",
}


def write_if_changed(path: Path, content: bytes) -> None:
    if not path.is_file() or path.read_bytes() != content:
        path.write_bytes(content)


def stage_recovery_usb(source: Path, core: Path, apply_overlay: bool, rpmh_read: bool) -> dict:
    core_files = {
        "Dwc3.c": "16681b8e405b0d3862cb3289005f210b80b5bea2cc33adc26b2258d18ab9cea6",
        "Dwc3.h": "7731dddfcecb5bf2b7fe44054bae4db786924d20f767ece43d84f82e8c84d690",
        "Dwc3Regs.h": "5e9a362cfa6385f55a793b48965963559fc2c1f167cab9d3afa3845603328e4b",
        "LICENSE.m1n1": "11ca423a564b52d3b7f260398baa20d016c25ae180fbdd0067c27b7d1ed7b420",
    }
    for name, expected in core_files.items():
        if hashlib.sha256((core / name).read_bytes()).hexdigest() != expected:
            raise ValueError(f"Recovery USB core differs from the pinned input: {name}")
    adapter = DEVICE / "scripts/driver-patches"
    iokit = source / "iokit/Kernel"
    files = {iokit / name: adapter / name for name in (
        "EldenRecoveryUsb.cpp", "EldenUsbDebugXnu.inc", "EldenUfsIdentity.h",
        "EldenPonObserve.inc")}
    files.update({iokit / name: core / name for name in ("Dwc3.h", "Dwc3Regs.h", "LICENSE.m1n1")})
    files[iokit / "EldenUsbDwc3.c"] = core / "Dwc3.c"
    files[source / "pexpert/arm/pe_elden_usb_log.h"] = adapter / "EldenRecoveryUsbLog.h"
    files[iokit / "EldenUsbResources.h"] = DEVICE / "soc/sm8750/include/EldenUsbResources.h"
    files[iokit / "EldenPonResources.h"] = DEVICE / "soc/sm8750/include/EldenPonResources.h"
    for destination, path in files.items():
        content = path.read_bytes()
        if rpmh_read and destination.name == "EldenRecoveryUsb.cpp":
            content = b"#define ELDEN_RECOVERY_RPMH_READ 1\n" + content
        write_if_changed(destination, content)
    log = source / "pexpert/arm/pe_sheng_log.h"
    if apply_overlay:
        subprocess.run(["git", "-C", source, "apply", PATCHES / "elden-macos26-usb-console.patch"],
                       check=True)
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
    subprocess.run(["git", "-C", source, "add", "-N", "--", *files], check=True)
    return {str(path.relative_to(source)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (*files, log)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sheng-root", type=Path, required=True)
    parser.add_argument("--kdk-root", type=Path)
    parser.add_argument("--prepare-only", action="store_true",
                        help="stage and verify the patched XNU source without a KDK or compiler")
    parser.add_argument("--output-name", default="elden-macos26-4k-bootstrap",
                        help="build directory name under the reference checkout's out directory")
    parser.add_argument("--elden-ufs-device", type=Path,
                        help="also compile the read-only UFS companion from this device checkout")
    parser.add_argument("--elden-recovery-contract", type=Path,
                        help="pair the read-only BaseSystem with a local ISC seed contract")
    parser.add_argument("--build-gop-display", action="store_true",
                        help="include the Elden framebuffer in the UFS companion")
    parser.add_argument("--adreno830-gmu-firmware", type=Path,
                        help="include Elden GMU, GPUCC and command database probes")
    parser.add_argument("--adreno830-hfi-source", type=Path,
                        help="include the HFI table probe using pinned AdrenoFirmware sources")
    parser.add_argument("--recovery-usb-core", type=Path,
                        help="directory containing the pinned DWC3 core for the Recovery USB console")
    parser.add_argument("--recovery-graphics-cache", type=Path,
                        help="paired 25F71 .01 dyld cache for the opt-in software display")
    parser.add_argument("--trace-recovery-copy", action="store_true",
                        help="trace permission failures in the Recovery Data template copy")
    parser.add_argument("-j", "--jobs", type=int, default=8, choices=range(1, 33))
    args = parser.parse_args()
    if not args.prepare_only and args.kdk_root is None:
        parser.error("--kdk-root is required for compilation")
    if args.build_gop_display and args.elden_ufs_device is None:
        parser.error("--build-gop-display requires --elden-ufs-device")
    if args.elden_recovery_contract is not None and args.elden_ufs_device is None:
        parser.error("--elden-recovery-contract requires --elden-ufs-device")
    if args.adreno830_hfi_source is not None and args.adreno830_gmu_firmware is None:
        parser.error("--adreno830-hfi-source requires --adreno830-gmu-firmware")
    rpmh_read = args.recovery_usb_core is not None and args.adreno830_gmu_firmware is not None

    root = args.sheng_root.resolve(strict=True)
    sys.path.insert(0, str(root / "scripts"))
    import build_macos26_xnu as build

    if Path(args.output_name).name != args.output_name or args.output_name in ("", ".", ".."):
        parser.error("--output-name must be a single directory name")
    output = root / "out" / args.output_name
    output.mkdir(parents=True, exist_ok=True)
    source = output / "source"
    profile = tomllib.loads(PROFILE.read_text())
    kernel = profile["kernel"]
    platform_patches = [PATCHES / name for name in kernel["platform_patches"]]
    overlays = {
        entry["record"]: PATCHES / entry["patch"]
        if "optional" not in entry or getattr(args, entry["optional"]) else None
        for entry in profile["overlay"]
    }
    overlay_hashes = {key: build.sha(path) if path else None for key, path in overlays.items()}
    graphics_header = None
    graphics_name = "osfmk/vm/sheng_recovery_ui_pages.h"
    overlaid_headers = OVERLAID_HEADERS.copy()
    if args.recovery_graphics_cache is not None:
        graphics_header = elden_recovery_graphics.render(
            root / "config/macos26-recovery-ui-pages.json",
            args.recovery_graphics_cache, kernel["page_bytes"]).encode()
        overlaid_headers.add("osfmk/vm/sheng_recovery_ui.h")
    hfi_sources = []
    hfi_sha256 = None
    if args.adreno830_hfi_source is not None:
        hfi_sources = [args.adreno830_hfi_source / name for name in (
            "AdrenoHfi.h", "AdrenoHfiStartup.h", "AdrenoHfiA8xx.h", "AdrenoHfiA8xx.c",
            "AdrenoHfiVotes.h", "AdrenoHfiVotes.c")]
        digest = hashlib.sha256()
        for path in hfi_sources:
            digest.update(path.name.encode() + b"\0")
            digest.update(bytes.fromhex(build.sha(path)))
        hfi_sha256 = digest.hexdigest()
        if hfi_sha256 != "7c1a5eae37863b3debeb1cd2a7c7562958a8c323fcf4ff0281d2de4aa20e3446":
            raise ValueError("expected AdrenoFirmware HFI sources from deaaae24d7fe1a8f")
    profile_inputs = {
        "profile_sha256": build.sha(PROFILE),
        "wrapper_sha256": build.sha(Path(__file__)),
        "platform_patches": {path.name: build.sha(path) for path in platform_patches},
        "overlays": overlay_hashes,
        "usb_overlay_sha256": build.sha(PATCHES / "elden-macos26-usb-console.patch")
        if args.recovery_usb_core else None,
        "adreno830_firmware_sha256": build.sha(args.adreno830_gmu_firmware)
        if args.adreno830_gmu_firmware else None,
        "adreno830_hfi_source_sha256": hfi_sha256,
        "armdt_nvram_patch_sha256": build.sha(PATCHES / "elden-armdt-nvram-options.patch"),
        "recovery_graphics_header_sha256": hashlib.sha256(graphics_header).hexdigest()
        if graphics_header is not None else None,
    }
    rtc_driver = DEVICE / "drivers/EldenPmicRtc"
    rtc_resources = DEVICE / "soc/sm8750/include"
    rtc_inputs = [rtc_driver / name for name in ("EldenPmicRtc.h", "EldenPmicRtcXnu.inc")]
    rtc_inputs.extend(rtc_resources / name for name in ("EldenRtcResources.h", "EldenPonResources.h"))
    profile_inputs["rtc_sources"] = {path.name: build.sha(path) for path in rtc_inputs}
    previous_path = output / "build.json"
    previous = json.loads(previous_path.read_text()) if previous_path.is_file() else None
    if previous is not None and previous.get("elden_profile_inputs") != profile_inputs:
        raise ValueError("output contains a different Elden source profile; choose a fresh --output-name")
    if previous is None and source.exists():
        raise ValueError("output has an incomplete source tree; choose a fresh --output-name")

    prepare = build.prepare
    run = build.run
    stage_file = build.stage_file
    machine_patch = output / "elden-machine-driver-declarations.patch"
    write_if_changed(machine_patch, build.MACHINE_PATCH.read_bytes() + b"\n" +
                     (PATCHES / "elden-armdt-nvram-options.patch").read_bytes())
    build.MACHINE_PATCH = machine_patch
    armcpu_patch = output / "elden-armcpu-inline-asm-and-affinity.patch"
    write_if_changed(armcpu_patch, build.CPU_ASM_PATCH.read_bytes() + b"\n" +
                     (PATCHES / "elden-macos26-armcpu-affinity.patch").read_bytes())
    build.CPU_ASM_PATCH = armcpu_patch
    staged_platform_patches = []
    for patch in platform_patches:
        destination = output / patch.name
        write_if_changed(destination, patch.read_bytes())
        staged_platform_patches.append(destination)

    staged_headers = {}

    def stage_elden_file(origin, destination):
        path = Path(destination)
        if path.is_relative_to(source):
            name = path.relative_to(source).as_posix()
            if name == graphics_name and graphics_header is not None:
                write_if_changed(path, graphics_header)
                return
            if name in overlaid_headers:
                digest = build.sha(Path(origin))
                staged_headers[name] = digest
                if previous is not None:
                    if previous["elden_staged_header_inputs"].get(name) != digest:
                        raise ValueError(f"reference header changed: {name}; choose a fresh --output-name")
                    return
        stage_file(origin, destination)

    def prepare_elden():
        build.OUT = output
        build.PATCHES.extend(staged_platform_patches)
        record = prepare()
        if staged_headers.keys() != overlaid_headers:
            raise ValueError("reference builder did not stage the expected headers")
        header = source / "pexpert/pexpert/arm64/SHENG.h"
        if "__ARM_16K_PG__" in header.read_text():
            raise ValueError("staged SHENG source still requests the 16 KiB granule")
        if previous is None:
            for patch in filter(None, overlays.values()):
                subprocess.run(["git", "-C", source, "apply", patch], check=True)
        iokit = source / "iokit/Kernel"
        for item in rtc_inputs:
            write_if_changed(iokit / item.name, item.read_bytes())
        subprocess.run(["git", "-C", source, "add", "-N", "--", *(iokit / item.name for item in rtc_inputs)],
                       check=True)
        if graphics_header is not None:
            record["recovery_ui_policy"].update(
                boot_argument="elden-recovery-software-display=1",
                page_count=len(elden_recovery_graphics.POLICIES),
                page_bytes=kernel["page_bytes"],
                generated_sha256=build.sha(source / graphics_name),
                engine_sha256=build.sha(source / "osfmk/vm/sheng_recovery_ui.h"),
                kernel_hook_sha256=build.sha(source / "osfmk/vm/vm_shared_region_pager.c"),
            )
            record["language_trace"]["available"] = False
        if args.recovery_usb_core is not None:
            record["elden_recovery_usb_sources"] = stage_recovery_usb(
                source, args.recovery_usb_core.resolve(strict=True), previous is None, rpmh_read)
        if args.adreno830_gmu_firmware is not None:
            image = args.adreno830_gmu_firmware.read_bytes()
            digest = hashlib.sha256(image).hexdigest()
            if digest != "1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5":
                raise ValueError("expected the pinned Elden gen80000_gmu.bin")
            driver = DEVICE / "drivers/Adreno830"
            kit = root / "UniXNU/kits/AdrenoFirmware"
            pexpert = source / "pexpert/arm"
            inputs = [kit / name for name in ("AdrenoFirmware.c", "AdrenoFirmware.h")]
            inputs.extend(driver / name for name in
                          ("EldenAdreno830.c", "EldenAdreno830.h", "EldenAdreno830Boot.inc"))
            soc = DEVICE / "soc/sm8750"
            inputs.extend(soc / name for name in ("include/Sm8750AdrenoResources.h",
                "Sm8750CmdDb.c", "include/Sm8750CmdDb.h", "LICENSE.Adreno830"))
            if rpmh_read:
                inputs.append(driver / "EldenAdreno830PowerProbe.inc")
                inputs.extend(soc / name for name in (
                    "Sm8750RpmhRead.c", "include/Sm8750RpmhRead.h", "Sm8750GpuPower.c",
                    "include/Sm8750GpuPower.h", "include/Sm8750Qup1Resources.h",
                    "Sm8750GpuSmmuSurvey.c", "include/Sm8750GpuSmmuSurvey.h"))
            if hfi_sources:
                inputs.extend(hfi_sources)
                inputs.extend(driver / name for name in (
                    "EldenAdreno830HfiConfig.c", "EldenAdreno830HfiConfig.h", "EldenAdreno830HfiBoot.inc"))
            for item in inputs:
                write_if_changed(pexpert / item.name, item.read_bytes())
            rows = (", ".join(f"0x{byte:02x}" for byte in image[i:i + 16])
                    for i in range(0, len(image), 16))
            image_header = pexpert / "EldenAdreno830GmuImage.h"
            hfi_define = "#define ELDEN_ADRENO830_HFI_PROBE 1\n" if hfi_sources else ""
            rpmh_define = "#define ELDEN_RECOVERY_RPMH_READ 1\n" if rpmh_read else ""
            write_if_changed(image_header, (hfi_define + rpmh_define + "static const uint8_t elden_gmu_image[] = {\n    " +
                                            ",\n    ".join(rows) + "\n};\n").encode())
            if previous is None:
                pe_init = pexpert / "pe_init.c"
                code = pe_init.read_text()
                marker = "void\nPE_lockdown_iokit(void)\n{\n"
                if code.count(marker) != 1:
                    raise ValueError("expected one PE_lockdown_iokit definition")
                pe_init.write_text(code.replace(marker, '#include "EldenAdreno830Boot.inc"\n\n'
                                                   + marker + "\telden_adreno830_probe();\n"))
            subprocess.run(["git", "-C", source, "add", "-N", "--", image_header,
                            *(pexpert / item.name for item in inputs)], check=True)
            record["elden_adreno830_probe"] = {
                "firmware_sha256": digest,
                "sources_sha256": {item.name: build.sha(item) for item in inputs},
            }
        source_diff = subprocess.check_output(["git", "-C", source, "diff", "HEAD", "--binary"])
        record["source_diff_sha256"] = hashlib.sha256(source_diff).hexdigest()
        record["kernel_page_bytes"] = kernel["page_bytes"]
        record["elden_profile_inputs"] = profile_inputs
        record["elden_staged_header_inputs"] = staged_headers
        record["elden_bootstrap"] = {
            "page_bytes": kernel["page_bytes"],
            "arm64_large_memory": kernel["arm64_large_memory"],
            "link_address": kernel["link_address"],
            **overlay_hashes,
            "bootstrap_trace_header_sha256": build.sha(
                source / "osfmk/arm64/sheng_bootstrap_trace.h"),
            "hardware_verified": False,
        }
        build.save(record)
        return record

    def run_elden(command, *run_args, **kwargs):
        # Change only the make configuration for this isolated build.
        flags = {
            "ARM64_LARGE_MEMORY=1": f"ARM64_LARGE_MEMORY={int(kernel['arm64_large_memory'])}",
            "ARM64_LINK_ADDR=0xfffffe0007004000": f"ARM64_LINK_ADDR={kernel['link_address']}",
        }
        return run([flags.get(item, item) for item in command], *run_args, **kwargs)

    build.prepare = prepare_elden
    build.run = run_elden
    build.stage_file = stage_elden_file
    sys.argv = [sys.argv[0], "--arch", kernel["architecture"]]
    if args.prepare_only:
        sys.argv.append("--prepare-only")
        build.main()
        return
    sys.argv.extend(["--link-kernel", "--kdk-root", str(args.kdk_root.resolve(strict=True)),
                     "-j", str(args.jobs)])
    if args.elden_ufs_device is not None:
        device = args.elden_ufs_device.resolve(strict=True)
        sys.argv.extend(["--build-ufs-driver", "--elden-ufs-device", str(device)])
        if args.elden_recovery_contract is not None:
            sys.argv.extend(["--elden-recovery-contract", str(args.elden_recovery_contract.resolve(strict=True))])
        if args.build_gop_display:
            sys.argv.extend(["--build-gop-display", "--elden-gop-board", str(device / "board.toml")])
    build.main()
    symbols = set(build.capture("xcrun", "nm", "-j", str(output / "kernel.development.sheng")).splitlines())
    for required in ("_cs_native4k_hash_cache_version", "_vm_shared_region_slide_part_v5",
                     "_sheng_bootstrap_user_l0", "_sheng_kernel_pt_attr",
                     "_sheng_ce_stack_remaining"):
        if required not in symbols:
            raise ValueError(f"the linked kernel is missing {required}")


if __name__ == "__main__":
    main()
