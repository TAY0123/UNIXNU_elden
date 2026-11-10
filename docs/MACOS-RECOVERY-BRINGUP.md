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
4 GiB tail, and ext4 undo files are retained privately. The new partition
awaits the paired Recovery media.

The next storage checkpoint is to deploy the paired BaseSystem to
`macosroot` and boot with `native-ufs-elden=1 native-ufs-root=1` to
verify read-only media publication and the APFS root mount. A 1.471 GiB
BaseSystem image was staged on the host. Its manifest and full OS window
readback matched the compiled transport. The available ISC seed has a
different digest and volume identities from the current driver contract,
so this image contains only the BaseSystem. Cold PHY
initialization, controller reset, MCQ, and persistent writes are outside
this firmware-handoff implementation.
