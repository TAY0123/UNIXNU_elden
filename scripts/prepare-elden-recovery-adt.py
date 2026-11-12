#!/usr/bin/env python3
"""Add the GOP display and optional touch provider to an Elden Recovery ADT."""

import argparse
import hashlib
from pathlib import Path
import sys
import tempfile

from phase2_1_audit import AdtNode, adt_child, parse_adt


GOP_NODE = AdtNode("native-gop", {
    "name": b"native-gop\0",
    "device_type": b"display-subsystem\0",
    "compatible": b"sheng,gop-framebuffer\0",
}, [])

TOUCH_FIRMWARE_BYTES = 253952
TOUCH_FIRMWARE_SHA256 = "413216d0e8738078414fa681262439ab6d1c181f02974215e0394f102fcc08dc"


def touch_node(path: Path) -> AdtNode:
    with path.open("rb") as stream:
        image = stream.read(TOUCH_FIRMWARE_BYTES + 1)
    if (len(image) != TOUCH_FIRMWARE_BYTES or
            hashlib.sha256(image).hexdigest() != TOUCH_FIRMWARE_SHA256):
        raise ValueError("requires the pinned Elden NT36536 CSOT firmware")
    return AdtNode("elden-novatek", {
        "name": b"elden-novatek\0",
        "device_type": b"touchscreen\0",
        "compatible": b"novatek,nt36536-ts\0",
        "novatek,firmware-image": image,
    }, [])


def add_node(parent: AdtNode, child: AdtNode) -> None:
    existing = [entry for entry in parent.children if entry.name == child.name]
    if existing:
        if existing != [child]:
            raise ValueError(f"existing {child.name} descriptor differs from the reference")
        return
    parent.children.append(child)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unixnu-root", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--touch-firmware", type=Path,
                        help="add the SPI4 touch provider with the local Elden CSOT firmware")
    args = parser.parse_args()

    sys.path.insert(0, str(args.unixnu_root.resolve(strict=True) / "scripts"))
    from unixnu.devicetree import node

    def encode(entry: AdtNode) -> bytes:
        return node(entry.name, [(key, value) for key, value in entry.properties.items()
                                 if key != "name"], [encode(child) for child in entry.children])

    original = args.input.read_bytes()
    expected = parse_adt(original)
    if expected.properties.get("compatible", b"").split(b"\0")[0] != b"lenovo,elden":
        raise ValueError("requires an Elden Recovery device tree")
    arm_io = adt_child(expected, "arm-io")
    add_node(arm_io, GOP_NODE)
    if args.touch_firmware is not None:
        add_node(arm_io, touch_node(args.touch_firmware))

    with tempfile.TemporaryDirectory(dir=args.output.parent) as directory:
        candidate = Path(directory) / "recovery.adt"
        encoded = encode(expected)
        if parse_adt(encoded) != expected:
            raise ValueError("ADT did not round-trip after adding its device descriptors")
        candidate.write_bytes(encoded)
        candidate.replace(args.output)


if __name__ == "__main__":
    main()
