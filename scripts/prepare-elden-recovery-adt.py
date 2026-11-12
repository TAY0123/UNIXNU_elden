#!/usr/bin/env python3
"""Add the inherited GOP display descriptor to an Elden Recovery ADT."""

import argparse
from pathlib import Path
import sys
import tempfile

from phase2_1_audit import AdtNode, adt_child, parse_adt


GOP_NODE = AdtNode("native-gop", {
    "name": b"native-gop\0",
    "device_type": b"display-subsystem\0",
    "compatible": b"sheng,gop-framebuffer\0",
}, [])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unixnu-root", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
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
    existing = [child for child in arm_io.children if child.name == GOP_NODE.name]
    if existing and existing != [GOP_NODE]:
        raise ValueError("existing GOP display descriptor differs from the reference")
    if not existing:
        arm_io.children.append(GOP_NODE)

    with tempfile.TemporaryDirectory(dir=args.output.parent) as directory:
        candidate = Path(directory) / "recovery.adt"
        encoded = encode(expected)
        if parse_adt(encoded) != expected:
            raise ValueError("ADT did not round-trip after adding the display descriptor")
        candidate.write_bytes(encoded)
        candidate.replace(args.output)


if __name__ == "__main__":
    main()
