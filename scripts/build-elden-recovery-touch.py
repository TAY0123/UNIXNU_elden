#!/usr/bin/env python3
"""Add Elden's ARM64e touch driver to the paired Recovery UFS companion."""

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile


DEVICE = Path(__file__).resolve().parents[1]
C_SOURCES = ("EldenNovatekHid.c", "EldenNovatekTouch.c", "EldenNovatekSpi4.c", "GeniSpi.c")
CXX_SOURCES = ("EldenNovatekController.cpp", "EldenNovatekDigitizer.cpp")
WARNINGS = ("-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-Wno-c99-designator", "-Wno-reorder-init-list",
            "-Wno-deprecated-enum-enum-conversion", "-Wno-unnecessary-virtual-specifier")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compile_objects(native: Path, directory: Path) -> list[Path]:
    recipes = list((native / "obj").glob("**/IOPlatformExpert.cpo.json"))
    if len(recipes) != 1:
        raise ValueError("requires one paired XNU IOPlatformExpert compile recipe")
    recipe = json.JSONDecoder().raw_decode(recipes[0].read_text())[0]
    sdk = Path(subprocess.check_output(
        ["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip())
    headers = sdk / "System/Library/Frameworks/Kernel.framework/Versions/A/Headers"
    cxx = []
    arguments = iter(recipe["arguments"])
    for arg in arguments:
        if arg in ("-o", "-MF", "-MT", "-MQ", "-MJ"):
            next(arguments)
        elif arg == recipe["file"] or arg in ("-c", "-MD", "-MMD", "-MP"):
            continue
        elif arg == "-Xclang":
            option = next(arguments)
            if not option.startswith("-W"):
                cxx.extend((arg, option))
        elif not arg.startswith("-W"):
            cxx.append(arg)
    cxx[1:1] = ["-I", str(directory / "include")]
    cxx.extend(("-I", str(native / "source/osfmk"), "-I", str(headers), *WARNINGS))
    c = ["xcrun", "clang", "-target", "arm64e-apple-macos26.5", "-mkernel",
         "-isysroot", str(sdk), "-Wall", "-Wextra", "-Werror", "-Wimplicit-int-conversion",
         "-ffreestanding", "-fno-stack-protector"]
    objects = []
    for names, command in ((CXX_SOURCES, cxx), (C_SOURCES, c)):
        for name in names:
            source = directory / name
            obj = source.with_suffix(".o")
            subprocess.run([*command, "-c", str(source), "-o", str(obj)],
                           cwd=recipe["directory"], check=True)
            objects.append(obj)
    return objects


def build(native: Path, geni: Path, reference: Path, hid_headers: Path, output: Path) -> None:
    ufs = native / "ufs-driver"
    record = json.loads((ufs / "build.json").read_text())
    info = plistlib.loads((ufs / "Info.plist").read_bytes())
    hid = json.loads(reference.read_text())
    hid_abi = hid["abi"]
    bundle = "org.blu-sharky.NativeUfs"
    if (record.get("bundle") != bundle or info.get("CFBundleIdentifier") != bundle or
            record.get("elden_ufs", {}).get("board") != "elden" or
            record.get("binary_sha256") != sha(ufs / "NativeUfs.macho") or
            not record.get("native_gop_display") or hid_abi.get("build") != "25F71"):
        raise ValueError("requires the paired Elden Recovery UFS and GOP build")
    if output.exists():
        raise FileExistsError(output)

    board = DEVICE / "drivers/EldenNovatekTouch"
    sources = [*(board / name for name in (
        *CXX_SOURCES, *C_SOURCES[:-1], "EldenNovatekDigitizer.h", "EldenNovatekHid.h",
        "EldenNovatekSpi4.h", "EldenNovatekTouch.h")),
        geni / "GeniSpi.c", geni / "GeniSpi.h", DEVICE / "soc/sm8750/include/Sm8750Qup1Resources.h"]
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        destination = Path(temp) / "ufs-driver"
        shutil.copytree(ufs, destination)
        touch = destination / "elden-touch"
        touch.mkdir()
        for path in sources:
            shutil.copyfile(path, touch / path.name)
        source_hashes = {path.name: sha(touch / path.name) for path in sources}
        include = touch / "include/IOKit/hid"
        include.mkdir(parents=True)
        for name, entry in hid["apple_headers"]["files"].items():
            path = include / name
            shutil.copyfile(hid_headers / name, path)
            if sha(path) != entry["canonical_sha256"]:
                raise ValueError(f"HID header differs from its paired reference: {name}")
            source_hashes[path.relative_to(touch).as_posix()] = entry["canonical_sha256"]
        objects = compile_objects(native, touch)
        binary = destination / "NativeUfs.macho"
        subprocess.run(["xcrun", "ld", "-arch", "arm64e", "-kext", "-kexts_use_stubs",
                        "-platform_version", "macos", "26.5", "26.5",
                        "-undefined", "dynamic_lookup", "-fixup_chains", "-no_adhoc_codesign",
                        "-headerpad", "0x1000", str(destination / "NativeUfs.o"),
                        *map(str, objects), "-o", str(binary)], check=True)
        info["IOKitPersonalities"]["EldenNovatekController"] = dict(
            CFBundleIdentifier=bundle, IOClass="EldenNovatekController",
            IOProviderClass="IOPlatformDevice", IONameMatch="elden-novatek",
            IOMatchCategory="EldenNovatekController", IOProbeScore=500)
        info["IOKitPersonalities"]["EldenNovatekEventDriver"] = dict(
            CFBundleIdentifier="com.apple.iokit.IOHIDFamily", IOClass="IOHIDEventDriver",
            IOProviderClass="IOHIDInterface", IOPropertyMatch={"Product": "Elden Touch Screen"},
            DeviceUsagePairs=[dict(DeviceUsagePage=13, DeviceUsage=4)], IOProbeScore=3000)
        info["OSBundleLibraries"]["com.apple.iokit.IOHIDFamily"] = "2.0.0"
        record["binary_sha256"] = sha(binary)
        record["elden_touch"] = dict(
            default_off=True, boot_argument="elden-touch=1", abi=hid_abi,
            files=source_hashes)
        (destination / "Info.plist").write_bytes(plistlib.dumps(info))
        (destination / "build.json").write_text(json.dumps(record, indent=2) + "\n")
        destination.rename(output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-build", type=Path, required=True)
    parser.add_argument("--geni-spi-source", type=Path, required=True)
    parser.add_argument("--hid-reference", type=Path, required=True)
    parser.add_argument("--hid-headers", type=Path, required=True,
                        help="directory with the paired Apple private HID headers")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build(args.native_build.resolve(strict=True), args.geni_spi_source.resolve(strict=True),
          args.hid_reference.resolve(strict=True), args.hid_headers.resolve(strict=True),
          args.output.resolve())


if __name__ == "__main__":
    main()
