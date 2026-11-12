#!/usr/bin/env python3
"""Package the Kindle extension and its ARMhf runtimes."""

import argparse
from pathlib import Path
import re
import stat
import subprocess
import zipfile


HERE = Path(__file__).resolve().parent
PREFIX = "extensions/elden-tools"


def armhf_libraries(sysroot, programs):
    lib = sysroot / "usr/lib/arm-linux-gnueabihf"
    paths = {"ld-linux-armhf.so.3": lib / "ld-linux-armhf.so.3"}
    pending = [*programs, *paths.values()]
    while pending:
        binary = pending.pop()
        dynamic = subprocess.check_output(["readelf", "-d", binary], text=True)
        for name in re.findall(r"\(NEEDED\).*\[(.+?)\]", dynamic):
            if name in paths:
                continue
            candidates = (lib / name, lib / "android" / name)
            path = next((path for path in candidates if path.is_file()), None)
            if path is None:
                raise ValueError(f"Missing ARMhf runtime library {name}")
            paths[name] = path
            pending.append(path)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sysroot", required=True, type=Path, help="Extracted Debian ARMhf fastboot packages")
    parser.add_argument("--usb-reader", required=True, type=Path, help="ARMhf build of usb-cdc.c")
    parser.add_argument("--collector", required=True, type=Path, help="ARMv7 build of collector")
    parser.add_argument("--wireguard", required=True, type=Path, help="Directory with wg-arm, wireguard-go and lib/")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    fastboot = args.sysroot / "usr/lib/android-sdk/platform-tools/fastboot"
    files = {name: HERE / name for name in ("config.xml", "menu.json", "README.md")}
    files.update({f"bin/{name}": HERE / "bin" / name
                  for name in ("control.sh", "stage.sh", "fastboot", "usb-cdc")})
    files.update({"bin/runtime/fastboot": fastboot, "bin/runtime/usb-cdc": args.usb_reader,
                  "bin/collector": args.collector})
    try:
        libraries = armhf_libraries(args.sysroot, (fastboot, args.usb_reader))
        files.update({f"bin/runtime/{name}": path for name, path in libraries.items()})
        for name in ("wg-arm", "wireguard-go"):
            files[f"bin/{name}"] = args.wireguard / name
        for name in ("ld-musl-armhf.so.1", "libc.musl-armv7.so.1"):
            files[f"bin/wg-lib/{name}"] = args.wireguard / "lib" / name
        for license_file in (args.sysroot / "usr/share/doc").glob("*/copyright"):
            if license_file.is_file():
                files[f"licenses/{license_file.parent.name}.txt"] = license_file
        payload = {name: path.read_bytes() for name, path in files.items()}
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w") as package:
        for name, data in sorted(payload.items()):
            info = zipfile.ZipInfo(f"{PREFIX}/{name}")
            info.create_system = 3
            mode = 0o755 if name.startswith("bin/") else 0o644
            info.external_attr = (stat.S_IFREG | mode) << 16
            package.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)


if __name__ == "__main__":
    main()
