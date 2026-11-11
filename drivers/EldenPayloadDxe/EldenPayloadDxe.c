/** @file
  Publish the FAT image appended after the UEFI firmware volume as a read-only
  block device.

  BootShim passes the payload base and size to the firmware entry point; PrePi
  stores both in an ELDEN_PAYLOAD_INFO HOB.  DiskIoDxe, PartitionDxe and FatDxe
  bind to this handle, so UniXNU can read \\EFI\\UniXNU\\unixnu.toml from the
  FAT volume even when the bootloader replaces the Android ramdisk field.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

#include "EldenPayloadLz4.h"

#define ELDEN_PAYLOAD_BLOCK_SIZE  512
#define ELDEN_FDT_MAILBOX_OFFSET  0x1c0

typedef struct {
  UINT64  Base;
  UINT64  Size;
  UINT64  EntryFdt;
} ELDEN_PAYLOAD_INFO;

//
// Must match patches/overlays/mu_wide_platforms/Platforms/AndromedaPkg/PrePi/PrePi.c.
//
STATIC CONST EFI_GUID  mEldenPayloadHobGuid = {
  0x5E1D9C3A, 0x7B24, 0x4F0E, { 0x9A, 0x6D, 0x2C, 0x8F, 0x1B, 0x4E, 0x7D, 0x10 }
};

STATIC CONST EFI_GUID  mEldenPayloadDeviceGuid = {
  0x9F3C2A11, 0x6D4B, 0x47E8, { 0xB2, 0x5A, 0x71, 0xC3, 0x0D, 0x8E, 0x44, 0x96 }
};

STATIC UINT8                 *mPayload;
STATIC UINTN                  mPayloadSize;
STATIC UINT64                 mEntryFdt;
STATIC CONST UINT32          *mChunkOffsets;
STATIC CONST UINT8           *mChunkData;
STATIC UINT8                 *mChunkBuffer;
STATIC UINTN                  mCachedChunk = MAX_UINTN;
STATIC EFI_BLOCK_IO_MEDIA     mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo;

//
// Screen checkpoint: a coloured band at the bottom of the panel tells the
// operator which layer failed without needing a photo of the log.
//   red    - driver ran but no payload HOB arrived (BootShim/PrePi handoff)
//   yellow - HOB arrived but the payload is not a valid FAT image
//   green  - payload validated and the BlockIo handle is installed
//
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mIndicatorGop;
STATIC EFI_EVENT                     mIndicatorTimer;
STATIC UINT32                        mIndicatorTicks;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mIndicatorColor = { 0x00, 0x00, 0xE0, 0x00 };

STATIC
VOID
EldenDrawIndicator (
  VOID
  )
{
  if ((mIndicatorGop == NULL) || (mIndicatorGop->Mode == NULL) ||
      (mIndicatorGop->Mode->Info == NULL))
  {
    return;
  }

  mIndicatorGop->Blt (
                    mIndicatorGop,
                    &mIndicatorColor,
                    EfiBltVideoFill,
                    0,
                    0,
                    0,
                    mIndicatorGop->Mode->Info->VerticalResolution - 160,
                    mIndicatorGop->Mode->Info->HorizontalResolution,
                    160,
                    0
                    );
}

STATIC
VOID
EFIAPI
EldenIndicatorTick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mIndicatorGop == NULL) {
    gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mIndicatorGop);
  }
  EldenDrawIndicator ();
  mIndicatorTicks++;
  if (mIndicatorTicks > 150) {
    gBS->CloseEvent (Event);
    mIndicatorTimer = NULL;
  }
}

STATIC
VOID
EldenSetIndicator (
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Color
  )
{
  mIndicatorColor = Color;
  if (mIndicatorGop == NULL) {
    gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mIndicatorGop);
  }
  EldenDrawIndicator ();
}

STATIC
VOID
EldenStartIndicator (
  VOID
  )
{
  mIndicatorGop = NULL;
  gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mIndicatorGop);
  EldenDrawIndicator ();

  if (gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  EldenIndicatorTick,
                  NULL,
                  &mIndicatorTimer
                  ) == EFI_SUCCESS)
  {
    gBS->SetTimer (mIndicatorTimer, TimerPeriodic, 2000000);
  }
}

STATIC
EFI_STATUS
EFIAPI
EldenBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                 ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EldenReadChunk (
  UINTN  Chunk
  )
{
  UINTN        Offset;
  UINTN        Size;
  UINTN        PackedSize;
  CONST UINT8  *Source;

  if (Chunk == mCachedChunk) {
    return TRUE;
  }
  mCachedChunk = MAX_UINTN;
  Offset = Chunk * ELDEN_PACKED_CHUNK;
  Size = MIN (ELDEN_PACKED_CHUNK, mPayloadSize - Offset);
  Source = mChunkData + mChunkOffsets[Chunk];
  PackedSize = mChunkOffsets[Chunk + 1] - mChunkOffsets[Chunk];
  if (PackedSize == Size) {
    CopyMem (mChunkBuffer, Source, Size);
  } else if (!EldenLz4Decode (Source, PackedSize, mChunkBuffer, Size)) {
    return FALSE;
  }
  mCachedChunk = Chunk;
  return TRUE;
}

STATIC
EFI_STATUS
EldenOpenPackedPayload (
  UINTN  StoredSize
  )
{
  UINTN  Chunks;
  UINTN  TableSize;
  UINTN  Chunk;
  UINTN  Size;

  // The magic is followed by a u32 logical size and an array of u32
  // offsets into the packed data, including its final end offset.
  mPayloadSize = ((CONST UINT32 *)mPayload)[2];
  if ((mPayloadSize == 0) || (mPayloadSize > ELDEN_PACKED_MAX_VOLUME) ||
      ((mPayloadSize % ELDEN_PAYLOAD_BLOCK_SIZE) != 0)) {
    return EFI_VOLUME_CORRUPTED;
  }
  Chunks = (mPayloadSize + ELDEN_PACKED_CHUNK - 1) / ELDEN_PACKED_CHUNK;
  TableSize = 12 + (Chunks + 1) * sizeof (UINT32);
  if (TableSize >= StoredSize) {
    return EFI_VOLUME_CORRUPTED;
  }
  mChunkOffsets = (CONST UINT32 *)(mPayload + 12);
  mChunkData = mPayload + TableSize;
  if ((mChunkOffsets[0] != 0) || (mChunkOffsets[Chunks] > StoredSize - TableSize)) {
    return EFI_VOLUME_CORRUPTED;
  }
  for (Chunk = 0; Chunk < Chunks; Chunk++) {
    Size = MIN (ELDEN_PACKED_CHUNK, mPayloadSize - Chunk * ELDEN_PACKED_CHUNK);
    if ((mChunkOffsets[Chunk + 1] <= mChunkOffsets[Chunk]) ||
        (mChunkOffsets[Chunk + 1] - mChunkOffsets[Chunk] > Size)) {
      return EFI_VOLUME_CORRUPTED;
    }
  }
  mChunkBuffer = AllocatePool (ELDEN_PACKED_CHUNK);
  if (mChunkBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  if (!EldenReadChunk (0)) {
    return EFI_VOLUME_CORRUPTED;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
EldenBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  UINT64  Offset;
  UINT64  EndOffset;
  UINT64  PayloadEnd;

  if ((Buffer == NULL) || (BufferSize == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if (MediaId != This->Media->MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if ((BufferSize % ELDEN_PAYLOAD_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (Lba > This->Media->LastBlock) {
    return EFI_INVALID_PARAMETER;
  }

  Offset = (UINT64)Lba * ELDEN_PAYLOAD_BLOCK_SIZE;
  PayloadEnd = (UINT64)mPayloadSize;
  if (Offset >= PayloadEnd) {
    return EFI_INVALID_PARAMETER;
  }
  EndOffset = Offset + BufferSize;
  if ((EndOffset < Offset) || (EndOffset > PayloadEnd)) {
    return EFI_INVALID_PARAMETER;
  }

  if (mChunkBuffer == NULL) {
    CopyMem (Buffer, mPayload + (UINTN)Offset, BufferSize);
  } else {
    UINT8  *Output = Buffer;

    while (BufferSize != 0) {
      UINTN  InChunk = (UINTN)Offset % ELDEN_PACKED_CHUNK;
      UINTN  Length = MIN (BufferSize, ELDEN_PACKED_CHUNK - InChunk);

      if (!EldenReadChunk ((UINTN)Offset / ELDEN_PACKED_CHUNK)) {
        return EFI_DEVICE_ERROR;
      }
      CopyMem (Output, mChunkBuffer + InChunk, Length);
      Output += Length;
      Offset += Length;
      BufferSize -= Length;
    }
  }

  // Expose the live ABL pointer in the reserved FAT mailbox. Leave the
  // original packed data and any mailbox already populated by BootShim intact.
  if (Lba == 0 && mEntryFdt != 0) {
    UINT8 *Mailbox = (UINT8 *)Buffer + ELDEN_FDT_MAILBOX_OFFSET;
    if (CompareMem (Mailbox, "ELDFDT01", 8) == 0 &&
        ReadUnaligned32 ((CONST UINT32 *)(Mailbox + 8)) == 1 &&
        ReadUnaligned32 ((CONST UINT32 *)(Mailbox + 12)) == 0 &&
        ReadUnaligned64 ((CONST UINT64 *)(Mailbox + 16)) == 0) {
      WriteUnaligned64 ((UINT64 *)(Mailbox + 16), mEntryFdt);
    }
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
EldenBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
EldenBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
EldenPayloadDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                  Status;
  EFI_HANDLE                  Handle;
  EFI_HOB_GUID_TYPE           *Hob;
  ELDEN_PAYLOAD_INFO          *Info;
  CONST UINT8                 *BootSector;
  MEDIA_RAM_DISK_DEVICE_PATH  *RamDiskPath;
  EFI_DEVICE_PATH_PROTOCOL    *EndPath;
  UINTN                       DevicePathSize;

  Handle = NULL;
  mPayload = NULL;
  mPayloadSize = 0;
  mEntryFdt = 0;

  EldenSetIndicator ((EFI_GRAPHICS_OUTPUT_BLT_PIXEL){ 0x00, 0x00, 0xE0, 0x00 });
  EldenStartIndicator ();

  Hob = GetFirstGuidHob (&mEldenPayloadHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) < OFFSET_OF (ELDEN_PAYLOAD_INFO, EntryFdt))) {
    DEBUG ((DEBUG_ERROR, "ELDEN-PAYLOAD: entry argument HOB is missing\n"));
    return EFI_NOT_FOUND;
  }

  Info = (ELDEN_PAYLOAD_INFO *)GET_GUID_HOB_DATA (Hob);
  EldenSetIndicator ((EFI_GRAPHICS_OUTPUT_BLT_PIXEL){ 0x00, 0xE0, 0xE0, 0x00 });
  if ((Info->Base == 0) ||
      (Info->Size == 0) ||
      ((Info->Size % ELDEN_PAYLOAD_BLOCK_SIZE) != 0))
  {
    DEBUG ((
      DEBUG_ERROR,
      "ELDEN-PAYLOAD: invalid payload 0x%lx size 0x%lx\n",
      Info->Base,
      Info->Size
      ));
    return EFI_LOAD_ERROR;
  }

  mPayload = (UINT8 *)(UINTN)Info->Base;
  mPayloadSize = (UINTN)Info->Size;
  if (GET_GUID_HOB_DATA_SIZE (Hob) >= sizeof (*Info) && (Info->EntryFdt & 3) == 0) {
    mEntryFdt = Info->EntryFdt;
  }
  BootSector = mPayload;
  if (CompareMem (mPayload, ELDEN_PACKED_MAGIC, 8) == 0) {
    Status = EldenOpenPackedPayload ((UINTN)Info->Size);
    if (EFI_ERROR (Status)) {
      goto Failed;
    }
    BootSector = mChunkBuffer;
  }
  if ((BootSector[510] != 0x55) || (BootSector[511] != 0xAA)) {
    DEBUG ((
      DEBUG_ERROR,
      "ELDEN-PAYLOAD: no FAT boot signature at 0x%lx\n",
      Info->Base
      ));
    Status = EFI_LOAD_ERROR;
    goto Failed;
  }

  ZeroMem (&mMedia, sizeof (mMedia));
  mMedia.MediaId = 0x454C4445;
  mMedia.RemovableMedia = TRUE;
  mMedia.MediaPresent = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly = TRUE;
  mMedia.BlockSize = ELDEN_PAYLOAD_BLOCK_SIZE;
  mMedia.LastBlock = (mPayloadSize / ELDEN_PAYLOAD_BLOCK_SIZE) - 1;
  mMedia.IoAlign = 0;

  ZeroMem (&mBlockIo, sizeof (mBlockIo));
  mBlockIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
  mBlockIo.Media = &mMedia;
  mBlockIo.Reset = EldenBlockReset;
  mBlockIo.ReadBlocks = EldenBlockRead;
  mBlockIo.WriteBlocks = EldenBlockWrite;
  mBlockIo.FlushBlocks = EldenBlockFlush;

  DevicePathSize = sizeof (MEDIA_RAM_DISK_DEVICE_PATH) + sizeof (EFI_DEVICE_PATH_PROTOCOL);
  RamDiskPath = AllocateZeroPool (DevicePathSize);
  if (RamDiskPath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Failed;
  }

  RamDiskPath->Header.Type = MEDIA_DEVICE_PATH;
  RamDiskPath->Header.SubType = MEDIA_RAM_DISK_DP;
  SetDevicePathNodeLength (RamDiskPath, sizeof (*RamDiskPath));
  RamDiskPath->StartingAddr[0] = (UINT32)(UINTN)mPayload;
  RamDiskPath->StartingAddr[1] = (UINT32)(((UINT64)(UINTN)mPayload) >> 32);
  RamDiskPath->EndingAddr[0] =
    (UINT32)(Info->Base + Info->Size - 1);
  RamDiskPath->EndingAddr[1] =
    (UINT32)((Info->Base + Info->Size - 1) >> 32);
  CopyMem (&RamDiskPath->TypeGuid, &mEldenPayloadDeviceGuid, sizeof (EFI_GUID));

  EndPath =
    (EFI_DEVICE_PATH_PROTOCOL *)(
      (UINT8 *)RamDiskPath + sizeof (MEDIA_RAM_DISK_DEVICE_PATH)
      );
  SetDevicePathEndNode (EndPath);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  &gEfiDevicePathProtocolGuid,
                  RamDiskPath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ELDEN-PAYLOAD: BlockIo install failed: %r\n", Status));
    FreePool (RamDiskPath);
    goto Failed;
  }

  DEBUG ((
    DEBUG_INFO,
    "ELDEN-PAYLOAD: OK at 0x%lx size 0x%lx blocks %lu\n",
    Info->Base,
    Info->Size,
    (UINT64)mMedia.LastBlock + 1
    ));
  EldenSetIndicator ((EFI_GRAPHICS_OUTPUT_BLT_PIXEL){ 0x00, 0xE0, 0x00, 0x00 });
  return EFI_SUCCESS;

Failed:
  if (mChunkBuffer != NULL) {
    FreePool (mChunkBuffer);
    mChunkBuffer = NULL;
  }
  return Status;
}
