#!/usr/bin/env python3
"""Prepare the Elden 7.2.8-y700-r2-androidcrypt1 Image for its EFI stub."""

import argparse
import hashlib
from pathlib import Path


IMAGE_SHA256 = "6aed9425e69c1e07ba65d3467738a6009874fe75d1fd50215483d333ba0634c3"
CMDLINE_OFFSETS = (0x3078FE3, 0x4153538, 0x4154265)
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


def enable_diagnostics(image: bytearray, keep_boot_console: bool) -> None:
    first = CMDLINE_OFFSETS[0]
    original = bytes(image[first:image.index(0, first)])
    if (not original.startswith(b"console=ttyGS0,115200 console=tty0 ")
            or b" panic=10 efi=noruntime " not in original):
        raise ValueError("unexpected built-in command line")
    diagnostic = original.replace(b"console=ttyGS0,115200", b"earlycon=efifb", 1)
    diagnostic = diagnostic.replace(b" panic=10 efi=noruntime ", b" panic=0 efi=noruntime,debug ", 1)
    if keep_boot_console:
        # The EFI payload has no bootconfig trailer. Reuse its argument
        # space for keep_bootcon and leave EFI runtime disabled.
        diagnostic = diagnostic.replace(b" bootconfig ", b" keep_bootcon ", 1)
        diagnostic = diagnostic.replace(b" efi=noruntime,debug ", b" efi=noruntime ", 1)
    if len(diagnostic) > len(original):
        raise ValueError("diagnostic command line exceeds the compiled string")
    for start in CMDLINE_OFFSETS:
        end = start + len(original)
        if image[start:end + 1] != original + b"\0":
            raise ValueError(f"unexpected command-line copy at {start:#x}")
        image[start:end] = diagnostic.ljust(len(original))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="original Linux Image from boot_b")
    parser.add_argument("output", type=Path, help="new EFI-only Linux Image")
    parser.add_argument("--diagnostics", action="store_true",
                        help="enable EFI framebuffer logging and hold Linux panics on screen")
    parser.add_argument("--keep-boot-console", action="store_true",
                        help="retain the EFI framebuffer after the normal console registers")
    args = parser.parse_args()
    if args.keep_boot_console and not args.diagnostics:
        parser.error("--keep-boot-console requires --diagnostics")

    image = bytearray(args.image.read_bytes())
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        parser.error("unsupported Linux Image SHA-256")
    for offset, original, replacement in PATCHES:
        end = offset + len(original)
        if image[offset:end] != original:
            parser.error(f"unexpected instructions at {offset:#x}")
        image[offset:end] = replacement
    if args.diagnostics:
        enable_diagnostics(image, args.keep_boot_console)

    with args.output.open("xb") as output:
        output.write(image)


if __name__ == "__main__":
    main()
