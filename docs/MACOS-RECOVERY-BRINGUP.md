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

The pinned ARM64e XNU source passed preparation with 78 patches. The
DarwinBoot EFI driver built on Linux with Clang/LLD 23.1.1. COFF inspection
confirmed that `DarwinEnter` and `DarwinEnterEnd` share a section, so the
loader can use the end label to bound cache cleaning. The original
function macro put the end label in an empty section that LLD discarded.

After applying the reference's integration patches and building its Mu
BaseTools, apply the Elden compatibility patch and build the loader:

```sh
git -C UniXNU/drivers/DarwinBoot apply \
  "$ELDEN_REPO/patches/macos-recovery/darwinboot-handoff-end-label.patch"
bash scripts/build_native_loader.sh "$LLVM_BIN"
```

Set `ELDEN_REPO` to the device checkout and `LLVM_BIN` to the directory
containing Clang, LLD and the LLVM binary tools. The verified driver is at
`out/native-loader/RELEASE_CLANGPDB/AARCH64/DarwinBoot.efi`, with SHA-256
`810082d8a58723d462a8da5952bb6eabea451eee20e4d9cbaea954d1dcb38a1e`.

The Apple silicon build host verified the Apple KDK package signature
and both matching kernel/DWARF pairs. Xcode 27.0 build `27A5194q` and SDK
27.0 linked the SHENG ARM64e kernel. The Linux host rechecked the kernel,
dSYM UUID, link map, and KDK report after transfer. The kernel UUID is
`4777A227-486E-3F94-8835-7A4CCEB34A38` and its SHA-256 is
`5002cd970525296c7667fff624e31f435ae21f0f2c38c17994ae081159f9069b`.

QEMU reached the real `arm_init` via UEFI and DarwinBoot. The debugger
matched the kernel's instructions at the checkpoint and observed an
enabled EL1 MMU with 16 KiB translation granules and a 47-bit address
regime. The separate raw-entry probe also reached the first C entry.

```sh
.venv/bin/python scripts/observe_macos26_kernel.py \
  --kernel-build "$KERNEL_BUILD" --firmware "$QEMU_EFI" \
  --output out/elden-reference-arm-init --checkpoint arm_init --timeout 75
```

Set `KERNEL_BUILD` to the directory containing the linked kernel, build
record, map and dSYM, and `QEMU_EFI` to the ARM64 UEFI code FD. The
reference's pinned Sheng input archive supplies the QEMU boot manager.

### Recovery userspace

On 2026-11-10, QEMU TCG with one Cortex-A710 and 4 GiB of RAM mounted the
paired APFS BaseSystem over VirtIO and matched the disk's first-sector
hash. The debugger observed the original ARM64e `dyld` entry and matched
the bytes at `launchd`'s `LC_MAIN` entry after its first instruction ran
in EL0. The console reported `recoveryosd` and `coreservicesd` started.
The 180-second run ended at the observation limit.

The guest used the reference's QEMU development-root policy and a separate
ISC test disk with a disposable QCOW2 overlay. The Recovery candidate SHA-256
was `86090c41cdb612d1261f7812504628c930012df60d3dfae084ea1d89098a6026`.
Prepare `ISC_DISK` with the reference's `prepare_qemu_isc.py` on macOS.
Set `ROOT_VOLUME_UUID` to the APFS volume UUID from the paired raw disk.
The observer's `--let-vector-log` option lets XNU handle its initial
synchronous page fault and continue.

```sh
media=private/apple/macos26
candidate=out/elden-reference-recovery-candidate
.venv/bin/python scripts/emit_macos26_security.py \
  --kernel-build "$KERNEL_BUILD" --qemu-only --recovery-components \
  --storage-driver "$KERNEL_BUILD/storage-driver" --output "$candidate"
.venv/bin/python scripts/observe_macos26_kernel.py \
  --kernel-build "$candidate" --firmware "$QEMU_EFI" \
  --output out/elden-reference-launchd --checkpoint thread_setentrypoint \
  --follow-init-entry --follow-launchd-main --continue-after-launchd-main \
  --virtio-block-image "$media/recovery-raw" --root-volume-uuid "$ROOT_VOLUME_UUID" \
  --root-hash-payload "$media/BaseSystem.root-hash" --qemu-development-root --qemu-no-ignition \
  --isc-block-image "$ISC_DISK" --isc-write-overlay \
  --qemu-memory-mib 4096 --owned-memory-mib 2048 --timeout 180 \
  --capture-screen --early-console --boot-progress --let-vector-log
```

The graphical checkpoint remains open. WindowServer started four times
and exited three times. `recoveryosd` reported that `mountrecursive`
exited with status 255. The language chooser had zero starts at the final
sample.

## Stage 4 · Elden hardware

Qualify EL1 PAC and 16 KiB translation on the tablet before adapting the
loader, memory map, and UFS path. Use the repository's owner-operated
RAM-boot procedure for hardware tests.
