#!/usr/bin/env python3
"""Assemble a KUAL extension from the pinned Kindle kTerm release."""

import argparse
import hashlib
import io
from pathlib import Path
import stat
import tarfile
import zipfile


HERE = Path(__file__).resolve().parent
RELEASE_SHA256 = "d1ee896a919950bb12def6b68fd4e6d0ab7b6fc50c1e390bea3f316ff70be4ba"
SOURCE_SHA256 = "6c165b699a631403c46981d003d85fe7fff77cd2793ad1e5ea806c777f3e70c9"
SOURCE_ROOT = "kterm-2a0622a7a1d5a7796848505e97be49465db934ea"
EXTENSION = "extensions/kindle-terminal"
EXECUTABLES = {"bin/kterm", "bin/launch.sh", "bin/shell.sh"}


def checked_archive(path, digest):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"Unexpected SHA-256 for {path}")
    return io.BytesIO(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", type=Path, help="Official kTerm 2.6 ARMhf ZIP")
    parser.add_argument("source", type=Path, help="Pinned kTerm source tar.gz")
    parser.add_argument("output", type=Path, help="KUAL extension ZIP to write")
    args = parser.parse_args()

    try:
        with zipfile.ZipFile(checked_archive(args.release, RELEASE_SHA256)) as release:
            runtime = ("bin/kterm", "vte/terminfo/x/xterm", "vte/terminfo/x/xterm-256color")
            files = {name: release.read(f"kterm/{name}") for name in runtime}
        with tarfile.open(fileobj=checked_archive(args.source, SOURCE_SHA256), mode="r:gz") as source:
            with source.extractfile(f"{SOURCE_ROOT}/COPYING") as license_file:
                files["COPYING"] = license_file.read()
        for name in ("config.xml", "menu.json", "bin/launch.sh", "bin/shell.sh",
                     "layouts/keyboard.xml", "README.md"):
            files[name] = (HERE / name).read_bytes()
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, tarfile.TarError) as error:
        parser.error(str(error))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, mode="w") as package:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(f"{EXTENSION}/{name}")
            info.create_system = 3
            mode = 0o755 if name in EXECUTABLES else 0o644
            info.external_attr = (stat.S_IFREG | mode) << 16
            package.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)


if __name__ == "__main__":
    main()
