#!/usr/bin/env python3
"""Seal a WireGuard profile for the APK and write its separate provisioning key."""

import argparse
import os
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("assets", type=Path)
    parser.add_argument("key", type=Path)
    args = parser.parse_args()
    assets = args.assets.resolve()
    key_file = args.key.resolve()
    if key_file.is_relative_to(assets):
        parser.error("Keep the provisioning key outside the APK asset directory")
    profile = args.profile.read_bytes()
    if not profile or len(profile) >= 65_500:
        parser.error("Unexpected WireGuard profile size")

    os.umask(0o077)
    assets.mkdir(parents=True, exist_ok=True)
    key = AESGCM.generate_key(bit_length=256)
    nonce = os.urandom(12)
    with key_file.open("xb") as key_output:
        key_output.write(key)
    with (assets / "wireguard.conf.sealed").open("xb") as asset:
        asset.write(nonce + AESGCM(key).encrypt(nonce, profile, b"elden-wireguard-v1"))


if __name__ == "__main__":
    main()
