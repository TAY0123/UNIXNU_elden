/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Generated/EldenPonResources.h"
#include "Generated/EldenUsbRepeaterConfig.h"

#define SPMI_CHANNELS_PHYS 0x0c500000ULL
#define TLMM_GPIO29_PAGE (0x0f100000ULL + PTN_MUX_GPIO * EFI_PAGE_SIZE)

// ABI of the Pakala Qualcomm I2CDxe, revision 1.0.
typedef UINT32 (EFIAPI *QCOM_I2C_IO)(VOID *, VOID *, UINT32, UINT32, VOID *,
                                    UINT32, UINT32 *, UINT32);
typedef struct {
    UINT64 Revision;
    UINT32 (EFIAPI *Open)(UINT32, VOID **);
    QCOM_I2C_IO Read;
    QCOM_I2C_IO Write;
    VOID *Transfer;
    UINT32 (EFIAPI *Close)(VOID *);
} QCOM_I2C;

STATIC EFI_GUID mQcomI2cGuid = {0xb27ae8b1, 0x3e10, 0x4d07,
    {0xab, 0x5c, 0xeb, 0x9a, 0x6d, 0xc6, 0xfa, 0x8f}};
STATIC CONST CHAR8 *mRepeaterStep;
STATIC UINT32 mRepeaterI2cStatus, mRepeaterI2cBytes, mRepeaterId;
STATIC UINT64 mRepeaterEngine;

STATIC BOOLEAN RepeaterBus(VOID *Handle)
{
    // Client -> hardware context -> QUP configuration -> SE MMIO base.
    if (Handle == NULL) return FALSE;
    CONST VOID *Hw = *(CONST VOID *CONST *)Handle;
    if (Hw == NULL) return FALSE;
    CONST UINT64 *Config = *(CONST UINT64 *CONST *)Hw;
    if (Config == NULL) return FALSE;
    mRepeaterEngine = Config[3];
    return mRepeaterEngine == PTN_GENI_BASE;
}

STATIC BOOLEAN RepeaterIo(QCOM_I2C_IO Io, VOID *Handle, UINT32 Reg, UINT8 *Data, UINT32 Length)
{
    // 100 kHz, 7-bit address, 500 us clock stretch, default core settings.
    UINT32 Config[7] = {100, PTN_I2C_ADDRESS, 0, 500, 0, 0, 0};
    mRepeaterI2cBytes = 0;
    mRepeaterI2cStatus = Io(Handle, Config, Reg, 1, Data, Length, &mRepeaterI2cBytes, 2500);
    return mRepeaterI2cStatus == 0 && mRepeaterI2cBytes == Length;
}

STATIC EFI_STATUS ResetChannels(UINT64 *Observer, UINT64 *Writer)
{
    if (!IdentityMapped(ELDEN_SPMI_CORE_PHYS, ELDEN_SPMI_CORE_BYTES, FALSE) ||
        !IdentityMapped(ELDEN_SPMI_CONFIG_PHYS, ELDEN_SPMI_CONFIG_BYTES, FALSE) ||
        (ReadReg(ELDEN_SPMI_CORE_PHYS) >> 28) != 7) return EFI_UNSUPPORTED;
    UINT32 Count = ReadReg(ELDEN_SPMI_CORE_PHYS + 4) & 0x7ff;
    if (!Count || Count > 1024) return EFI_UNSUPPORTED;

    UINT32 Channel = MAX_UINT32;
    for (UINT32 I = 0; I < Count; ++I) {
        UINT32 Ppid = (ReadReg(ELDEN_SPMI_CORE_PHYS + 0x2000 + I * 4) >> 8) & 0xfff;
        if (Ppid != PTN_RESET_PPID || (ReadReg(ELDEN_SPMI_CONFIG_PHYS + I * 4) & 7)) continue;
        if (Channel != MAX_UINT32) return EFI_UNSUPPORTED;
        Channel = I;
    }
    if (Channel == MAX_UINT32) return EFI_NOT_FOUND;
    UINT64 ObsPage = ELDEN_SPMI_OBSERVER_PHYS + (Channel * 0x20u & ~(UINT64)EFI_PAGE_MASK);
    *Observer = ELDEN_SPMI_OBSERVER_PHYS + Channel * 0x20u;
    *Writer = SPMI_CHANNELS_PHYS + Channel * EFI_PAGE_SIZE;
    if (!IdentityMapped(ObsPage, EFI_PAGE_SIZE, TRUE) ||
        !IdentityMapped(*Writer, EFI_PAGE_SIZE, TRUE)) return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}

STATIC BOOLEAN SpmiGpioIo(UINT64 Channel, UINT8 Reg, UINT8 *Value, BOOLEAN Write)
{
    if (Write) WriteReg(Channel + 0x10, *Value);
    WriteReg(Channel, (Write ? 0 : BIT27) | ((UINT32)Reg << 4));
    for (UINTN Retry = 0; Retry < 1000; ++Retry) {
        UINT32 Status = ReadReg(Channel + 0x08);
        if (Status & 0x0e) return FALSE;
        if (Status & 1) {
            if (!Write) *Value = (UINT8)ReadReg(Channel + 0x18);
            return TRUE;
        }
        gBS->Stall(1);
    }
    return FALSE;
}

STATIC EFI_STATUS ReleaseRepeaterReset(VOID)
{
    UINT64 Observer, Writer;
    mRepeaterStep = "reset-channel";
    EFI_STATUS Status = ResetChannels(&Observer, &Writer);
    if (EFI_ERROR(Status)) return Status;

    // PM8550VE GPIO 6 as observed under Linux. Drive high to release reset.
    STATIC CONST UINT8 Registers[] = {0x40, 0x41, 0x42, 0x44, 0x45, 0x46};
    STATIC CONST UINT8 Config[] = {0x02, 0x01, 0x05, 0x80, 0x02, 0x80};
    UINT8 Old[ARRAY_SIZE(Registers)], Type = 0, Subtype = 0;
    EFI_TPL Tpl = gBS->RaiseTPL(TPL_NOTIFY);
    mRepeaterStep = "reset-gpio-id";
    Status = EFI_UNSUPPORTED;
    if (!SpmiGpioIo(Observer, 0x04, &Type, FALSE) || Type != 0x10 ||
        !SpmiGpioIo(Observer, 0x05, &Subtype, FALSE) || Subtype != 0x13) goto Done;
    Status = EFI_DEVICE_ERROR;
    mRepeaterStep = "reset-gpio-config";
    for (UINTN I = 0; I < ARRAY_SIZE(Registers); ++I)
        if (!SpmiGpioIo(Observer, Registers[I], &Old[I], FALSE)) goto Done;
    UINT8 Disabled = Old[5] & ~BIT7;
    if (!SpmiGpioIo(Writer, Registers[5], &Disabled, TRUE)) goto Done;
    for (UINTN I = 0; I < ARRAY_SIZE(Registers); ++I) {
        UINT8 Value = Config[I];
        if (!SpmiGpioIo(Writer, Registers[I], &Value, TRUE)) goto Restore;
        if (!SpmiGpioIo(Observer, Registers[I], &Value, FALSE) || Value != Config[I]) goto Restore;
    }
    Status = EFI_SUCCESS;
    goto Done;
Restore:
    for (UINTN I = 0; I < ARRAY_SIZE(Registers); ++I)
        SpmiGpioIo(Writer, Registers[I], &Old[I], TRUE);
Done:
    gBS->RestoreTPL(Tpl);
    return Status;
}

STATIC EFI_STATUS PrepareRepeater(VOID)
{
    QCOM_I2C *I2c;
    UINT32 CloseStatus;
    mRepeaterStep = "i2c-protocol";
    EFI_STATUS Status = gBS->LocateProtocol(&mQcomI2cGuid, NULL, (VOID **)&I2c);
    if (EFI_ERROR(Status)) return Status;
    if (I2c->Revision != 0x10000 || !I2c->Open || !I2c->Read || !I2c->Write || !I2c->Close)
        return EFI_UNSUPPORTED;
    VOID *Handle = NULL;
    mRepeaterStep = "i2c-open";
    mRepeaterI2cStatus = I2c->Open(PTN_I2C_INSTANCE, &Handle);
    if (mRepeaterI2cStatus != 0 || Handle == NULL) return EFI_NOT_READY;
    mRepeaterStep = "i2c-engine";
    Status = EFI_UNSUPPORTED;
    if (!RepeaterBus(Handle)) goto Close;

    UINT8 Id[3];
    if (!RepeaterIo(I2c->Read, Handle, 0x14, Id, sizeof(Id))) {
        Status = ReleaseRepeaterReset();
        if (EFI_ERROR(Status)) goto Close;
        gBS->Stall(2000);
        mRepeaterStep = "i2c-id-read";
        Status = EFI_NO_RESPONSE;
        if (!RepeaterIo(I2c->Read, Handle, 0x14, Id, sizeof(Id))) goto Close;
    }
    mRepeaterStep = "i2c-id";
    mRepeaterId = ((UINT32)Id[0] << 16) | ((UINT32)Id[1] << 8) | Id[2];
    Status = EFI_UNSUPPORTED;
    if (Id[0] != 0x22 || Id[1] != 0x32 || Id[2] != 0x11) goto Close;

    // Follow the firmware's live mux. Linux applies these overrides only to C1.
    Status = EFI_SUCCESS;
    mRepeaterStep = "ready";
    if (IdentityMapped(TLMM_GPIO29_PAGE, EFI_PAGE_SIZE, FALSE) &&
        (ReadReg(TLMM_GPIO29_PAGE) & (BIT9 | 0x3c)) == BIT9 &&
        !(ReadReg(TLMM_GPIO29_PAGE + 4) & BIT1)) {
        STATIC CONST UINT8 Tuning[][2] = PTN_LONG_EDGE_TUNING;
        for (UINTN I = 0; I < ARRAY_SIZE(Tuning); ++I) {
            UINT8 Value = Tuning[I][1];
            if (!RepeaterIo(I2c->Write, Handle, Tuning[I][0], &Value, 1) ||
                !RepeaterIo(I2c->Read, Handle, Tuning[I][0], &Value, 1) || Value != Tuning[I][1]) {
                mRepeaterStep = "tuning";
                Status = EFI_DEVICE_ERROR;
                break;
            }
        }
    }
Close:
    CloseStatus = I2c->Close(Handle);
    if (CloseStatus != 0 && !EFI_ERROR(Status)) {
        mRepeaterStep = "i2c-close";
        mRepeaterI2cStatus = CloseStatus;
        Status = EFI_DEVICE_ERROR;
    }
    return Status;
}
