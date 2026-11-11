_Head:
	/* Set _Entry address */
	adr		x1, _Payload

	/* Jump to the real code */
	b		_Start

	/* Fake Linux kernel header */

_StackBase:
	/* Text Offset */
#ifdef RAW_ARM64_IMAGE
	.quad 0
#else
	.quad UEFI_BASE
#endif

_StackSize:
	/* Image Size: firmware plus embedded FAT payload when present */
#ifdef RAW_ARM64_IMAGE
	.quad UEFI_SIZE + (EMBEDDED_PAYLOAD_SIZE) + (_Payload - _Head)
#elif defined(EMBEDDED_PAYLOAD_SIZE)
	.quad	UEFI_SIZE + (EMBEDDED_PAYLOAD_SIZE)
#else
	.quad	UEFI_SIZE
#endif

	/* Flags */
#ifdef RAW_ARM64_IMAGE
	.quad 0xa	/* 4 KiB pages, relocatable physical placement */
#else
	.quad 0
#endif

	/* Reserved */
	.quad 0

	/* Reserved */
	.quad 0

	/* Reserved */
	.quad 0

	/* ARM64 Magic */
	.ascii "ARM\x64"

	/* Reserved */
	.long 0

_Start:
	mov		x4, x1
#ifdef RAW_ARM64_IMAGE
	ldr		x5, _FirmwareBase
#else
	ldr		x5, _StackBase
#endif
	cmp		x4, x5
	beq		_Entry
#ifdef EMBEDDED_PAYLOAD_SIZE
	ldr		x6,  _CopySize
#else
	ldr		x6,  _StackSize
#endif

_CopyLoop:
	ldp		x2, x3, [x4], #16
	stp		x2, x3, [x5], #16
	subs	x6, x6, #16
	b.ne	_CopyLoop
#ifdef RAW_ARM64_IMAGE
	ldr		x5, _FirmwareBase
#else
	ldr		x5, _StackBase
#endif

_Entry:
#ifdef EMBEDDED_PAYLOAD_SIZE
	/* x1 still holds the decompressed image base; the payload follows the FD. */
	add		x2, x1, #UEFI_SIZE
	ldr		x3, =EMBEDDED_PAYLOAD_SIZE
#ifdef ENTRY_FDT_MAILBOX_OFFSET
	/* Record the live ABL FDT in the reserved RAM FAT slot; no pointer read. */
	str		x0, [x2, #ENTRY_FDT_MAILBOX_OFFSET]
#endif
#endif
	br		x5

#ifndef ENTRY_FDT_MAILBOX_OFFSET
_Dead:
	/* We should never get here */
	b		_Dead
#endif

#ifdef EMBEDDED_PAYLOAD_SIZE
.align 3
_CopySize:
	/* Only the firmware volume is copied; the payload stays in place. */
	.quad	UEFI_SIZE
#endif

#ifdef RAW_ARM64_IMAGE
_FirmwareBase:
	.quad UEFI_BASE
	.ltorg
#endif

.text
.align 4

_Payload:
	/* Your code will get ran right after this binary */
