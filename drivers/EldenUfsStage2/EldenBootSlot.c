/* SPDX-License-Identifier: GPL-3.0-only */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include "EldenUfsHardware.h"
#include "EldenUsbMenuProtocol.h"

STATIC ELDEN_UFS_HARDWARE mHardware;
STATIC struct elden_slot_change mChange;
STATIC EFI_GUID mUsbGuid = ELDEN_USB_MENU_SERIAL_GUID;
STATIC EFI_SERIAL_IO_PROTOCOL *mUsb;

STATIC INTN
GetTarget(CONST EFI_LOADED_IMAGE_PROTOCOL *Image)
{
    if (Image->LoadOptions == NULL) return -1;
    if (Image->LoadOptionsSize == StrSize(L"inspect") &&
        StrnCmp(Image->LoadOptions, L"inspect", ARRAY_SIZE(L"inspect")) == 0) return 0;
    if (Image->LoadOptionsSize == StrSize(L"inspect-usb") &&
        StrnCmp(Image->LoadOptions, L"inspect-usb", ARRAY_SIZE(L"inspect-usb")) == 0) return 0;
    if (Image->LoadOptionsSize == StrSize(L"a")) {
        if (StrnCmp(Image->LoadOptions, L"a", ARRAY_SIZE(L"a")) == 0) return 1;
        if (StrnCmp(Image->LoadOptions, L"b", ARRAY_SIZE(L"b")) == 0) return 2;
    }
    return -1;
}

STATIC VOID
WaitForKey(VOID)
{
    UINT32 Control;
    if (mUsb && !EFI_ERROR(mUsb->GetControl(mUsb, &Control)) &&
        (Control & EFI_SERIAL_CARRIER_DETECT)) return;
    if (gST->ConIn != NULL) {
        EFI_INPUT_KEY Key;
        UINTN Index;
        Print(L"Press a key to return to the UniXNU menu.\r\n");
        if (!EFI_ERROR(gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index))) {
            gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
        }
    }
}

STATIC VOID
ReportUsb(INTN Target, int Result, EFI_STATUS Status, EFI_STATUS Closed)
{
    if (mUsb == NULL) return;
    CHAR8 Line[160];
    UINTN Bytes = AsciiSPrint(Line, sizeof(Line),
        "ELDEN SLOT: old=%u target=%u result=%d open=%r close=%r\r\n",
        mChange.current, (UINT32)Target, EFI_ERROR(Closed) ? KSHIM_STORAGE_IO : Result, Status, Closed);
    mUsb->Write(mUsb, &Bytes, Line);
}

EFI_STATUS EFIAPI
UefiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    (VOID)SystemTable;
    EFI_LOADED_IMAGE_PROTOCOL *Image;
    EFI_STATUS Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
    if (EFI_ERROR(Status)) return Status;
    INTN Target = GetTarget(Image);
    if (Target < 0) return EFI_INVALID_PARAMETER;
    gBS->LocateProtocol(&mUsbGuid, NULL, (VOID **)&mUsb);

    Print(L"ELDEN SLOT: checking UFS and mirrored GPT state\r\n");
    Status = EldenUfsHardwareOpenSlot(Image->DeviceHandle, &mHardware);
    int Result = KSHIM_STORAGE_IO;
    if (!EFI_ERROR(Status)) {
        Result = elden_ufs_plan_slot(&mHardware.Session, &mChange, (UINT32)Target);
        if (!Result) {
            Print(L"ELDEN SLOT: active %c; UFS boot LUN %u\r\n",
                  L'A' + mChange.current - 1, mChange.current);
            if (Target != 0 && mChange.current != (UINT32)Target) {
                Print(L"ELDEN SLOT: applying and verifying slot %c\r\n", L'A' + Target - 1);
                Result = elden_ufs_apply_slot(&mHardware.Session, &mChange);
            }
        }
    }
    EFI_STATUS Closed = EldenUfsHardwareClose(&mHardware);
    ReportUsb(Target, Result, Status, Closed);
    if (EFI_ERROR(Closed) || Result == ELDEN_SLOT_UNCERTAIN) {
        Print(L"ELDEN SLOT: recovery required (result %d, controller %r)\r\n", Result, Closed);
        Print(L"Slot state may be incomplete. Automatic reboot stopped.\r\n");
        CpuDeadLoop();
    }
    if (Result) {
        Print(Result == ELDEN_SLOT_ROLLED_BACK ?
              L"ELDEN SLOT: switch failed, original metadata verified (result %d)\r\n" :
              L"ELDEN SLOT: preflight failed (result %d)\r\n", Result);
        WaitForKey();
        return EFI_DEVICE_ERROR;
    }
    if (Target == 0) {
        WaitForKey();
        return EFI_SUCCESS;
    }
    Print(L"ELDEN SLOT: slot %c verified; restarting\r\n", L'A' + Target - 1);
    gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    return EFI_DEVICE_ERROR;
}
