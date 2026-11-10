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

The exact UFS candidate reached `arm_init` with a 256 MiB owned window
through the ARM64e-aware loader in QEMU. Its occupied virtual span is
`0x8f44000`. The sanitized Elden memory descriptions admit five
32 MiB-congruent 256 MiB windows for this link base. The same-boot
firmware memory map and loader allocation remain tablet checkpoints.
The independent Elden ADT audit accepts both the older 64-byte seed and
the current generator's 256-byte seed.

### Elden EL1 RAM probe

The standalone 47-bit UEFI app from the pinned reference checks 16 KiB
TTBR1 walks, high-address instruction fetch, access permissions, and
restoration of the firmware registers. The Android v4 RAM image is
2,727,936 bytes and has SHA-256
`5ef0a3fe7350d47a7db4c595648cc375cee422322e097372b4094afb873b3c1f`.
The phase-2.3 verifier accepted the BootShim, 4 MiB firmware volume,
and embedded 8 MiB FAT. Reading back the EFI file from FAT matched its
build output. The six standalone assembly cases passed in QEMU. The
EFI app also reached EL1 in QEMU, where it declined a RAM allocation
below its admitted physical range.

The tablet reported product `elden_prc_wifi` and `is-userspace: no`
over MacBook USB. Bootloader fastboot accepted the image transfer and
the RAM-boot command. The on-screen probe result is still pending.

The verified copy is in `~/Downloads/elden-el1-native47-ram.img` on the
MacBook. For the owner-operated run, enter the tablet bootloader and
run the following from the MacBook.

```sh
"$HOME/Library/Android/sdk/platform-tools/fastboot" boot "$HOME/Downloads/elden-el1-native47-ram.img"
```

Select **ELDEN 2.3 PROBE - EL1 16K 47-bit**, then follow the on-screen
key prompt. Capture the `walks`, `execute`, `controls`, and
`firmware-restored` results. This probe executes before XNU. The
separate EL1 PAC checkpoint still requires tablet evidence.

To rebuild the payload, place the reference build's `ShengNativeEl1.efi`
at `EFI/UniXNU/Payloads/Native47.efi` on an 8 MiB FAT16 image labeled
`ELDENPROBE`, with `package/elden-el1-native47-unixnu.toml` at
`EFI/UniXNU/unixnu.toml`. Use `build_phase2_firmware.py` and
`build_phase2_image.py --kind probe --embed-payload` with that FAT image,
then verify with `verify_phase2_artifact.py --kind probe --embed-payload
--expect-default native47`.

### Elden framebuffer driver

On 2026-11-10, a read-only query of the running tablet's Linux MSM DRM driver
found the connected internal DSI panel at 1904×3040 and an active 165 Hz mode.
The panel is CSOT PP8807HB1-1 with a Novatek NT36536 controller, a 118×190 mm
active area, and an AW99706 backlight. The Linux framebuffer used a 7,680-byte
stride. The UEFI handoff recorded a 7,616-byte stride in the reserved
`0xfc800000`–`0xff300000` aperture.

The Elden profile uses the boot-time stride and checks the 32-bit scanout
extent against the board reservation before mapping it. It advertises one
CPU-presented mode and the panel's physical size. The software presentation
timer stays at 60 Hz, independent of the firmware's panel refresh.

From the pinned macOS-ForSheng checkout on the Mac build host:

```sh
git apply "$ELDEN_REPO/patches/macos-recovery/elden-gop-display.patch"
.venv/bin/python scripts/build_macos26_xnu.py --arch arm64e --link-kernel \
  --build-qemu-gop-display --build-ufs-driver --build-gop-display \
  --elden-gop-board "$ELDEN_REPO/board.toml" --kdk-root "$KDK_ROOT" -j 8
```

The Mac built both the VirtIO and UFS companions. Both prelinks passed
the 25F71 audit for all 473 framebuffer vtable slots and their PAC metadata.
The hardware profile is selected with `native-gop-display=1 elden-gop-display=1`
and a supported explicit `cpus` value. The QEMU observer uses
`--native-gop-display` with the generic framebuffer profile. Build the QEMU
candidate with `--hid-components` and select `IOMobileGraphicsFamily`,
`IOSurface`, and `IOAVFamily` through `--include-module`.

The QEMU driver mapped the 800×600 framebuffer and reached `Display on`.
`launchd` ran, and WindowServer had one start and zero exits by the end of
the 180-second run. When explicitly enabled against the same 800×600
framebuffer, the Elden profile rejected the mismatched scanout before
mapping it. Recovery still reached `launchd` in that control run.

The captured screen still showed the boot console. No composited frame or
language chooser was observed. Display tests on Elden, panel modesetting,
backlight control, and GPU acceleration remain separate milestones.

### Elden UFS controller

A read-only query on 2026-11-10 against Elden Linux
`7.2.8-y700-r2-androidcrypt1` found the SM8750 host at `0x1d84000`, six
LUNs with 4 KiB logical sectors, and the legacy request queue in use.
Linux reported MCQ disabled. LUN 0 did not have a `macosroot` partition.

On the MacBook, USB ADB identified the running `lenovo,elden` Linux
device. A read of exactly one 4 KiB block at LUN 0 LBA 1 through Linux
returned an EFI GPT header with CRC32 `4b1100b9`. The macOS driver's GPT
predicate, compiled as a host check, accepted the returned block and
rejected the same block with an altered checksum. This verifies the
expected probe data through Linux. Native macOS controller access remains
pending.

The macOS 26.5 ARM64e adapter extends the pinned `NativeUfs` transport.
It requires the Elden ADT, powered GCC branches, a legacy UFSHCI queue,
and firmware's inherited identity DMA route for stream `0x60`. It verifies
that route before changing the request list and leaves the SMMU untouched.
The DMA arena is aligned to 16 KiB. Reads stay within the reported LUN 0
capacity, and the submission path rejects writes to media. The adapter
retains the DMA arena and quarantines the transport after a failed takeover
that may have left the controller in ownership of the buffer.

Apply the Elden overlays to the pinned macOS-ForSheng checkout and build on
the Mac host:

```sh
git apply "$ELDEN_REPO/patches/macos-recovery/elden-gop-display.patch"
git apply "$ELDEN_REPO/patches/macos-recovery/elden-ufs-build.patch"
.venv/bin/python scripts/build_macos26_xnu.py --arch arm64e --link-kernel \
  --build-ufs-driver --elden-ufs-device "$ELDEN_REPO" \
  --build-gop-display --elden-gop-board "$ELDEN_REPO/board.toml" \
  --kdk-root "$KDK_ROOT" -j 8
```

The MacBook built and prelinked the ARM64e candidate
`7e4d2664f29195f3bef698bbbd39a15ca3a77581feb2a2728f5847ebebca06af`
for macOS 26.5 (25F71). Its 326-slot storage ABI and pointer-authentication
audit passed. Sanitized host tests exercised the command and queue path,
including full-width DMA, capacity and LUN bounds, write rejection, and
quarantine after a transport failure. The QEMU control reached
`IOFindBSDRoot` with the probe requested and refused the non-Elden ADT
before mapping controller registers or publishing media.

After the Elden macOS handoff is qualified, the first owner-operated RAM
boot can select `native-ufs-elden=1 native-ufs-probe=10 native-ufs-diagnostics=1`.
This reads LUN 0's GPT header and checks its CRC without needing a
`macosroot` partition. The older Elden DarwinBoot parser requires ARM64
CPU subtype zero and rejects the ARM64e candidate's subtype `0xc0000002`.
Integrate and qualify the ARM64e-aware DarwinBoot before packaging this
kernel for Elden. The next checkpoint is read-only Recovery media
publication with `native-ufs-elden=1 native-ufs-root=1` and matching
BaseSystem on a dedicated `macosroot` partition. Cold initialization of
the PHY, controller reset, MCQ, and persistent writes are outside this
firmware-handoff implementation.
