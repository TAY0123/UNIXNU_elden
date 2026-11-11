/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include "EldenUsbMenuProtocol.h"
#include "EfiCompat.h"
#include "Generated/Dwc3.h"
#include "Generated/EldenUsbResources.h"

STATIC EFI_GUID mSerialGuid = ELDEN_USB_MENU_SERIAL_GUID;
// Qualcomm's platform USB initialization event, also signaled by FastbootLib.
STATIC EFI_GUID mInitUsbGuid = {0x1c0cffce, 0xfc8d, 0x4e44,
    {0x8c, 0x78, 0x9c, 0x9e, 0x5b, 0x53, 0x0d, 0x36}};
STATIC UINT64 mContext[DWC3_CONTEXT_BYTES / sizeof(UINT64)];
STATIC EFI_PHYSICAL_ADDRESS mArena;
STATIC EFI_EVENT mPoll, mExit;
STATIC BOOLEAN mActive;
STATIC UINT32 mSavedHs, mSavedSs;

STATIC struct dwc3 *Core(VOID) { return (struct dwc3 *)(VOID *)mContext; }

STATIC UINT32 ReadReg(UINT64 Address)
{
    UINT32 Value = *(volatile UINT32 *)(UINTN)Address;
    __asm__ volatile("dmb oshld" ::: "memory");
    return Value;
}

STATIC VOID WriteReg(UINT64 Address, UINT32 Value)
{
    *(volatile UINT32 *)(UINTN)Address = Value;
    __asm__ volatile("dsb sy" ::: "memory");
}

STATIC BOOLEAN IdentityMapped(UINT64 Address, UINTN Bytes, BOOLEAN Write)
{
    UINT64 Saved, Par;
    BOOLEAN Valid = TRUE;
    __asm__ volatile("mrs %0, par_el1" : "=r"(Saved));
    for (UINT64 Page = Address; Page < Address + Bytes; Page += EFI_PAGE_SIZE) {
        if (Write) {
            __asm__ volatile("at s1e1w, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(Par) : "r"(Page) : "memory");
        } else {
            __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(Par) : "r"(Page) : "memory");
        }
        if ((Par & 1) || (Par & 0x000ffffffffff000ULL) != Page) {
            Valid = FALSE;
            break;
        }
    }
    __asm__ volatile("msr par_el1, %0\n\tisb" :: "r"(Saved) : "memory");
    return Valid;
}

STATIC UINT32 Smmu(UINT32 Offset) { return ReadReg(ELDEN_USB_SMMU_PHYS + Offset); }

STATIC BOOLEAN IdentityDma(VOID)
{
    UINT32 Cr0 = Smmu(0), Id0 = Smmu(0x20), Id1 = Smmu(0x24), Id2 = Smmu(0x28);
    UINT32 Streams = Id0 & 255, Banks = Id1 & 255, Pages = 1u << (((Id1 >> 28) & 7) + 1);
    if ((Cr0 & 9) || (Id0 & 0x48000000u) != 0x48000000u || !(Id2 & 0x1000) ||
        (Id1 & 0x80000000u) || !Streams || !Banks || Banks > Pages || Pages * 8192u > ELDEN_USB_SMMU_BYTES ||
        (Id2 & 15) > 5 || ((Id2 >> 4) & 15) > 5) return FALSE;
    UINT32 Slot = 0, Matches = 0;
    for (UINT32 I = 0; I < Streams; ++I) {
        UINT32 Smr = Smmu(0x800 + 4 * I), Mask = (Smr >> 16) & 0x7fff;
        if ((Smr & 0x80000000u) && !((Smr ^ ELDEN_USB_SID) & 0xffff & ~Mask)) {
            if (Mask || (Smr & 0xffff) != ELDEN_USB_SID) return FALSE;
            Slot = I;
            ++Matches;
        }
    }
    if (Matches != 1) return FALSE;
    UINT32 Route = Smmu(0xc00 + 4 * Slot);
    if (((Route >> 16) & 3) == 1) return TRUE;
    UINT32 Bank = Route & 255;
    return !((Route >> 16) & 3) && Bank < Banks && Bank >= ((Id1 >> 16) & 255) &&
           ((Smmu(0x1000 + 4 * Bank) >> 16) & 3) == 1 && (Smmu(0x1800 + 4 * Bank) & 1) &&
           !(Smmu((Pages + Bank) * EFI_PAGE_SIZE) & 1);
}

STATIC uint32_t DwRead(void *Unused, uint32_t Offset)
{ (VOID)Unused; return ReadReg(ELDEN_USB_DWC_PHYS + Offset); }
STATIC void DwWrite(void *Unused, uint32_t Offset, uint32_t Value)
{ (VOID)Unused; WriteReg(ELDEN_USB_DWC_PHYS + Offset, Value); }
STATIC void DwDelay(void *Unused, uint32_t Usec)
{ (VOID)Unused; gBS->Stall(Usec); }
STATIC void DwBarrier(void *Unused)
{ (VOID)Unused; __asm__ volatile("dsb sy" ::: "memory"); }
STATIC void DwSync(void *Unused, void *Buffer, size_t Bytes, enum dwc3_sync_direction Direction)
{
    (VOID)Unused;
    for (UINTN At = (UINTN)Buffer & ~(UINTN)63; At < (UINTN)Buffer + Bytes; At += 64) {
        if (Direction == DWC3_TO_DEVICE) __asm__ volatile("dc cvac, %0" :: "r"(At) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(At) : "memory");
    }
    DwBarrier(NULL);
}

STATIC VOID EFIAPI Poll(EFI_EVENT Event, VOID *Context)
{
    (VOID)Event;
    (VOID)Context;
    if (mActive && dwc3_poll(Core(), 16) < 0) {
        if (dwc3_stop(Core()) == 0) {
            WriteReg(ELDEN_USB_QSCRATCH_PHYS + 0x10, mSavedHs);
            WriteReg(ELDEN_USB_QSCRATCH_PHYS + 0x30, mSavedSs);
        }
        mActive = FALSE;
    }
}

STATIC VOID EFIAPI Handoff(EFI_EVENT Event, VOID *Context)
{
    (VOID)Event;
    (VOID)Context;
    if (mActive && dwc3_stop(Core()) == 0) {
        WriteReg(ELDEN_USB_QSCRATCH_PHYS + 0x10, mSavedHs);
        WriteReg(ELDEN_USB_QSCRATCH_PHYS + 0x30, mSavedSs);
    }
    mActive = FALSE;
    // DMA uses reserved pages for the rest of this boot even if stop fails.
}

STATIC EFI_STATUS EFIAPI Reset(EFI_SERIAL_IO_PROTOCOL *This)
{ (VOID)This; return mActive ? EFI_SUCCESS : EFI_NOT_READY; }

STATIC EFI_STATUS EFIAPI SetAttributes(EFI_SERIAL_IO_PROTOCOL *This, UINT64 Baud, UINT32 Depth,
    UINT32 Timeout, EFI_PARITY_TYPE Parity, UINT8 Data, EFI_STOP_BITS_TYPE Stop)
{
    (VOID)This; (VOID)Depth; (VOID)Timeout;
    return ((!Baud || Baud == 115200) && (!Data || Data == 8) &&
            (Parity == DefaultParity || Parity == NoParity) &&
            (Stop == DefaultStopBits || Stop == OneStopBit)) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC EFI_STATUS EFIAPI SetControl(EFI_SERIAL_IO_PROTOCOL *This, UINT32 Control)
{
    (VOID)This;
    return (Control & ~(EFI_SERIAL_REQUEST_TO_SEND | EFI_SERIAL_DATA_TERMINAL_READY)) ?
           EFI_UNSUPPORTED : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI GetControl(EFI_SERIAL_IO_PROTOCOL *This, UINT32 *Control)
{
    (VOID)This;
    if (!Control) return EFI_INVALID_PARAMETER;
    EFI_TPL Tpl = gBS->RaiseTPL(TPL_CALLBACK);
    *Control = (!mActive || !dwc3_rx_pending(Core())) ? EFI_SERIAL_INPUT_BUFFER_EMPTY : 0;
    if (!mActive || !dwc3_queued(Core())) *Control |= EFI_SERIAL_OUTPUT_BUFFER_EMPTY;
    if (mActive && dwc3_ready(Core())) *Control |= EFI_SERIAL_CARRIER_DETECT;
    gBS->RestoreTPL(Tpl);
    return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Write(EFI_SERIAL_IO_PROTOCOL *This, UINTN *Bytes, VOID *Buffer)
{
    (VOID)This;
    if (!Bytes || !Buffer) return EFI_INVALID_PARAMETER;
    UINTN Requested = *Bytes;
    EFI_TPL Tpl = gBS->RaiseTPL(TPL_CALLBACK);
    *Bytes = mActive ? dwc3_queue(Core(), Buffer, Requested) : 0;
    gBS->RestoreTPL(Tpl);
    return *Bytes == Requested ? EFI_SUCCESS : EFI_TIMEOUT;
}

STATIC EFI_STATUS EFIAPI Read(EFI_SERIAL_IO_PROTOCOL *This, UINTN *Bytes, VOID *Buffer)
{
    (VOID)This;
    if (!Bytes || !Buffer) return EFI_INVALID_PARAMETER;
    UINTN Requested = *Bytes;
    EFI_TPL Tpl = gBS->RaiseTPL(TPL_CALLBACK);
    *Bytes = mActive && dwc3_ready(Core()) ? dwc3_read(Core(), Buffer, Requested) : 0;
    gBS->RestoreTPL(Tpl);
    return *Bytes == Requested ? EFI_SUCCESS : EFI_TIMEOUT;
}

STATIC EFI_SERIAL_IO_MODE mMode = {0, 0, 115200, DWC3_RX_BYTES, 8, NoParity, OneStopBit};
STATIC EFI_SERIAL_IO_PROTOCOL mSerial = {
    EFI_SERIAL_IO_PROTOCOL_REVISION, Reset, SetAttributes, SetControl, GetControl, Write, Read, &mMode, NULL
};

STATIC VOID EFIAPI InitEvent(EFI_EVENT Event, VOID *Context) { (VOID)Event; (VOID)Context; }

EFI_STATUS EFIAPI EldenUsbMenuEntry(EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
    (VOID)SystemTable;
    EFI_LOADED_IMAGE_PROTOCOL *Loaded;
    EFI_STATUS Status = gBS->HandleProtocol(Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
    if (EFI_ERROR(Status)) return Status;
    if (Loaded->LoadOptionsSize != StrSize(L"elden") || !Loaded->LoadOptions ||
        StrnCmp(Loaded->LoadOptions, L"elden", ARRAY_SIZE(L"elden"))) return EFI_UNSUPPORTED;

    UINT64 El, Ctr;
    __asm__ volatile("mrs %0, CurrentEL\n\tmrs %1, ctr_el0" : "=r"(El), "=r"(Ctr));
    if (El != 4 || (4u << ((Ctr >> 16) & 15)) != 64 ||
        !IdentityMapped(ELDEN_USB_GCC_PHYS, ELDEN_USB_GCC_BYTES, FALSE) ||
        !IdentityMapped(ELDEN_USB_DWC_PHYS, ELDEN_USB_DWC_BYTES, TRUE) ||
        !IdentityMapped(ELDEN_USB_SMMU_PHYS, ELDEN_USB_SMMU_BYTES, FALSE) ||
        !IdentityMapped(ELDEN_USB_QSCRATCH_PHYS & ~(UINT64)EFI_PAGE_MASK, EFI_PAGE_SIZE, TRUE)) {
        Print(L"ELDEN USB: controller MMIO not mapped\r\n");
        return EFI_UNSUPPORTED;
    }
    EFI_EVENT Signal;
    Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, InitEvent, NULL, &mInitUsbGuid, &Signal);
    if (EFI_ERROR(Status)) return Status;
    gBS->SignalEvent(Signal);
    gBS->CloseEvent(Signal);
    UINT32 Power = ReadReg(ELDEN_USB_GCC_PHYS + ELDEN_USB_GDSCR);
    UINT32 Clocks = ReadReg(ELDEN_USB_GCC_PHYS + ELDEN_USB_MASTER_CLK) |
                    ReadReg(ELDEN_USB_GCC_PHYS + ELDEN_USB_CFG_CLK);
    if (!(Power & 0x80000000u) || (Power & 1) || (Clocks & 0x80000000u)) {
        Print(L"ELDEN USB: controller power or clocks unavailable\r\n");
        return EFI_NOT_READY;
    }
    if (((DwRead(NULL, 0xc110) >> 12) & 3) != 2 || (DwRead(NULL, 0xc704) & (1u << 31))) {
        Print(L"ELDEN USB: controller not idle in device mode\r\n");
        return EFI_NOT_READY;
    }
    if (!IdentityDma()) {
        Print(L"ELDEN USB: inherited SID 0x40 DMA route refused\r\n");
        return EFI_UNSUPPORTED;
    }
    mArena = MAX_UINT32;
    Status = gBS->AllocatePages(AllocateMaxAddress, EfiReservedMemoryType,
                               EFI_SIZE_TO_PAGES(DWC3_DMA_BYTES), &mArena);
    if (EFI_ERROR(Status)) return Status;
    if (!IdentityMapped(mArena, DWC3_DMA_BYTES, TRUE)) {
        gBS->FreePages(mArena, EFI_SIZE_TO_PAGES(DWC3_DMA_BYTES));
        return EFI_UNSUPPORTED;
    }

    const struct dwc3_platform Io = {NULL, DwRead, DwWrite, DwDelay, DwSync, DwBarrier, NULL};
    const struct dwc3_identity Id = {0x1209, 0x7004, "Elden Lab", "UniXNU Firmware", "ELDEN-UEFI"};
    int Result = dwc3_init(mContext, sizeof(mContext), &Io, &Id,
                          (VOID *)(UINTN)mArena, mArena, DWC3_DMA_BYTES);
    if (Result) {
        Print(L"ELDEN USB: DWC3 initialization failed %d\r\n", Result);
        return EFI_DEVICE_ERROR;
    }
    mSavedHs = ReadReg(ELDEN_USB_QSCRATCH_PHYS + 0x10);
    mSavedSs = ReadReg(ELDEN_USB_QSCRATCH_PHYS + 0x30);
    WriteReg(ELDEN_USB_QSCRATCH_PHYS + 0x10, mSavedHs | (1u << 20) | (1u << 28));
    WriteReg(ELDEN_USB_QSCRATCH_PHYS + 0x30, mSavedSs | (1u << 24));
    mActive = TRUE;
    if (dwc3_start(Core()) != 0) {
        Handoff(NULL, NULL);
        return EFI_DEVICE_ERROR;
    }
    Status = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Poll, NULL, &mPoll);
    if (!EFI_ERROR(Status)) Status = gBS->SetTimer(mPoll, TimerPeriodic, EFI_TIMER_PERIOD_MILLISECONDS(2));
    if (!EFI_ERROR(Status)) Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Handoff,
                                                       NULL, &gEfiEventBeforeExitBootServicesGuid, &mExit);
    if (!EFI_ERROR(Status)) Status = gBS->InstallProtocolInterface(&Image, &mSerialGuid, EFI_NATIVE_INTERFACE, &mSerial);
    if (EFI_ERROR(Status)) {
        if (mPoll) gBS->CloseEvent(mPoll);
        if (mExit) gBS->CloseEvent(mExit);
        Handoff(NULL, NULL);
    } else {
        STATIC CHAR8 Banner[] = "UNIXNU: ELDEN-UEFI USB console v1\r\n";
        UINTN Bytes = sizeof(Banner) - 1;
        Write(&mSerial, &Bytes, Banner);
    }
    return Status;
}
