# Related ports reviewed for Elden

Reviewed 2026-11-13 against the pinned primary sources below. These are source
and documentation findings. Other projects' published hardware results have
not been reproduced on Elden.

## Highest priority: native Adreno 830 and Metal

[Adreno830 `1a132105`](https://github.com/Shad0w23333/Adreno830/tree/1a1321056ffc5428ae925f4680093718b46ca7c2)
already contains SM8750 power, RPMh, SMMU, GMU/HFI, command processor, secure
firmware loading, fence and accelerator code. Its maintainers report a Metal
desktop on OnePlus 13T with iPadOS 17.7.11. The reusable controller code and
[Pagani board inputs](https://github.com/Shad0w23333/UniXNU-Pagani_SM8750/tree/25b110978fec628cc29bf5bd1b5b5ce830156bfd)
provide a stronger starting point than implementing every stage again.

Pagani's chip ID is `0x44050001`; Elden's is `0x44050000`. The exact Elden Linux
catalog has different speed-bin mapping, GMU OPPs, dependency votes and HFI
policy. Keep these in the board/SoC data layer and require measured inputs.
Review controller ownership, register access and the macOS kernel ABI before
enabling any hardware stage. Retain the examined MIT notices when reusing code.

[AdrenoMetal `73d582fd`](https://github.com/UniXNU/AdrenoMetal/tree/73d582fd4d5772b1f5fa83581a25077a16f2b6cd)
provides a Metal frontend through metal2vulkan and Turnip, including surface
lifetimes and macOS cache handling. The
[DroidVM macOS build adapters](https://github.com/Droid-VM/macOS-DroidVM/blob/9db2f6f263173b660536530c2d102f480280cdb3/scripts/build-metal-bridge-macos.py)
are useful ARM64e integration references. Audit a single matching
`NativeAdrenoABI.h`, PAC conventions and IOKit interfaces across the kernel,
frontend and Turnip. Preserve the currently qualified component pins until an
explicit integration candidate passes testing.

## Kernel compatibility and Recovery

[macOS-DroidVM `9db2f6f`](https://github.com/Droid-VM/macOS-DroidVM/blob/9db2f6f263173b660536530c2d102f480280cdb3/patches/xnu-12377/README.md)
documents substantial Apple userspace running on a 4 KiB XNU. Useful comparison
points are shared-cache page locking, code-signing tails, PAC and IOKit ABI
adaptations. Its userspace target and Gunyah transport differ from Elden's
25F71 native EL1 path. Compare individual invariants instead of transplanting
the entire patch series.

Its
[VM startup code](https://github.com/Droid-VM/macOS-DroidVM/blob/9db2f6f263173b660536530c2d102f480280cdb3/scripts/run-macos-vm.py)
records a Language Chooser wait in Bluetooth coordinator initialization when
`bluetoothd` is disabled. Check that stack only after Elden starts the chooser.
A virtual Bluetooth transport would not qualify the tablet's physical radio.

[SHENG's paired 25F71 reference](https://github.com/CrossDarwin/macOS-ForSheng/blob/ca57db73c066162323e77156a9f0f1d72b1de4ff/docs/status.md)
remains useful for software scanout, framebuffer ownership and bounded Recovery
diagnostics. Its grouped guest/FSKit/keychain workarounds should not replace
Elden's now-tested original `lifs` dependency. Full GUI tools and installation
still require separate evidence.

## Testing and architecture

[Snapintosh](https://github.com/Snapintosh/Snapintosh/blob/940eeb95ba637528dc45ba098e6b8091163dd489/docs/ARCHITECTURE.md)
uses an EL2 monitor that Elden's present handoff cannot own. Its staged GPU
completion checks, pixel comparisons and automated test cycles are useful
without adopting that boot architecture.
[m1n1](https://asahilinux.org/docs/sw/m1n1-user-guide/) similarly offers useful
USB control/logging and reconnect patterns; its Apple hardware drivers do not
apply to SM8750.

No verified public fix was found for Elden's Boot Picker to Language Chooser
transition. Apple's
[VM Recovery start option](https://developer.apple.com/documentation/virtualization/vzmacosvirtualmachinestartoptions)
could provide a matched-release control for observing the real sysctl, NVRAM,
environment and service order on the Mac. It does not prove that a guest-mode
NVRAM variable alone selects the desired screen.

## Integration order

1. Continue original Recovery startup diagnosis on the working software display.
2. Reuse the SM8750 controller implementation behind Elden-verified inputs.
3. Qualify power/clocks, reset-held firmware transfer, GMU/HFI and CP fence
   readback in bounded hardware stages with automatic recovery.
4. Validate the shared kernel/userspace ABI, then Vulkan pixels and Metal
   IOSurface presentation. Measure memory use and submission latency before
   enabling DVFS or power collapse.

Keep hardware transport, macOS ABI compatibility and Recovery behavior
independently testable. Buffer reuse and reduced diagnostic contention are
optimization candidates after the corresponding path is correct.
