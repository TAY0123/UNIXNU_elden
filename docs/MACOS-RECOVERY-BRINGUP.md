# Elden macOS Recovery bring-up

The reference target is macOS 26.5 build 25F71 with XNU 12377.121.6, ARM64e
and 16 KiB pages. Start with the pinned
[macOS-ForSheng 26.5 reference](https://github.com/CrossDarwin/macOS-ForSheng/tree/ca57db73c066162323e77156a9f0f1d72b1de4ff)
on QEMU's ARM `virt` machine. Keep the Elden iPadOS defconfig available
as the device baseline.

## Stage 1 · Linux host and emulator

Passed on 2026-11-10 with x86_64 Arch Linux, Python 3.13.14, Clang/LLD 23.1.1,
QEMU 11.1.1 with TCG, and EDK II 202608.

- ARM64 UEFI reached the boot manager in QEMU.
- Three EL1 cases passed. They exercised the 16 KiB table walk, the forced
  4 KiB fallback control, and exception recovery. All three confirmed PAC
  key switching and restoration for IA, IB, DA, DB and GA.
- Three 47-bit execution cases passed. They exercised instruction fetch,
  the undefined-instruction control, and the execute-never control.
- Elden's iPadOS defconfig resolved and assembled with UniXNU at
  `95ad78129203442c3c14c02336303adbddf8d0a3`. The porting record,
  DTS compilation, and 4 MiB phase-1 FAT payload verification also passed.

From the pinned reference checkout, with Clang, LLD and
`qemu-system-aarch64` on PATH:

```sh
uv venv --python 3.13 .venv
uv pip install --python .venv/bin/python -r requirements.txt -r requirements-test.txt
.venv/bin/python scripts/test_el1_native.py \
  --clang "$(command -v clang)" --qemu "$(command -v qemu-system-aarch64)"
.venv/bin/python scripts/test_el1_native.py --layout47 \
  --clang "$(command -v clang)" --qemu "$(command -v qemu-system-aarch64)"
```

The reference writes logs and `results.json` to `out/el1-native-qemu/` and
`out/el1-layout47-qemu/`. These probes execute on QEMU. Elden's EL1 access,
PAC behavior and translation regime require their own tablet evidence.

## Stage 2 · Paired Recovery inputs

Passed on 2026-11-10. The pinned 25F71 IPSW passed SHA-256, build and board
checks. BaseSystem reported macOS 26.5 build 25F71 and contained ARM64e
`launchd` and `dyld`. The read-only QEMU image is 1,577,092,608 bytes, with
SHA-256 `6199e53d3cf60c59790e507851ef48141dbfa23875fdde5e838446984ed56881`.
The extracted BaseSystem root hash matched the original IPSW manifest.

From the pinned reference checkout, after its core sources are available:

```sh
media=private/apple/macos26
.venv/bin/python scripts/fetch_inputs.py --config config/macos26.json --jobs 8
.venv/bin/python scripts/inspect_macos26.py
.venv/bin/python scripts/prepare_macos26_recovery.py --ipsw-tool "$IPSW_TOOL"
.venv/bin/python scripts/prepare_macos26_block.py --output "$media/recovery-raw"
.venv/bin/python scripts/prepare_macos26_root_hash.py \
  --disk "$media/recovery-raw" --output "$media/BaseSystem.root-hash"
```

Set `IPSW_TOOL` to the executable verified against the reference's
`config/ipsw-tool.json`. The KDK DMG also passed its pinned SHA-256 check.
KDK package signature and kernel UUID checks run on the macOS build host.
See the [pinned reference procedure](https://github.com/CrossDarwin/macOS-ForSheng/blob/ca57db73c066162323e77156a9f0f1d72b1de4ff/docs/bringup.md).

## Stage 3 · Reference Recovery boot in QEMU

Use a matching ARM64e kernel build and its build record from the reference
macOS toolchain, alongside the reference DarwinBoot and the paired Recovery
inputs. Capture kernel entry, root mount, `launchd`, and the graphical
Recovery language chooser as separate checkpoints.

## Stage 4 · Elden hardware

Qualify EL1 PAC and 16 KiB translation on the tablet before adapting the
loader, memory map, and UFS path. Use the repository's owner-operated
RAM-boot procedure for hardware tests.
