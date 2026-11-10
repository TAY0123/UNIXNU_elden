#!/usr/bin/env python3
"""Build the Elden 4 KiB ARM64e XNU candidate on the macOS build host."""

import argparse
from pathlib import Path
import shutil
import sys


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sheng-root", type=Path, required=True)
    parser.add_argument("--kdk-root", type=Path, required=True)
    parser.add_argument("-j", "--jobs", type=int, default=8, choices=range(1, 33))
    args = parser.parse_args()

    root = args.sheng_root.resolve(strict=True)
    sys.path.insert(0, str(root / "scripts"))
    import build_macos26_xnu as build

    output = root / "out/elden-macos26-4k-bootstrap"
    output.mkdir(parents=True, exist_ok=True)
    overlay = (Path(__file__).resolve().parents[1] / "patches/macos-recovery/"
               "elden-macos26-4k-user-va.patch")
    staged_overlay = output / overlay.name
    shutil.copyfile(overlay, staged_overlay)
    prepare = build.prepare
    run = build.run

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
        build.PATCHES.append(staged_overlay)

        record = prepare()
        header = output / "source/pexpert/pexpert/arm64/SHENG.h"
        if "__ARM_16K_PG__" in header.read_text():
            raise ValueError("staged SHENG source still requests the 16 KiB granule")
        record["elden_bootstrap"] = {
            "page_bytes": 4096,
            "arm64_large_memory": False,
            "link_address": "0xfffffff007004000",
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
    build.main()


if __name__ == "__main__":
    main()
