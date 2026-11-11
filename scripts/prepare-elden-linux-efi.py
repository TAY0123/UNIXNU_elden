#!/usr/bin/env python3
"""Prepare the Elden 7.2.8-y700-r2-androidcrypt1 Image for its EFI stub."""

import argparse
import hashlib
from pathlib import Path


IMAGE_SHA256 = "6aed9425e69c1e07ba65d3467738a6009874fe75d1fd50215483d333ba0634c3"
PATCHES = (
    # setup_arch: pass the physical __fdt_pointer saved from x0 by head.S.
    # Preserve x20 (kimage_voffset) and the call to setup_machine_fdt.
    (
        0x3E54D88,
        bytes.fromhex("291e00f0 29811391 880240f9 200108cb"),
        bytes.fromhex("290d00b0 20a945f9 1f2003d5 1f2003d5"),
    ),
    # Suppress the now-inaccurate "Using built-in device tree" printk.
    (0x3E54DA8, bytes.fromhex("36010797"), bytes.fromhex("1f2003d5")),
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="original Linux Image from boot_b")
    parser.add_argument("output", type=Path, help="new EFI-only Linux Image")
    args = parser.parse_args()

    image = bytearray(args.image.read_bytes())
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        parser.error("unsupported Linux Image SHA-256")
    for offset, original, replacement in PATCHES:
        end = offset + len(original)
        if image[offset:end] != original:
            parser.error(f"unexpected instructions at {offset:#x}")
        image[offset:end] = replacement

    with args.output.open("xb") as output:
        output.write(image)


if __name__ == "__main__":
    main()
