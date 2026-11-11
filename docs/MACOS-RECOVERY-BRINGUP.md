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

Qualify EL1 PAC and the available translation granule on the tablet
before adapting the loader, memory map, and UFS path. The tablet's
current EL1 environment reports 4 KiB support and no 16 KiB support.
Use the repository's owner-operated RAM-boot procedure for hardware
tests.

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
the RAM-boot command. The tablet photo shows that the app ran at EL1,
raised a recoverable exception, and restored the firmware registers.
The reported values were `walks=FAIL execute=FAIL controls=FAIL
firmware-restored=PASS`, `stage=1`, `ESR=0x96000010`,
`FAR=0xffff800000004000`, and `ELR=0xcccb80a8`.
The shipped probe's instruction at offset `0xa8` is the first
`AT S1E1R` for the low TTBR1 alias. The syndrome is a synchronous
external Data Abort. The probe restored state before reaching either
indirect call, so direct 16 KiB instruction fetch is still open.

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

### Elden direct instruction-fetch follow-up

Apply `patches/macos-recovery/elden-el1-direct-execution.patch` with
`patch --batch --fuzz=0 -p1` in an isolated copy of the reference
probe sources. Rebuild `ci/preflight.dsc` with
`-D SHENG_NATIVE_EL1_PROBE=1` and use the same 8 MiB payload and
phase-2.3 firmware. Append `DIRECT` to the probe menu title and
keep the default entry ID `native47`.

This variant keeps the same 47-bit, 16 KiB table layout and attempts
the two indirect calls and data accesses before restoration. The
temporary TTBR1 phase does not execute the `AT` controls. The app
prints the requested and active TCR, active TTBR1, and any fault
location. The EFI disassembly shows two indirect calls. The image
passed the phase-2.3 verification and the MacBook copy matched
SHA-256 `6ec54f8b0f5be83065e4cf365f5318d6c6521a87069b0846be36833feb99e657`.

The follow-up is at `~/Downloads/elden-el1-native47-direct-ram.img`
on the MacBook. Elden's bootloader returned `OKAY` for both the
transfer and the RAM-boot command. Select **ELDEN 2.3 PROBE - EL1
16K 47-bit DIRECT**.
Capture `execute`, `registers`, and `firmware-restored` or the
reported trap stage.

The tablet photo shows `execute=FAIL registers=PASS firmware-restored=PASS`.
Firmware TCR was `0x480803514`, active TCR was `0x475113514`, and active
TTBR1 was `0xccc64000`. The first direct instruction fetch
at `0xffff800000004000` faulted at probe checkpoint 2 with `ESR=0x86000002`
and the same FAR and ELR. The syndrome is an Instruction Abort with a
level-2 address size fault.

`ID_AA64MMFR0_EL1=0x2100122100001024` reports stage-1 `TGran16=0`
and `TGran4=0`, which encode unsupported 16 KiB and supported 4 KiB.
Readback of the requested TG1 field establishes that its value was
written. The reported granule support and fault apply to Elden's
current EL1 environment. The existing macOS kernel was built for
16 KiB pages. [Arm feature register encoding](https://documentation-service.arm.com/static/62c410c8b334256d9ea8dffb)

### Elden 4 KiB execution control

Apply `patches/macos-recovery/elden-el1-4k-control.patch` after the
direct-execution patch. The control uses four-level 4 KiB tables for the
same 47-bit addresses, code, and data, and retains the original
TTBR0 configuration. An independent host table walk checked both
executable aliases, writable non-executable data, and an unmapped
address. The build and embedded image checks passed.

The MacBook copy is `~/Downloads/elden-el1-native47-4k-ram.img` with
SHA-256 `8f4103d10c065a5ee652f92872b8b32e26377ba227da48365dfc3972857dafef`.
Elden bootloader fastboot accepted the image and the RAM-boot command.
The tablet showed `execute=PASS registers=PASS firmware-restored=PASS`
and EFI `Success`. Both aliases returned their expected PCs
(`0xffff800000004000` and `0xfffffe0007004000`), and the shared data
read returned `0x5757`. Active TCR was `0x4b5113514` and TTBR1 was
`0xccc64000`. This confirms 47-bit EL1 execution with 4 KiB pages
for the standalone probe. The 4 KiB macOS kernel handoff remains a
separate checkpoint.

XNU builds L1/L2 bootstrap tables. A 4 KiB configuration gives TTBR1
39 bits with a full-page L1 or 38 bits with a subpage L1. Both exclude
the reference link address `0xfffffe0007004000`. The first Elden
kernel candidate uses the small-memory layout and links inside the
38-bit window.

### Elden EL1 PAC probe

Apply `patches/macos-recovery/elden-el1-pac-probe.patch` to an isolated,
unmodified copy of the pinned reference probes. Build
`ci/preflight.dsc` with `-D SHENG_NATIVE_EL1_PROBE=1`. Package the
output EFI as `Payloads/EldenPac.efi` and set the default entry ID to
`pac` in the phase-2.3 probe payload.

The standalone app checks the boot CPU's address and generic PAC
feature fields. It reads the original keys and tests writing those
same values before trying synthetic keys. It verifies restoration
before returning to firmware. A local synchronous fault while the
test keys are installed triggers restoration. The app stops if it
cannot verify safe restoration. No original key values appear in
the report.

The EFI build, opcode check, and RAM image verifier passed. The MacBook
copy is `~/Downloads/elden-el1-pac-ram.img` with SHA-256
`800e8b7e87045d92cfce9e4654b2f75104bfcad940ddb2c2a7d6424f273e889b`.
Elden bootloader fastboot accepted the transfer and RAM boot.
The tablet ran **ELDEN 2.3 PROBE - EL1 PAC** at EL1 and reported
`bank-switch=PASS flags=7FFF expected=7FFF` and EFI `Success`.
Markers 0–4 confirm vector setup, key reads, unchanged writes,
completed restore writes, and successful restoration verification.
The probe qualifies boot-CPU PAC bank switching and instruction
round trips. The XNU task and thread PAC lifecycle remains untested
on Elden.

### Elden macOS 26.5 kernel entry

On the Mac build host, run the Elden wrapper against the pinned
macOS-ForSheng checkout and its verified KDK.

```sh
.venv/bin/python "$ELDEN_REPO/scripts/build-elden-macos26-bootstrap.py" \
  --sheng-root "$PWD" --kdk-root "$KDK_ROOT" -j 10
```

The isolated output is `out/elden-macos26-4k-bootstrap`. The 4 KiB
pmap reserves the low commpage's enclosing 1 GiB region by bounding
the kernel's userspace address limit at 63 GiB. Disassembly confirmed
`TCR_EL1=0x00080022a51a251a` in the bootstrap code, which selects 4 KiB
granules and a 38-bit TTBR1 window. The ARM64e Mach-O links at
`0xfffffff007004000` with entry `0xfffffff007544488`. Its UUID is
`5AF018E6-4D5A-30C8-8DFB-4603A696A323`.

Apply `patches/macos-recovery/elden-darwinboot-arm64e.patch` in
`third_party/UniXNU/drivers/DarwinBoot`. Keep both UEFI overlays from
the Fastboot section for the menu. Build the phase-2.3 firmware and
generate the ADT with `UniXNU/scripts/make-adt.py`, the compiled Elden
DTS, `board.toml`, and `package/elden-macos26-bootstrap-adt.toml`. Use
a temporary nonzero identity for this bootstrap test. Run
`scripts/darwinboot_preflight.py --arm64e`, then package with
`scripts/make_phase2_payload.py` using
`package/elden-macos26-bootstrap-unixnu.toml`. The Android v4 image
uses the 56 MiB embedded FAT channel and boots the source kernel
with one CPU and the inherited GOP console.

The verified MacBook image is `~/Downloads/elden-macos26-4k-ram.img`
with SHA-256 `3e64fc1a121f665d79e1de5ce5a27d7f264699f8ece27bfb1a7b064a813a62da`.
Elden accepted the RAM boot. After the owner selected **Elden macOS
26.5 - 4 KiB Kernel Entry**, DarwinBoot validated the inputs, exited
boot services, and entered the kernel. The kernel lit stages 0–C
green, through programming TCR, the translation table bases, and
MAIR before enabling the MMU. The blue D–F markers indicate a
prepared framebuffer mapping. The first translated instruction
and C entry remain unverified. Recovery media was absent.

The first kernel carried the reference diagnostic mapper's 16 KiB
table indexes into the 4 KiB build. The retest uses XNU's selected
L1 and L2 geometry and checks each L2 slot before writing any block
descriptors. For the tablet's framebuffer base and stride, the
diagnostic region fits in three consecutive 2 MiB blocks under
TTBR0 L1 index 3. The build, disassembly, and embedded-image checks
passed using the same firmware and ADT as the first test.

The MacBook retest image is `~/Downloads/elden-macos26-4k-r2-ram.img`
with SHA-256 `1f6faa01602ad8781d4bb066950eb256e784d0432bea5cd096a37350b76e3069`.
The device's `SHENG-PANIC-1` record matched the linked kernel UUID
`FB8F5446-4A2F-3991-916F-148E0D3DB677`. XNU passed the MMU transition
and `arm_init` and reached `kmem_range_init`. It stopped at
`vm_kern.c:3220` for `kmem_data_private_range`, with hole size
`0xb8001b000` and requested size `0x5555554df5536000`. The 128 GiB
macOS zone claim exceeded the largest hole of about 46 GiB. The
unsigned subtraction in `kmem_add_extra_claims` wrapped before
dividing the remainder among pointer and data ranges.

The third image uses XNU's existing 24 GiB zone VA and 8 GiB wired
cap on small SHENG maps. If fixed claims exhaust the largest hole,
XNU now reports the two sizes before subtracting them. The linked
zone claim was checked directly in the Mach-O startup data.
The verified MacBook image is `~/Downloads/elden-macos26-4k-r3-ram.img`
with SHA-256 `7a4c686c457c8660313ca77b4be8ca43319fa5099990f10ccd585283a605e9b3`.
Elden accepted the RAM boot. The kernel UUID is
`3021AE3E-E615-31E6-9E7C-3A85656F42E0`.
The device cleared `kmem_range_init` and entered IOKit device-tree
initialization. A data abort at `0xfffffff007aa96e8` was the null
registry-entry dereference in `IODTGetICellCounts`. Its caller was
`IODTMapInterruptsSharing`, which could not resolve the UFS interrupt
parent through IOKit's `AAPL,phandle` property.

Before generating the ADT for the fourth image, apply
`patches/macos-recovery/elden-adt-interrupt-phandle.patch` to the
UniXNU checkout that provides `scripts/make-adt.py`. The regenerated
ADT retains `phandle` and adds the same value as `AAPL,phandle` on
the GIC. Host validation matched both values to the UFS
`interrupt-parent` and checked the three-cell specifier. The kernel
and firmware are unchanged from the third image.

The MacBook image is `~/Downloads/elden-macos26-4k-r4-ram.img` with
SHA-256 `b3dd3c4aca35c4b9521ee952b0691dbadfaba78b879a2c05a54793362a1deb5c`.
The tablet passed interrupt-parent parsing. The next panic was a data
abort at `SHA1Init` in `corecrypto_sha1.c:43`, called by
`IOPlatformExpertDevice::generatePlatformUUID`. The bootstrap image
reached this point without a registered corecrypto provider.

For the fifth image, apply
`patches/macos-recovery/elden-macos26-prelink-base.patch` to
macOS-ForSheng and emit the paired Recovery module set.

```sh
.venv/bin/python scripts/emit_macos26_security.py --elden-bootstrap \
  --recovery-components --kernel-build "$KERNEL_BUILD" --output "$PRELINK"
```

Set `KERNEL_BUILD` to the isolated Elden output with its matching dSYM.
The emitter uses the linked image base for authenticated pointer
targets and slides the BootKC components into the same aperture.
It emitted 15 paired modules with no unresolved instruction relocation.
DarwinBoot preflight accepted the 109,101,056-byte kernel and its
`0x8f38000` virtual span in a 256 MiB owned window. Package with
`package/elden-macos26-recovery-unixnu.toml` and a 112 MiB FAT volume.

The verified MacBook image is `~/Downloads/elden-macos26-4k-r5-ram.img`
with SHA-256 `b6dd563354e58850007ee968331216976204decd83d251e22bc839ace892f40f`.
After selecting **Elden macOS 26.5 - 4 KiB Recovery Components**,
the tablet passed the previous SHA1 initialization fault and reached
ArmCpu startup. XNU returned `KERN_FAILURE` (5) for CPU 0 registration.
The ADT has a boot CPU with MPIDR affinity `0x0` and a secondary
with `0x10000`. The original lookup discarded Aff2, so ArmCpu
matched both providers to logical CPU 0 even with `cpus=1`.

The next build preserves Aff2 in the SHENG CPU lookup. Its ADT check
finds exactly one match for logical CPU 0, and disassembly confirms
the 24-bit mask. The retest keeps the same firmware, ADT, and paired
prelink modules. Recovery root media and multicore startup remain
separate checkpoints.

The MacBook retest image is `~/Downloads/elden-macos26-4k-r6-ram.img`
with SHA-256 `3fe386db944745727360ab57c5bf03146aa9c2be577e9fd52a6f1ef4ec54ffb3`.
Its kernel UUID is `4BC95A0C-8278-3B67-A060-99E592519074`.
The tablet passed CPU registration and panicked in
`ml_unsafe_kernel_text()`. The paired dSYM resolves the fault to the
`_unsafe_kernel_text_initialized` assertion. XNU's initializer expects
a four-byte `/chosen/kernel-ctrr-to-be-enabled` value from the loader.

The seventh image supplies that value as `1`, which keeps XNU's
writable-text tracing disabled. This is the policy value consumed
by XNU. The kernel and the 15 prelinked modules are identical to the
sixth image. The independent ADT comparison found this to be the
only change apart from the freshly generated seed and test identity.
The MacBook image is `~/Downloads/elden-macos26-4k-r7-ram.img` with
SHA-256 `a58bff66f00e9de42322e9afac3373e84e6593f8dc728a02807a3f162ffe8824`.
Bootloader fastboot accepted the image and disconnected from USB.
The owner's photo shows Elden passed the text-policy assertion,
reached `PE_lockdown_iokit`, loaded APFS, and entered
`IOFindBSDRoot`. The `Apple_HFS` match is XNU's default when no
specific root match is supplied. This image contains no Recovery
root media. NVRAM sync reported that no controller was available.
The separate read-only UFS probe is the next storage test.

### UniXNU firmware TTY commands

The opt-in menu reads newline-terminated commands from the existing
EFI Serial I/O protocol and writes replies to the same port. Set
`serial_commands = true` in the payload's `[boot]` section. Send
`help`, `list`, or `status` to inspect the menu, and `boot <id>` to run
a configured entry. Entries with a confirmation prompt require local
selection. A recognized command stops the countdown. While the serial
reader is active, volume-key scan-code navigation remains available
and text shortcuts are suppressed so command bytes cannot select a
menu item through a shared terminal.

Apply the overlay to the pinned device-side UniXNU checkout and build
into a separate output directory:

```sh
git -C third_party/UniXNU apply "$PWD/patches/uefi/serial-menu-commands.patch"
python3 scripts/build_phase2_firmware.py --toolchain "$LLVM_BIN" \
  --crypto-path "$PWD/.cache/phase1/basecrypto" --output-dir out/elden-tty/firmware
```

Package the direct-execution probe as above with `serial_commands`
enabled. The verified RAM image is `~/Downloads/elden-tty-ram.img`
on the MacBook, with SHA-256
`6fb0e19c02f671bd3855c00d2eba43edf1593b664aeadd9e134b7d9e3ee7022c`.
The parser passed host tests for split input, CRLF, overflow,
invalid bytes, duplicate IDs, and entries requiring confirmation.
The Elden firmware and embedded payload passed the build and image
checks. Tablet TTY input and output remain unverified.

This stage relies on firmware exposing a working serial port. The
existing Elden USB CDC adapter runs after XNU starts. A pre-boot CDC
transport still needs to publish a serial port for the MacBook's
USB-C connection. Child EFI applications currently use their own
console for output and key prompts.

### Reboot to Fastboot

Select **Reboot to Fastboot** in the Elden UniXNU menu. On an EFI serial
connection, send `boot fastboot` followed by a newline. The action uses
the Elden bootloader's PSCI `SYSTEM_RESET2` type `0x80010001` and cookie
`0x2`. If the request returns, UniXNU reports the error in the menu.
[Elden Linux hardware results](https://github.com/xiaquwan1001/lenovo-legion-y700-gen4-tb322fc-elden-linux/blob/main/archive/topics/FLASHCTL-2026-10-03.md)

Apply both UEFI overlays to the pinned UniXNU checkout, then build
and package the image. `PAC_EFI` is the compiled EL1 PAC probe from above.
The PAC probe is still the default menu entry and **Restart** is the
ordinary reboot.

```sh
git -C third_party/UniXNU apply "$PWD/patches/uefi/serial-menu-commands.patch"
git -C third_party/UniXNU apply "$PWD/patches/uefi/elden-fastboot-menu-action.patch"
work=out/elden-fastboot
python3 scripts/build_phase2_firmware.py --toolchain "$LLVM_BIN" \
  --crypto-path "$PWD/.cache/phase1/basecrypto" --output-dir "$work/firmware"
PYTHONPATH=scripts python3 -c '
from pathlib import Path
from make_payload import build_config_image
build_config_image(Path("package/elden-fastboot-unixnu.toml"),
                   Path("out/elden-fastboot/probe.img"), 8,
                   label=b"ELDENPROBE", media_id=0x53484e47)
'
mmd -i "$work/probe.img" ::/EFI/UniXNU/Payloads
mcopy -i "$work/probe.img" "$PAC_EFI" ::/EFI/UniXNU/Payloads/EldenPac.efi
python3 scripts/build_phase2_image.py --kind probe --header-version 4 \
  --embed-payload --fd "$work/firmware/SILICIUM_UEFI.fd" \
  --payload "$work/probe.img" --work "$work" --clang "$LLVM_BIN/clang" \
  --output "$work/elden-fastboot-ram.img"
python3 scripts/verify_phase2_artifact.py --kind probe --embed-payload \
  --fd "$work/firmware/SILICIUM_UEFI.fd" --shim "$work/shim-probe-embed.bin" \
  --payload "$work/probe.img" --image "$work/elden-fastboot-ram.img" \
  --expect-default pac --report "$work/verify.json"
```

The firmware build, PSCI argument and return-path tests, and embedded
payload checks passed. The MacBook copy is `~/Downloads/elden-fastboot-ram.img`
with SHA-256 `b865e2ef7d6038842119144f0076cb11f7e8c9461616474c4967a486eedb4caa`.
The tablet accepted the RAM boot and disconnected from USB fastboot.
After the owner selected **Reboot to Fastboot**, the MacBook saw the
tablet re-enumerate with `product: elden_prc_wifi` and `is-userspace: no`.
This verifies the local menu action on the tablet. The serial route
still requires a working EFI Serial I/O connection.

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
`macosroot` partition. The Elden DarwinBoot overlay admits ARM64e
after checking both address and generic PAC.

The 4 KiB UFS companion builds with the Elden bootstrap wrapper's
`--elden-ufs-device "$ELDEN_REPO"` option after applying
`elden-ufs-build.patch` to macOS-ForSheng. Pass the resulting
`ufs-driver` directory to the prelink emitter's `--storage-driver`
option. The 4 KiB prelink passed the 326-slot storage ABI check and
DarwinBoot preflight. Package with
`package/elden-macos26-ufs-probe-unixnu.toml` and the seventh image's
ADT. The verified MacBook copy is `~/Downloads/elden-macos26-4k-ufs-ram.img`
with SHA-256 `8d884a50a17ffa676adece06a39b80008475cac6c8173723ba81123233a035c7`.
After the partition update, bootloader fastboot reported `macosroot` as
`0x100000000` bytes and accepted the RAM boot of this image. The owner
selected the UFS probe. It timed out at `NOP OUT` with `db=1` and `is=0`
and reached neither LUN capacity nor the GPT read. The truncated panic
did not include the SMMU fault registers or link state.

The next image ports the SM8750 link wake sequence from the earlier
Elden iPadOS driver. After stopping the legacy queue it disables the
automatic Hibern8 timer, checks the TX lane state, exits Hibern8 if
needed, and disables completion aggregation before restarting the
queue. The compact panic line includes TX state, UIC result, AHIT,
doorbell, interrupts, and global and context-bank SMMU fault status.
The transport retains the inherited DMA route and rejects media writes.

The MacBook compiled and linked the revised driver. Six host link
tests, the ARM64e storage prelink, DarwinBoot preflight, and embedded
image checks passed. The retry image is `~/Downloads/elden-macos26-4k-ufs-wake-ram.img`
with SHA-256 `bdebbc4098043d6d544adf2a8a5cf09ee7f43d0fe515eae246085b4294716eb3`.
Bootloader fastboot accepted the retry RAM boot. Select
**Elden macOS 26.5 - UFS Read-only Probe** and capture the result.

The link-wake retest observed TX lane state `1` (Hibern8). It reported
a timeout during exit with `uic=0`, `db=0`, and `is=0x420` in the later
snapshot. `0x420` contains both UIC command completion and Hibern8
exit. The previous implementation made separate timed decisions and
could miss a completion at the deadline.

The next probe polls both bits together with a single 600 ms budget
measured against the ARM physical counter. It uses the status from the
deadline iteration, checks the UIC result and local power status, and
rechecks the TX lane before starting the queue. The panic now includes
the status used for the decision and the before/after SMMU fault status.
Eight host cases passed, including both bits on the final sample and
one bit missing at timeout. The Mac build, storage ABI prelink, DarwinBoot
preflight, and embedded image checks passed. The MacBook image is
`~/Downloads/elden-macos26-4k-ufs-uic-ram.img` with SHA-256
`b07b974f4819396fef30c1feab2a731e00a22d0aa2a36639de3afd6dca2d5b3d`.
Bootloader fastboot accepted this RAM boot. The owner photo shows IOKit,
APFS registration, and the default `Apple_HFS` root-device wait. The
earlier panic was absent from the captured screen. The visible log
does not include the `NativeUfs` GPT result.

With probe 10 and diagnostics enabled, the next image uses the existing
panic display to hold `ELDEN UFS PROBE PASS` and the CRC on screen after
one successful LUN 0 LBA 1 read and header verification. A failed
transfer or invalid header stops with its stage and reason. Normal
probe mode without diagnostics continues after logging success. The
MacBook image is `~/Downloads/elden-macos26-4k-ufs-result-ram.img` with
SHA-256 `0adacbc5f0b68f040f9864e930abea24ebbef508ccb1795fef3108cd64aa4fe0`.

The physical tablet reported `ELDEN UFS PROBE PASS: LUN 0 LBA 1 GPT
header CRC verified`. The reported CRC matched the primary GPT header
saved after repartitioning. This verifies the inherited UFS link, DMA,
and read path through the first GPT block. The diagnostic stop is the
expected end of probe 10.

On 2026-11-10, TWRP's `baseband_guard` denied writes to the Linux
`rootfs`. A RAM boot of the installed mainline Linux kernel with a
maintenance initramfs brought up USB serial and kept UFS filesystems
unmounted. Offline ext4 resize reduced `rootfs` by 4 GiB and kept its
start LBA and GPT identity. A second filesystem check
passed after the kernel reread the new partition table.

Both GPT copies matched the verified candidate byte for byte. Only the
end of `rootfs` and the new 4 GiB `macosroot` entry changed. Linux
enumerated `macosroot` as `/dev/sda18` on LUN 0. The original GPT copies,
4 GiB tail, and ext4 undo files are retained privately.

### Elden Recovery root media

On 2026-11-11 the Linux maintenance RAM boot installed the 1.471 GiB
paired BaseSystem on the dedicated `macosroot` partition. It verified
the data before committing the manifest and matched the complete image
on readback after flushing the partition cache. Both GPT copies were
unchanged. The installed image
contains BaseSystem only. The available ISC seed differs from the
current driver's pinned digest and volume identities.

The Recovery Root RAM image uses the qualified UFS kernel with
`native-ufs-elden=1 native-ufs-root=1`, a Recovery ADT that matches the
paired APFS volume UUID, and the matching BaseSystem root hash. The ADT
retains the verified Elden kernel text policy. The MacBook image is
`~/Downloads/elden-macos26-4k-recovery-root-ram.img` with SHA-256
`15fe0bd06d6fca5ab0668bba5e9abef5fecaf45134649483ac12439950947043`.
Bootloader fastboot accepted the RAM boot. Select **Elden macOS 26.5 -
Recovery Root** to test read-only media publication and the APFS mount.

The owner photo shows `Root mounted` recorded in the progress bitmap and
`launchd exec` as the latest stage. PID 1 recorded one exec and zero
exits. At 93 seconds there was no later stage or recorded start of
`recoveryosd`, RecoveryOS Agent, WindowServer, or Language Chooser. The
APFS root mount and PID 1 exec are device-verified. The image does not
establish that `launchd` reached its main function.

The next RAM image adds `sheng-stderr-log=2 sheng-stderr-name=launchd`
to capture PID 1 output with the same kernel and Recovery ADT. The
MacBook copy is `~/Downloads/elden-macos26-4k-recovery-pid1-ram.img`
with SHA-256 `76d640cd8124296ddf60503f208bc4d47bca3056aefe0999292ba35751053206`.
The image passed the DarwinBoot and embedded payload checks.

The PID 1 log retest remained at `launchd exec` after 153 seconds. The
photo shows one recorded PID 1 exec, zero exits, and zero recorded
Recovery service starts. The visible log contains no PID 1 output.
A snapshot of the PID 1 thread state would help distinguish a wait in
dyld from a stall later in startup.

### Elden two-core Recovery test

The separate RAM image requests `cpus=2 sheng-smp=1 armcpu-self-ipi=1`
using the same kernel and Recovery ADT. The first two ADT entries are
MPIDR `0x0` and `0x100`, which remain distinct under the current
secondary reset vector's 16-bit mask. The kernel includes `SHENG-SMP`
logging for PSCI status, CPU startup stages, and per-core timer and IPI
counts. The test passes when CPU 1 reaches step 7 with a successful
PSCI result and receives both interrupt types.

The verified MacBook image is `~/Downloads/elden-macos26-4k-recovery-smp2-ram.img`
with SHA-256 `99646af150b0dcf250d9d9da3c93454415bed96fd84c1308b07666743191166e`.
Secondary startup remains pending a device run.

### Elden cross-cluster CPU test

The reset vector now preserves MPIDR Aff2 when locating the secondary
CPU's data. ArmCpu compares the complete hardware affinity before
enabling the core's redistributor. The Elden kernel uses one logical
scheduler cluster and has the monitor disabled. The fix keeps that
configuration and uses full hardware affinities for startup and GIC
routing.

The new ADT moves `0x10000` to the second CPU entry while preserving
all eight CPU nodes. Select **CPU Aff2 Test** to exercise the former
collision with boot affinity `0x0`. Select **Eight-core Test** after
the two-core case records step 7, PSCI result 0, and timer and IPI
delivery. Both choices use the same paired read-only UFS transport
and installed BaseSystem.

The MacBook rebuilt the 4 KiB ARM64e kernel. Disassembly verified the
24-bit reset mask, and a host audit checked all 64 expected/observed
affinity pairs. The prelink, DarwinBoot, and embedded payload checks
passed. The MacBook image is
`~/Downloads/elden-macos26-4k-recovery-smp-aff2-ram.img` with SHA-256
`8a46c7522f52037c374161e2d0a6c63989f22c0e146581f82540299df4333c0a`.
Bootloader fastboot accepted this RAM boot. The first photo shows IOKit
entry with CPU 1 at step 0. The later photos verify the two-core test on
the tablet. CPU 1 reached step 7 with `fail=0` and `psci=0`. The requested
MPIDR `0x10000` matches the observed `0x80010000` under the hardware
affinity mask. CPU 1's timer count rose from 452 to 1,367 and its IPI
count from 405 to 542. The boot mounted the macOS Base System and
recorded one `launchd` exec and zero exits. After 31 seconds at that
stage, no later service had been recorded.

The same RAM image has the **Eight-core Test** entry. Verify that it
reports `cpus=8 secondaries=7` and that CPUs 1 through 7 each reach step 7
with `fail=0`, `psci=0` and timer and IPI delivery. Bootloader fastboot
accepted the same image for the eight-core run. The owner photo captures
`cpu=7 timer=2 ipi=1`, confirming that logical CPU 7 received both
interrupt types. The aggregate and other secondary rows had scrolled off
the panel, so the capture qualifies CPU 7 and leaves the full eight-core
result open. At 218 seconds after `launchd exec`, the same run recorded
one start, zero exits and zero starts for the watched Recovery services.
The next Recovery checkpoint is evidence that PID 1 runs beyond exec.

A separate, host-verified **CPU Affinity Control** image keeps the Aff2
kernel and two-core boot arguments and restores the original ADT CPU
order. It selects `0x100` as the secondary. The MacBook copy is
`~/Downloads/elden-macos26-4k-recovery-smp-control-ram.img` with SHA-256
`154b51b260c46ecc5b54b839ba9d366021eca34369a0bf7b1552e3050ff782a1`.
This control has not been run on the tablet.

### Recovery PID 1 trace

The opt-in `sheng-pid1-trace=1` reports whether the kernel's initial
exec call returned, whether PID 1 took synchronous exceptions from EL0,
and whether `launchd` acknowledged reaching `main` by setting
`kern.initproc_spawned`. The report includes the last syscall number
and the syndrome of the last non-syscall exception. Counters are
independent samples. A zero exception count means no trap was recorded.
The exception hook only records counters and the progress worker prints
a summary every 30 seconds.

The **Recovery PID 1 Trace** menu entry uses one CPU, the original
Recovery ADT and the deployed BaseSystem. It retains the launchd output
filter from the earlier PID 1 log run.

The MacBook built the 4 KiB ARM64e kernel and the paired UFS companion.
The 15-module prelink, 326-slot storage ABI, DarwinBoot preflight and
embedded RAM image verification passed. The MacBook copy is
`~/Downloads/elden-macos26-4k-recovery-pid1-trace-ram.img` with SHA-256
`d96e5491d19257172168e727591f7e5b86cd576f6ce77a210db7a0c0541e055b`.
Bootloader fastboot accepted the trace image. Runtime verification
requires a photo of the `SHENG-PID1` line after `launchd exec`.

The later tablet photos show the same PID 1 counts at 1339 and 1369 seconds
after `launchd exec`. `launchd` had one start, zero exits, 179 EL0
synchronous exceptions and 52 SVCs. RecoveryOS services and WindowServer
had zero starts. The last recorded SVC was `sysctl` (202) and the last
non-SVC syndrome was `0x82000007`, an EL0 instruction abort with a level 3
translation fault. The photos establish a stall and repeated progress
reports. The fault could have been serviced before PID 1 stopped making
progress. The `main=0` field means the `kern.initproc_spawned` acknowledgment
was absent at the time of capture.

The updated PID 1 trace records the last instruction fault's PC, FAR,
LR and VM result and the latest SVC's entry, handler return and BSD errno.
The progress worker prints `SHENG-PID1-IF` and `SHENG-PID1-SVC` without
printing from the exception handlers. A result of `-1` means the trace has
no completion sample for that operation. The trace uses the Recovery
framebuffer build's one-CPU profile and the paired Recovery volume.

The rebuilt kernel links with the same 30 Recovery modules. The 326-slot
UFS and 473-slot display checks matched the previous ABI and PAC metadata,
and DarwinBoot preflight and raw Android v4 image verification passed.
The MacBook image is `~/Downloads/elden-macos26-pid1-fault-ram.img` with
SHA-256 `840932c863f9a5a252102e420514d34fcf317d3d71b20c668237a53c481d3ec4`.
RAM-boot this image and select **Recovery Framebuffer Test**. Capture
the `SHENG-PID1-IF` and `SHENG-PID1-SVC` lines after the stall recurs.

### Recovery framebuffer RAM image

The Elden bootstrap wrapper's `--build-gop-display` option includes the
software framebuffer in the same read-only UFS companion. The separate
**Recovery Framebuffer Test** menu entry enables the Elden framebuffer
and keeps PID 1 tracing on one CPU. The original Recovery ADT, BaseSystem
contract and firmware are reused.

The MacBook built the combined 4 KiB ARM64e companion. Prelink preserved
the Recovery modules and added the paired graphics and HID modules for
30 total. The 326-slot storage and 473-slot display ABI checks passed
with their pointer authentication metadata. Eleven host tests passed
for scanout, framebuffer ownership, boot progress and display bindings.

The larger prelink requires a 128 MiB embedded FAT payload. DarwinBoot
preflight and embedded-image verification passed. The verified MacBook
copy is `~/Downloads/elden-macos26-4k-recovery-framebuffer-ram.img` with
SHA-256 `7af49b03be2d3fafb07944677b40b290675ff7b27150b3db4a9ae2524eec63f2`.
The first tablet run still needs to establish `Display on` and
`First frame` after the Recovery services start.

### Recovery and Linux RAM image

The Linux entry uses the original ARM64 EFI-stub kernel extracted with
`unpack_bootimg` from the saved `boot_b` Android v4 image. Its initramfs
is built into the kernel. The Elden DTB was extracted from that same
kernel at `0x421b4e0`. The host checked the EFI application header and the
DTB's `lenovo,elden` compatibility string.

Use `scripts/make_phase2_payload.py --size-mib 224` with the framebuffer
test's kernel and ADT, `package/elden-macos26-linux-unixnu.toml`, and the
extracted files supplied through `--linux-kernel` and `--linux-dtb`.
Package and verify it with `build_phase2_image.py` and
`verify_phase2_artifact.py` in embedded-payload mode. The Linux menu
uses UniXNU's EFI-stub handoff and selects the existing `rootfs` partition.
The Recovery entry uses the framebuffer test's original arguments.

The 224 MiB FAT payload and 4 MiB firmware fit within the board's declared
256 MiB kernel reservation. Host verification read the Linux kernel and
DTB back byte for byte and verified the Recovery kernel and ADT. The
63.6 MiB boot image is staged as
`~/Downloads/elden-macos26-linux-dual-boot-ram.img` on the MacBook with
SHA-256 `3653a424874caf0b503411e130fbd843aec24a35ea12ff2e7addb0278c625a2f`.
The combined image still needs a tablet RAM boot. Select **Elden Linux -
installed rootfs** and verify Linux mounts the existing rootfs and reaches
its login screen.

### Slot B flash image

The running tablet Linux system supplied raw backups of all 37 GPT
partitions ending in `_b`, including the boot and recovery partitions.
The host checked all 826,736,640 bytes against their device SHA-256
digests. The backup files and manifest are stored privately.

The dual-boot RAM image was prepared as a 96 MiB raw image for `boot_b`.
The AVB footer uses the key named by the live `vbmeta_b` boot descriptor
and the existing boot image's `SHA256_RSA4096` algorithm, `boot` descriptor,
salt and rollback index `1738713600` at location `0`. The host verified
the RSA signature, AVB payload digest and byte-for-byte RAM image prefix.
The signed image SHA-256 is
`ccf4d3e02891e6d3ac586b4c52f9845bb78189272f0f033fca4705d8a8ae5107`.
The image was written to `boot_b` from the running tablet Linux system.
The full partition readback matched the signed image, and the other 36
slot B partition hashes matched the backup. The tablet reached UniXNU
from the installed image. The owner reported that selecting **Elden
Linux - installed rootfs** returned to the UniXNU menu. There is no
captured Linux panic or EFI return status for that run.

The saved Linux kernel's embedded configuration enables
`CONFIG_BUILTIN_DTB` and `CONFIG_CMDLINE_FORCE`, with `panic=10`.
The diagnostic RAM image changes the three copies of the forced panic
timeout to `panic=0` using an equal-length replacement. The host checked
the original kernel hash, confirmed that reversing the replacement
recovers the exact original bytes, and verified the packaged readback.
The MacBook copy is `~/Downloads/elden-linux-panic-hold-ram.img` with
SHA-256 `775c5cf107e517c66702486e705fb9f56b1ecc6d731db3f263aaa5c5b1d49892`.
The **Elden Linux - keep panic on screen** entry needs a tablet test.
The original `boot_b` backup is also staged on the MacBook for a direct
RAM boot and possible `pstore` capture.

### Linux EFI device tree and A/B menu

The installed Linux Image selected its built-in DTB in `setup_arch`,
discarding the EFI stub's device tree and the live EFI memory map carried in
its `/chosen` node. `scripts/prepare-elden-linux-efi.py` checks the original
Image digest and instruction bytes and patches only the EFI payload copy.
The new instructions pass the physical FDT address saved from x0 to
`setup_machine_fdt`. The direct-boot Linux image still uses its built-in tree.
This corrects a confirmed handoff defect. The prior reboot has no captured
panic or EFI return status to establish its cause.

Use the patched Image as `--linux-kernel` when packaging
`package/elden-macos26-linux-unixnu.toml`. **Switch to slot A and reboot (via
Linux)** and **Switch to slot B and reboot (via Linux)** boot the installed
rootfs with the selected slot in `/chosen/unixnu,boot-slot`. The Linux service
uses the installed TB322FC protected slot selector to check the GPT and UFS
boot LUN and reboots after the selector succeeds. The normal Linux entry
does not set the property. On a rootfs with the protected selector installed,
install the service from this repository with:

```sh
sudo install -m 0755 package/linux/elden-unixnu-select-slot /usr/local/libexec/
sudo install -m 0644 package/linux/elden-unixnu-select-slot.service /etc/systemd/system/
sudo systemctl enable elden-unixnu-select-slot.service
```

The host verified the firmware, embedded FAT contents and both slot requests.
The tablet's current Linux verified a dry run for slot A and reported that
slot B was already selected with UFS boot LUN 2. The installed service was
skipped on the normal Linux boot because the request property was absent.
The RAM image is `~/Downloads/elden-macos26-linux-slot-menu-ram.img` on the
MacBook with SHA-256
`4024f857c08220976fed9070c4b3470c3f9b78516f665680513f55ff3a5b5bd2`.
An end-to-end test needs a tablet boot through the new Linux EFI entry and
verification of a slot change triggered by the UniXNU menu.

The tablet's **TB322FC Boot Control** desktop launcher calls its local
`set-slot` script, which runs `qbootctl -s a` or `qbootctl -s b` from Linux.
Its Android label for A predates the UniXNU install. The root-owned
`tb322fc-select-boot-slot` helper provides a stronger reference for a
firmware implementation. It verifies the active A/B partition pairs, both
GPT copies and the UFS `boot_lun_enabled` attribute. A read-only status
check on the tablet confirmed slot B and UFS boot LUN 2.

The Qualcomm reference fastboot handler calls `SetActiveSlot`, which
updates the partition tables and selects the UFS boot LUN through its
`EFI_MEM_CARDINFO_PROTOCOL`. The current Elden UniXNU image omits the
vendor `UFSDxe`. The independent Elden UEFI UFS session permits reads
through its command filter. A standalone slot action needs a qualified
UEFI path for both the GPT update and the boot LUN attribute, including
readback and recovery if one update fails.

The 96 MiB slot A image uses the signing key named by the live `vbmeta_a`
boot chain descriptor, with the original slot A rollback index `1785542400`
and SHA256_RSA4096 algorithm. The running Linux system wrote the image to
inactive `boot_a` and verified its full readback SHA-256
`7af81d9ca661474f7408792afc9f419a8604a0b6fd9ca77af521fcaed03b3939`.
`boot_b` matched the original Linux backup before and after the write, slot B
remained active with UFS boot LUN 2, and both boot partitions were read-only
after verification. The original slot A backup and `vbmeta_a` were saved
privately. The installed image has yet to boot on the tablet.

### Direct boot slot selection in UniXNU

`EldenBootSlot.efi` runs from the UniXNU menu using the inherited UFS
controller. **Inspect boot slot** reads the active GPT state and UFS boot
LUN. **Switch directly to slot A and restart** and its slot B counterpart
require explicit confirmation. The action verifies both GPT copies and
the selected boot image header, updates the reviewed A/B pairs on LUNs 0
and 4, and sets `bBootLunEn`. It re-reads the updates before restarting.
The command filter permits writes only to the validated GPT sectors and
the boot LUN attribute. If a write fails, it attempts to restore and
verify the original state. A power interruption across the LUNs can leave
the selection incomplete.

Build the application with `scripts/build-ufs-stage2.py` and pass the
resulting `EldenBootSlot.efi` to `make_phase2_payload.py --slot-switch`.
Host tests exercised both directions, GPT integrity, the UFS command
filter and rollback. An in-memory replay also passed using read-only
copies of the tablet's actual GPTs and verified boot-image headers.
The MacBook RAM image is `~/Downloads/elden-native-slot-switch-ram.img`,
SHA-256 `f2bd708a9d63bc48f980af0b2a14567944d7a6687d088c57281ae4b3ccd604d2`.
The first firmware test should use **Inspect boot slot**. The native
read and write paths remain unverified on the tablet.

### Raw Android v4 image for slot A

The owner reported that the installed gzip image returned to fastboot.
Its signed partition readback had matched the host image. The bootloader
error is unknown. The image used Android header v4 with OS metadata
`13.0.0` / `2023-04`, and ABL had to expand its kernel field to 228 MiB.
The working Linux slot B image uses native `mkbootimg` v4 with an empty
ramdisk and a raw 94 MiB ARM64 kernel. The stock slot A image also uses a
raw ARM64 kernel and has OS metadata `16.0.0` / `2026-08`.

Build a new FD with `build_phase2_firmware.py`, then pass
`--embed-payload --raw-boot-reference <saved-stock-boot-a>` to
`build_phase2_image.py`. This path uses AOSP `mkbootimg` and the stock A
header metadata. It stores the FAT volume in independently compressed
64 KiB LZ4 blocks. EldenPayloadDxe reads the volume using one 64 KiB
buffer, so the bootloader only loads the 78 MiB raw ARM64 image. The
Linux EFI payload, A/B choices and Recovery payload match the previous
image byte for byte. This host path requires `mkbootimg` and `liblz4`.

Run `verify_phase2_artifact.py --embed-payload --raw-packed-payload`
with the built FD, raw shim and original FAT. The host verified the full
FAT readback, checked the firmware decoder against every compressed
block, and compared the new Android header with the stock slot A header.
The RAM image is `~/Downloads/elden-macos26-linux-raw-v4-ram.img` on the
MacBook, SHA-256 `5276aab993601fe4a1fcd754ec68d414b86ca1881f59938d165fc542c1f039d2`.
The signed 96 MiB candidate is `~/Downloads/elden-macos26-linux-raw-v4-boot-a.img`,
SHA-256 `87591804ebb4f581e72cb140a7a5d564026fbb0a9d00765792cdbc4ee4f02f6c`.
It preserves the original slot A AVB key, salt, rollback index and boot
properties. On November 11, the running slot B Linux system wrote the
raw-v4 image to inactive `boot_a` over SSH. A full readback matched the
signed image digest. The original `boot_b` digest also matched before and
after the write, both boot partitions returned to read-only, and slot B
remained active with UFS boot LUN 2. The RAM and installed boot paths still
need tablet testing.

### Linux EFI handoff diagnostics after the raw-v4 flash

The owner reached the UniXNU menu and reported that both the regular Linux
entry and the slot B entry returned to the menu. The slot B entry has to
reach the Linux rootfs before its systemd service can change the active
slot. No service journal or kernel panic from that attempt was available.
After SSH returned, the tablet was running Linux from slot B with UFS boot
LUN 2. The service journal showed only boots without a slot request. Both
pstore locations were empty, and the `boot_a` and `boot_b` digests still
matched the verified raw-v4 and original Linux images respectively.

`prepare-elden-linux-efi.py --diagnostics` keeps the FDT handoff correction
and enables `earlycon=efifb`, EFI debug logging, and `panic=0` in all three
copies of the kernel's forced command line. The diagnostic uses the same
raw-v4 firmware, device tree and Recovery payload. Host checks verified the
packed payload readback and every change to the forced command line.
The RAM image is `elden-linux-efi-diagnostics-ram.img`, SHA-256
`e715b84bf171a347b6758be7586c4db5ffe163d13df9c8d18c837df34f1995c0`.
If Linux returns to the menu, record the EFI status in the footer. If the
tablet resets, record the last visible Linux line and whether the Lenovo
splash appears. This diagnostic has not been flashed.

### Combined Recovery and Linux diagnostics

The combined raw-v4 image pairs the rebuilt Recovery PID 1 kernel with
the Linux EFI diagnostic kernel and its matching DTB in the 224 MiB FAT
payload. It uses the existing dual-boot menu. **Recovery Framebuffer
Test** reports `SHENG-PID1-IF` and `SHENG-PID1-SVC`. **Elden Linux -
installed rootfs** enables early framebuffer output, EFI debug logging
and `panic=0`. Both slot menu entries use that Linux diagnostic kernel.

DarwinBoot preflight, the full packed FAT comparison, and byte-for-byte
Linux, DTB and menu readback passed. The RAM image SHA-256 is
`8d7a2321b36ddbd32a79548a4ae62391ea446d5a621861655134b0e0b651586f`.
The 96 MiB signed image preserves the stock slot A AVB key, salt, rollback
index and boot properties. Its SHA-256 is
`5241566c3b2e5acad07a07145735ccde31176de271b7522b236f4ca8a15504c2`.
The running Linux system wrote it to inactive `boot_a` over SSH and
verified the full partition digest. The original `boot_b` digest matched
before and after the write. Both partitions returned to read-only and
slot B remained active after flashing.

The owner's first Recovery run of the combined image reached PID 1 and
printed the Darwin Ignition startup banner. At 127 seconds after
`launchd exec`, the tablet showed `SHENG-PID1-IF done=67/67 vm=0` and
`SHENG-PID1-SVC done=50/52 code=202 errno=-1`. The final recorded
instruction fault completed successfully. The trace had no completion
sample for the latest `sysctl`. `errno=-1` is the pending sentinel in the
trace. PID 1 still showed one start, zero exits, and no main acknowledgment.
The tracked Recovery and WindowServer services had zero starts.

The matching macOS-ForSheng reference run stopped after the same Ignition
banner and identified its last call as `security.mac.img4.ignition_blob`.
That makes the Image4 Ignition wait a candidate for the tablet stall.
The Elden trace identifies the syscall number and program counter, so
the specific sysctl still needs confirmation on hardware.

### Linux EFI console handoff on the tablet

The combined image's Linux entry reached the kernel on the physical tablet.
The photo shows the forced diagnostic command line, memory initialization,
GICv3 and architected timer initialization. Linux registered `tty0` and
then disabled its EFI framebuffer boot console. The photo ends there.
SSH did not respond during the check, so mounting `rootfs` and starting
userspace remain unverified.

The next RAM diagnostic uses `prepare-elden-linux-efi.py --diagnostics
--keep-boot-console` to keep `efifb` registered when `tty0` takes over.
The diagnostic reuses the space for the unused `bootconfig` argument and
omits EFI debug verbosity. It retains `panic=0`, `efi=noruntime` and
the original root selection. The Recovery trace and menu remain in the
same image. The MacBook copy is `~/Downloads/elden-linux-retained-console-ram.img`,
SHA-256 `927acc080bc99af61f243096dadddca98b635c2d12356a69e25d41ddfd5ee4dd`.
The raw Android v4 artifact passed DarwinBoot preflight, full payload
comparison and Linux/DTB/menu readback. Tablet testing is pending.

### Recovery Ignition control and Linux console retention

The combined image adds **Recovery without Ignition**. This entry passes
`-elden-recovery-skip-ignition` to the Elden development kernel. Before
starting PID 1, the kernel checks the Elden device-tree identity, the
native UFS root options and the result of the existing root authentication
call. Once those checks pass, it publishes dyld's `skipIgnition` bit and
prints `Elden Recovery dyld skipIgnition enabled after root authentication`.
The regular Recovery entry provides a control run with the same PID 1
traces. This option tests the suspected AppleImage4 wait identified by the
prior `sysctl` trace. Recovery GUI startup still requires a device result.

The Linux and slot entries use the `keep_bootcon` EFI diagnostic kernel.
This retains the boot console during the handoff to `tty0`, which is where
the previous tablet photo ended. It can show whether Linux reaches the
installed rootfs or reports a later error.

The 4 KiB ARM64e build, Recovery prelink, DarwinBoot preflight, and embedded
Linux, DTB and menu readback passed. The `boot_a` image also passed AVB
verification with the stock image's key and rollback metadata. Both MacBook
copies matched the build-host SHA-256 values.

| MacBook image | SHA-256 |
| --- | --- |
| `~/Downloads/elden-recovery-linux-fixes-ram.img` | `8bd2593c3a485e137d07e0d89bd0cf6b1cdce9ec547baaf7d694622f6787233f` |
| `~/Downloads/elden-recovery-linux-fixes-boot-a.img` | `98b443abee8a6737625586a66829e941aee96170fab7ca2375013292a2626ce2` |

The running slot B Linux system wrote the signed image to inactive
`boot_a` over SSH. The full partition readback matched the signed SHA-256
above. The previous `boot_a` image was backed up and verified before the
write. `boot_b` matched its original Linux image before and after the
write, both boot partitions returned to read-only, and slot B remained
active with UFS boot LUN 2.

Boot slot A to check for the root-authentication marker and the first
Recovery service to start. On a separate boot, select **Elden Linux -
installed rootfs** and capture the output after `tty0` registers.

### Recovery USB console

On the prior image, XNU UUID `2035771F-40F1-3041-B86B-398A501E830F`
printed AppleImage4 nonce-slot and root power-domain warnings during
security policy initialization. The subsequent panic came from PID 1
exiting in dyld with `Library not loaded: /usr/lib/libSystem.B.dylib`.
The verified 25F71 BaseSystem has the ARM64e cache and its subcaches under
`/System/Library/dyld`, and the cache map lists `libSystem.B.dylib`.
The failure to select or map that cache still needs a trace.

**Recovery USB Console** uses the **Recovery without Ignition** arguments
plus `elden-usb-debug=1`. The bootstrap wrapper's `--recovery-usb-core`
option accepts the `usb` directory from QcomXnuUsbDebug commit
`ab6e3635c4b915c248e04fc042ffaea28026b207`. The built-in XNU worker starts
before MAC policy initialization, checks the Elden ADT identity and
requires inherited USB power, device role and an identity DMA route.
It reads the existing retained log in bounded chunks and sends the early
buffer and live PID 1 output through USB CDC after the host opens the port.
The display continues to show boot progress if the inherited USB state
does not pass the checks.

The 4 KiB kernel, Recovery prelink, DarwinBoot preflight, raw Android v4
payload and Linux/DTB/menu readback passed host verification. The log
cursor passed prefix, wraparound, overwrite, contention and live replay
tests. The signed image passed AVB verification.

| MacBook image | SHA-256 |
| --- | --- |
| `~/Downloads/elden-recovery-usb-console-ram.img` | `707ae86204694cb77bdce3100c0fc222ad22937fa3df68a8750ae4027f7b33b6` |
| `~/Downloads/elden-recovery-usb-console-boot-a.img` | `2da5507f102fa5bd7b8cf2c2ed3b82a292aa627b25f978fc6cb940424163ed91` |

The running slot B Linux system wrote the signed image to inactive
`boot_a` over SSH. Full readback matched the signed SHA-256 above. The
previous `boot_a` was backed up and verified before the write. The
original `boot_b` digest matched before and after, both partitions
returned to read-only, and slot B remained active with UFS boot LUN 2.

Boot **Recovery USB Console** with the cable connected. On the MacBook,
find the new `/dev/cu.usbmodem…` device and open its actual path with
`screen -L /dev/cu.usbmodem… 115200`. Screen writes `screenlog.0` to the
current directory. Confirm the capture contains `ELDEN_USB` and XNU
output. If enumeration fails, record the last `ELDEN_USB` line from the
tablet display.

### Manage UniXNU and Recovery over USB

The USB management image loads `EldenUsbMenu.efi` from the embedded
payload. It starts the CDC device with serial `ELDEN-UEFI` after checking
the firmware's USB clocks, device role, and inherited SID 0x40 DMA route.
UniXNU prefers this interface for `list`, `status`, and `boot <id>`.
`boot-confirmed slot-a` and `boot-confirmed slot-b` explicitly confirm
the native A/B action from the USB console. The read-only `slot-status`
entry returns its result over the same link.

The firmware USB driver stops before the kernel takes over. **Recovery
USB Console** then starts the XNU CDC device with serial `ELDEN-XNU`.
Its opt-in `elden-usb-management=1` flag enables `reboot` and
`reboot-fastboot`. If an active host drains a panic report, XNU sends
`ELDEN PANIC END` and requests a fastboot reset. A panic before the USB
worker starts still uses the framebuffer diagnostics.

Build the UEFI payload driver using `build-ufs-stage2.py --usb-core`
and the firmware using `build_phase2_firmware.py --usb-menu`. Pass the
resulting driver to `make_phase2_payload.py --usb-menu-driver` together
with `--slot-switch`. Use the same pinned DWC3 sources for the XNU build
with `build-elden-macos26-bootstrap.py --recovery-usb-core`.

The MacBook controller uses the system Python serial APIs. From its
Downloads directory, it can load the RAM image, select an entry, and
record firmware and Recovery output in one log:

```sh
python3 elden-usb-control.py boot recovery-usb \
  --ram-image elden-recovery-panic-usb-ram.img --seconds 440 --log elden-boot.log
```

`boot slot-status` runs the read-only preflight. `list` displays menu IDs,
and `fastboot` requests a restart and checks for Elden in bootloader
fastboot. Slot selection requires the explicit `slot a` or `slot b`
operation. The host tool appends to the requested log file across USB
re-enumeration.

The `r35-usb-management` RAM image is 81,952,768 bytes and has SHA-256
`341137d7de8b110613843007efaf92bd35fe3bf0e63f419a6e95111f4f4e619f`.
DarwinBoot preflight and the embedded firmware, kernel, device tree,
and boot menu checks passed. A host pseudo-terminal test verified that
the controller sends `boot slot-status` and records the returned result.

The first USB reboot request disconnected Linux before the MacBook could
see bootloader fastboot. Once fastboot was visible, the MacBook loaded
the 20-second return-to-restart variant of `r35-usb-management`, SHA-256
`cd065d6f43935901a4bede611a7afbda34e5c91b17691b25e209ad07f9c4a643`.
Fastboot accepted the transfer and the tablet reached the visible UniXNU
menu. The MacBook saw no CDC interface and received no `slot-status`
result. The test used `fastboot boot` and performed no partition write.

On the untimed `r35` RAM image, macOS attached the USB serial device as
`/dev/cu.usbmodemELDEN_UEFI1`. With underscore handling in the host script,
`list` returned all nine entries and `boot slot-status` returned
`result=-2 open=Not Found close=Success`. The packed payload was missing
the current ABL FDT required for UFS platform admission. SEC now saves
the live pointer in the payload HOB. The read-only RAM block driver
inserts it into the reserved FAT mailbox on reads of sector zero, so
the slot app can validate and copy the actual bootloader tree.

The MacBook also selected `recovery-usb` and captured the UniXNU loader
handoff. No `ELDEN-XNU` device or XNU log appeared in the 110-second
capture window. The two host logs are `elden-usb-management-device.log`
and `elden-usb-management-recovery.log` in the MacBook's Downloads.

The `r36-live-fdt` build retained the Recovery kernel and Linux payloads
from `r35`. Its packed FAT readback included the reserved FDT mailbox.
The 96 MiB signed `elden-usb-management-live-fdt-boot-a.img` passed AVB
signature and payload verification and matched the MacBook copy at SHA-256
`01db309f90b1e49a3b2ed54a546dfdd97836e91c28b107c4757b97d7c72e4b4b`.
The previous signed slot A image remained available with the SHA-256
recorded in the Recovery USB Console section.

Bootloader fastboot reported active slot A and successful slot B. It
accepted the write to `boot_a` and rebooted into the installed UniXNU
USB console. `list` returned all nine entries. `boot slot-status`
returned `result=-2 open=Unsupported close=Success`. The previous
`Not Found` return came from FDT discovery. The newer return leaves
platform identification, execution state and MMIO admission to check.
The Elden firmware memory map omits the `0x01d84000` UFS host window,
which the UFS application requires to be identity mapped. The specific
rejected check still needs a hardware trace.

The bootloader lacks `fetch`, so post-flash verification consists of
fastboot's successful write and the installed boot and USB tests. The
MacBook then sent `boot fastboot`, verified bootloader fastboot on slot A,
rebooted, and listed the nine entries over USB again. The MacBook log is
`~/Downloads/elden-usb-management-slot-a.log`.

### Recovery panic capture and restart

The installed `r36` Recovery test reproduced the dyld panic in PID 1.
The MacBook saw the XNU CDC device after the initial 90-second capture
window, but neither logs nor a `ping` reply came through. The on-device
panic record reported `Library not loaded: /usr/lib/libSystem.B.dylib`.
The old panic hook checked for an already-open CDC session and returned
without polling. Once XNU panicked, its normal USB worker could no longer
service the late connection.

The revised hook polls for up to 12 seconds for the host to open CDC.
It then drains the retained log, sends the framed panic, and requests a
fastboot reboot after TX completes. The MacBook controller waits for
bootloader fastboot after seeing the end of the panic record.

The `r37-recovery-panic-usb` RAM image was built and verified at SHA-256
`cd4ffad892d8aa33603c05ba817070c38a5f47a7c27fcd4c516493745bf0c5af`.
The MacBook copy is `~/Downloads/elden-recovery-panic-usb-ram.img`.
On the tablet the host first received UniXNU, then XNU CDC about three
minutes after the loader handoff. XNU replayed the retained log from
`arm_init`. The MacBook reconnected, sent `reboot-fastboot`, recorded
`ELDEN ACK reboot-fastboot`, and verified the tablet in bootloader
fastboot on slot A. The MacBook log is
`~/Downloads/elden-recovery-panic-usb.log`.

The signed `elden-recovery-panic-usb-boot-a.img` passed AVB verification
and matched the MacBook copy at SHA-256
`14f8a4b6e975a1321bea15f332d26383e8514f9a65575841acfb6ce6d4527991`.
Fastboot wrote `boot_a`, then the installed UniXNU menu returned all nine
entries over USB. Slot B was marked successful and was left untouched.

On a second run from installed slot A, the MacBook captured the kernel
log from `arm_init`, the macOS Base System mount, and the `launchd` exec.
It received the complete framed panic including dyld's `no dyld cache`
reason, then detected bootloader fastboot with no host reboot command.
The MacBook log is `~/Downloads/elden-recovery-panic-usb-slot-a.log`.

The **Recovery USB Console** entry sets `elden-usb-timeout=90`. Once
XNU reaches its startup hook, a kernel callout requests fastboot if no
host opens the console within 90 seconds. The callout can run after the
loader hands control to XNU if XNU's scheduler is still running. The
no-host timeout has not been exercised on the tablet. The
UEFI boot-services watchdog expires only during firmware execution and
is disabled at `ExitBootServices()`.
[UEFI Boot Services specification](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html)

### Adreno 830 GMU firmware

On the tablet, Linux `7.2.8-y700-r2-androidcrypt1` and `vulkaninfo`
identified the Adreno 830 through Mesa Turnip. The live device tree
reported GPU `0x03d00000`, GMU `0x03d37000`, and GPU SMMU `0x03da0000`.
The GPU uses stream IDs 0 and 1 and GIC SPI 300. The GMU uses stream ID 5
and GIC SPIs 304 and 305. The Linux A8xx driver and device tree describe
these interfaces. [A8xx driver](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a8xx_gpu.c) ·
[SM8750 device tree](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8750.dtsi)

A headless test selected the Turnip ICD, submitted a Vulkan buffer fill,
waited on its fence and verified the 4096-byte CPU readback on the tablet.
Linux also enumerated the 4 GiB `macosroot` partition as read-only.

The device Linux installation supplied the following firmware identities.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `qcom/gen80000_gmu.bin` | 88896 | `1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5` |
| `qcom/gen80000_sqe.fw` | 111892 | `9c8f009bea112d7c06dd2ccb67e889999459e53b92f0f4859f5ea0db067ad6b5` |
| `qcom/gen80000_aqe.fw` | 25340 | `f04cb97e1e629b9003a26c5670b8f7e5b7bfeca95d35b2abd581fd8672efed20` |
| `qcom/sm8750/gen80000_zap.mbn` | 12088 | `8cf4191428da972095df365f1d3b171f45bbed806d6c0f3310cfc4dee00e04ae` |

The Adreno 830 loader uses UniXNU's shared `AdrenoFirmware` parser and the
SM8750 GMU address ranges. It
validates all loadable blocks before calling the transport and stops when
the transport reports failure. The same test passed with sanitizers on
the build host and against the installed firmware on the tablet under
Linux. It verifies the four loadable blocks and refuses the wrong chip,
truncation, overlapping blocks, and out-of-range destinations.
[Loader source](../drivers/Adreno830/EldenAdreno830.c)

The macOS bootstrap wrapper's `--adreno830-gmu-firmware` option checks
the local GMU file against its SHA-256 before embedding it in an opt-in
XNU probe. The **Adreno 830 GMU Probe** entry enables the probe. It counts
the validated writes without touching MMIO. The expected boot line is
`ELDEN-A830: GMU image result=0 blocks=4 bytes=88656`.

The 4 KiB ARM64e probe passed prelink, DarwinBoot preflight and embedded
image verification. The verified MacBook copy is
`~/Downloads/elden-macos26-adreno830-gmu-ram.img` with SHA-256
`8673a7d39f50735576f21501be34be8a392fe0ca578e21707b5019b2702dc3b6`.
The on-device XNU probe result is pending.

Hardware bring-up requires GMU power and clock control, an owned GPU
SMMU context, and a bounded GMU boot and HFI handshake. A command ring,
fences, rendering, and the macOS Metal interface follow those milestones.
