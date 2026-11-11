#!/usr/bin/env python3
"""Build the phase-2.3 FAT payload volume (56 MiB by default).

The volume holds ``EFI/UniXNU/unixnu.toml`` plus the phase-2.2 prelinked
``EFI/UniXNU/Payloads/kernel.macho`` and its ``.adt``.  It is the final payload
embedded after the firmware volume by ``scripts/build_phase2_image.py
--embed-payload --kind boot`` and published by ``EldenPayloadDxe``.

The filesystem is FAT16, produced by the same writer as the phase-1 payload.
EnhancedFatDxe derives the FAT type from the cluster count, so a 56 MiB FAT32
volume is only accepted above ``FAT_MAX_FAT16_CLUSTER`` (0xFFF5) clusters and a
smaller FAT32 volume is rejected with EFI_VOLUME_CORRUPTED.  FAT16 keeps both
the 8 MiB probe and this volume on the geometry already proven on the device in
phase 1.

The config comes from the tracked ``package/phase2.3-unixnu.toml`` so the menu
scale and any other UI policy are reviewable.  The kernel and ADT are copied out
of an existing payload volume with ``--from-image`` (or given with
``--kernel``/``--adt``), because the prelinked kernel is a build output that is
not committed.

Pass an ARM64 EFI-stub Linux kernel and its matching DTB to include a Linux
entry whose initramfs is built into the kernel.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from darwinboot_preflight import adt_walk, set_owned_memory_size, validate_trust_cache, with_trust_cache_placeholder
from make_payload import Fat16Builder, add_long_name_file


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out" / "phase2.3"
VOLUME = b"ELDENRAM"
VOLUME_ID = 0x53484E47
SIZE_MIB = 56
DEFAULT_CONFIG = ROOT / "package" / "phase2.3-unixnu.toml"
KERNEL_MACHO_BYTES = 54050816
KERNEL_MACHO_SHA256 = "65a3e799e1eb7f631a3b146dfc890ef5a8147c8fc613ee383a1665b398fb1ef9"
CONFIG_IMAGE_PATH = "EFI/UniXNU/unixnu.toml"
PAYLOAD_IMAGE_DIR = "EFI/UniXNU/Payloads"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def mtools(*arguments: str) -> None:
    if shutil.which("mcopy") is None:
        raise SystemExit("missing mtools tool: mcopy")
    subprocess.run(list(arguments), check=True)


def extract_from_image(image: Path, directory: Path) -> dict[str, Path]:
    """Copy the kernel and ADT out of an existing FAT volume."""
    targets = {
        "kernel": (f"{PAYLOAD_IMAGE_DIR}/kernel.macho", directory / "kernel.macho"),
        "adt": (f"{PAYLOAD_IMAGE_DIR}/kernel.macho.adt", directory / "kernel.macho.adt"),
    }
    for image_path, local in targets.values():
        mtools("mcopy", "-o", "-m", "-i", str(image), f"::{image_path}", str(local))
    files = {name: local for name, (_, local) in targets.items()}
    if adt_walk(files["adt"].read_bytes())["trustcache"] is not None:
        cache = directory / "kernel.macho.trustcache"
        mtools("mcopy", "-o", "-m", "-i", str(image), f"::{PAYLOAD_IMAGE_DIR}/{cache.name}", str(cache))
        files["trustcache"] = cache
    return files


def build_payload(
    output: Path,
    config: Path,
    kernel: Path,
    adt: Path,
    size_mib: int,
    expect_kernel_bytes: int = KERNEL_MACHO_BYTES,
    expect_kernel_sha256: str = KERNEL_MACHO_SHA256,
    *,
    trust_cache: Path | None = None,
    linux_kernel: Path | None = None,
    linux_dtb: Path | None = None,
    slot_switch: Path | None = None,
    usb_menu_driver: Path | None = None,
) -> None:
    data = kernel.read_bytes()
    if len(data) != expect_kernel_bytes or sha256(kernel) != expect_kernel_sha256:
        raise SystemExit(
            f"kernel.macho is {len(data)} bytes sha256={sha256(kernel)}, "
            f"expected {expect_kernel_bytes} bytes sha256={expect_kernel_sha256}")
    if adt.stat().st_size == 0:
        raise SystemExit("kernel.macho.adt is empty")
    if trust_cache is not None:
        adt_data = adt.read_bytes()
        if with_trust_cache_placeholder(adt_data) != adt_data:
            raise ValueError("TrustCache requires a matching ADT placeholder")
        validate_trust_cache(trust_cache.read_bytes())
    if (linux_kernel is None) != (linux_dtb is None):
        raise SystemExit("pass --linux-kernel and --linux-dtb together")
    if linux_kernel is not None and linux_dtb is not None:
        for linux_file in (linux_kernel, linux_dtb):
            if not linux_file.is_file() or linux_file.stat().st_size == 0:
                raise SystemExit(f"missing or empty Linux file: {linux_file}")
    if "Payloads/EldenBootSlot.efi" in config.read_text() and slot_switch is None:
        raise SystemExit("the native Elden slot entries require --slot-switch")
    if slot_switch is not None and (not slot_switch.is_file() or slot_switch.stat().st_size == 0):
        raise SystemExit(f"missing or empty Elden slot application: {slot_switch}")
    if usb_menu_driver is not None and (not usb_menu_driver.is_file() or usb_menu_driver.stat().st_size == 0):
        raise SystemExit(f"missing or empty Elden USB menu driver: {usb_menu_driver}")
    total_sectors = size_mib * 1024 * 1024 // 512
    builder = Fat16Builder(total_sectors, label=VOLUME, media_id=VOLUME_ID)
    efi = builder.add_directory(0, "EFI")
    unixnu = builder.add_directory(efi, "UNIXNU")
    builder.add_directory(unixnu, "PAYLOADS")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(builder.finish())
    add_long_name_file(output, config, CONFIG_IMAGE_PATH)
    add_long_name_file(output, kernel, f"{PAYLOAD_IMAGE_DIR}/kernel.macho")
    add_long_name_file(output, adt, f"{PAYLOAD_IMAGE_DIR}/kernel.macho.adt")
    if trust_cache is not None:
        add_long_name_file(output, trust_cache, f"{PAYLOAD_IMAGE_DIR}/kernel.macho.trustcache")
    if linux_kernel is not None and linux_dtb is not None:
        add_long_name_file(output, linux_kernel, f"{PAYLOAD_IMAGE_DIR}/linux.efi")
        add_long_name_file(output, linux_dtb, f"{PAYLOAD_IMAGE_DIR}/linux.dtb")
    if slot_switch is not None:
        add_long_name_file(output, slot_switch, f"{PAYLOAD_IMAGE_DIR}/EldenBootSlot.efi")
        mailbox = b'ELDFDT01' + struct.pack('<IIQ', 1, 0, 0)
        with output.open('r+b') as volume:
            volume.seek(0x1c0)
            if any(volume.read(len(mailbox))):
                raise SystemExit('FAT boot sector has no room for the live FDT mailbox')
            volume.seek(0x1c0)
            volume.write(mailbox)
    if usb_menu_driver is not None:
        add_long_name_file(output, usb_menu_driver, f"{PAYLOAD_IMAGE_DIR}/EldenUsbMenu.efi")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUT / "payload.img")
    parser.add_argument("--size-mib", type=int, default=SIZE_MIB)
    parser.add_argument("--from-image", type=Path)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--adt", type=Path)
    parser.add_argument("--trust-cache", type=Path, help="paired DarwinBoot trust-cache bundle")
    parser.add_argument("--owned-memory-size", type=lambda value: int(value, 0),
                        help="override the ADT's requested contiguous XNU RAM window in bytes")
    parser.add_argument("--linux-kernel", type=Path, help="ARM64 EFI-stub kernel extracted from boot.img")
    parser.add_argument("--linux-dtb", type=Path, help="DTB from the same Linux build")
    parser.add_argument("--slot-switch", type=Path, help="built EldenBootSlot.efi application")
    parser.add_argument("--usb-menu-driver", type=Path, help="built EldenUsbMenu.efi driver")
    parser.add_argument("--kernel-bytes", type=int, default=KERNEL_MACHO_BYTES,
                        help="expected kernel size; override for a probe kernel")
    parser.add_argument("--kernel-sha256", default=KERNEL_MACHO_SHA256,
                        help="expected kernel SHA-256; override for a probe kernel")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="elden-payload-") as directory:
        stage = Path(directory)
        if args.from_image is not None:
            files = extract_from_image(args.from_image, stage)
        elif args.kernel and args.adt:
            files = {"kernel": args.kernel, "adt": args.adt}
        else:
            raise SystemExit("pass --from-image or --kernel/--adt")
        if args.owned_memory_size is not None:
            adt = stage / "kernel-owned-memory.adt"
            try:
                adt.write_bytes(set_owned_memory_size(files["adt"].read_bytes(), args.owned_memory_size))
            except ValueError as error:
                parser.error(str(error))
            files["adt"] = adt
        trust_cache = args.trust_cache or files.get("trustcache")
        if trust_cache is not None:
            adt = stage / "kernel-trust-cache.adt"
            adt.write_bytes(with_trust_cache_placeholder(files["adt"].read_bytes()))
            files["adt"] = adt
        if not args.config.is_file():
            raise SystemExit(f"missing config: {args.config}")
        config = args.config
        if args.usb_menu_driver is not None:
            config = stage / 'usb-unixnu.toml'
            config.write_text(args.config.read_text() + '\n[[driver]]\n'
                'path = "Payloads/EldenUsbMenu.efi"\nargs = "elden"\nconnect = false\n')
        build_payload(
            args.output,
            config,
            files["kernel"],
            files["adt"],
            args.size_mib,
            args.kernel_bytes,
            args.kernel_sha256,
            trust_cache=trust_cache,
            linux_kernel=args.linux_kernel,
            linux_dtb=args.linux_dtb,
            slot_switch=args.slot_switch,
            usb_menu_driver=args.usb_menu_driver,
        )
        print(f"kernel:  {files['kernel'].stat().st_size} bytes sha256={sha256(files['kernel'])}")

    print(f"payload: {args.output} ({args.output.stat().st_size} bytes)")
    print(f"sha256:  {sha256(args.output)}")


if __name__ == "__main__":
    main()
