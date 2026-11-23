"""LZMA1 compression matching the T50 firmware (dict=8192, lc=3, lp=0, pb=2)."""

from __future__ import annotations

import lzma

from t50.buffer import PRINT_BUF_SIZE


def compress_lzma(data: bytes) -> bytes:
    filters = [
        {
            "id": lzma.FILTER_LZMA1,
            "dict_size": 8192,
            "lc": 3,
            "lp": 0,
            "pb": 2,
            "nice_len": 128,
        }
    ]
    compressed = bytearray(lzma.compress(data, format=lzma.FORMAT_ALONE, filters=filters))
    if len(compressed) < 13:
        raise RuntimeError("LZMA compressor returned a truncated stream")
    compressed[5:13] = len(data).to_bytes(8, "little")
    return bytes(compressed)


def compress_buffers(buffers: list[bytearray] | list[bytes]) -> tuple[bytes, int]:
    if not buffers:
        raise ValueError("no buffers to compress")
    concat = b"".join(bytes(b) for b in buffers)
    compressed = compress_lzma(concat)
    avg = len(compressed) // len(buffers)
    return compressed, avg


def calc_speed(compressed_size: int) -> int:
    if compressed_size > 3000:
        return 10
    if compressed_size > 2800:
        return 15
    if compressed_size > 2500:
        return 20
    if compressed_size > 2000:
        return 25
    if compressed_size > 1500:
        return 40
    if compressed_size > 1000:
        return 45
    if compressed_size > 500:
        return 55
    return 60
