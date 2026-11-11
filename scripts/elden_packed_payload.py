"""Pack an Elden FAT volume into independent raw LZ4 blocks for DXE."""

import ctypes
import ctypes.util
import struct


MAGIC = b"ELDLZ401"
CHUNK_SIZE = 65536
MAX_VOLUME_SIZE = 256 * 1024 * 1024


def lz4():
    name = ctypes.util.find_library("lz4")
    if name is None:
        raise SystemExit("packed Elden images require liblz4")
    library = ctypes.CDLL(name)
    library.LZ4_compress_HC.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_int, ctypes.c_int, ctypes.c_int]
    library.LZ4_decompress_safe.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                           ctypes.c_int, ctypes.c_int]
    return library


def pack_volume(volume: bytes) -> bytes:
    if not 0 < len(volume) <= MAX_VOLUME_SIZE or len(volume) % 512:
        raise ValueError("expected a FAT volume of whole sectors within 256 MiB")
    library = lz4()
    scratch = ctypes.create_string_buffer(CHUNK_SIZE)
    data = bytearray()
    offsets = [0]
    for start in range(0, len(volume), CHUNK_SIZE):
        chunk = volume[start:start + CHUNK_SIZE]
        size = library.LZ4_compress_HC(chunk, scratch, len(chunk), len(chunk) - 1, 9)
        data.extend(scratch.raw[:size] if size else chunk)
        offsets.append(len(data))
    header = MAGIC + struct.pack(f"<{len(offsets) + 1}I", len(volume), *offsets)
    return (header + data).ljust((len(header) + len(data) + 511) // 512 * 512, b"\0")


def unpack_volume(packed: bytes) -> bytes:
    if len(packed) < 512 or len(packed) % 512 or packed[:8] != MAGIC:
        raise ValueError("invalid packed Elden payload header")
    volume_size, = struct.unpack_from("<I", packed, 8)
    if not 0 < volume_size <= MAX_VOLUME_SIZE or volume_size % 512:
        raise ValueError("invalid packed Elden volume size")
    chunks = (volume_size + CHUNK_SIZE - 1) // CHUNK_SIZE
    table_end = 12 + (chunks + 1) * 4
    if table_end >= len(packed):
        raise ValueError("truncated packed Elden chunk table")
    offsets = struct.unpack_from(f"<{chunks + 1}I", packed, 12)
    data_end = table_end + offsets[-1]
    if offsets[0] or not 0 <= len(packed) - data_end < 512 or any(packed[data_end:]):
        raise ValueError("invalid packed Elden data extent")
    library = lz4()
    scratch = ctypes.create_string_buffer(CHUNK_SIZE)
    volume = bytearray()
    for index, (start, end) in enumerate(zip(offsets, offsets[1:])):
        size = min(CHUNK_SIZE, volume_size - index * CHUNK_SIZE)
        if not 0 <= start < end <= offsets[-1] or end - start > size:
            raise ValueError(f"invalid packed Elden chunk {index}")
        chunk = packed[table_end + start:table_end + end]
        if len(chunk) != size:
            if library.LZ4_decompress_safe(chunk, scratch, len(chunk), size) != size:
                raise ValueError(f"invalid LZ4 data in packed Elden chunk {index}")
            chunk = scratch.raw[:size]
        volume.extend(chunk)
    return bytes(volume)
