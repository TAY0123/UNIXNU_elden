#!/usr/bin/env python3
"""Build the Elden 4 KiB ARM64e XNU candidate on the macOS build host."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sheng-root", type=Path, required=True)
    parser.add_argument("--kdk-root", type=Path, required=True)
    parser.add_argument("--output-name", default="elden-macos26-4k-bootstrap",
                        help="build directory name under the reference checkout's out directory")
    parser.add_argument("--elden-ufs-device", type=Path,
                        help="also compile the read-only UFS companion from this device checkout")
    parser.add_argument("-j", "--jobs", type=int, default=8, choices=range(1, 33))
    args = parser.parse_args()

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
        source = output / "source"
        subprocess.run(["git", "-C", source, "apply", trace_overlay], check=True)
        source_diff = subprocess.check_output(["git", "-C", source, "diff", "HEAD", "--binary"])
        record["source_diff_sha256"] = hashlib.sha256(source_diff).hexdigest()
        record["elden_bootstrap"] = {
            "page_bytes": 4096,
            "arm64_large_memory": False,
            "link_address": "0xfffffff007004000",
            "bootstrap_trace_overlay_sha256": build.sha(trace_overlay),
            "bootstrap_trace_header_sha256": build.sha(
                source / "osfmk/arm64/sheng_bootstrap_trace.h"),
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
        sys.argv.extend(["--build-ufs-driver", "--elden-ufs-device",
                         str(args.elden_ufs_device.resolve(strict=True))])
    build.main()


if __name__ == "__main__":
    main()
