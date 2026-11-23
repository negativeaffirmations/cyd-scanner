"""4096-byte print-buffer framing used by the T50 firmware."""

from __future__ import annotations

MAX_BUF_DATA = 4074
PRINT_BUF_SIZE = 4096
PRINT_BUF_HEADER = 14
MARGIN_MAX_DOTS = 900
MAX_DENSITY = 15
CHECKSUM_STRIDE = 256


def build_page_reg_bits(
    *,
    page_st: bool = False,
    page_end: bool = False,
    prt_end: bool = False,
    cut: int = 0,
    savepaper: bool = False,
    first_cut: int = 0,
    nodu: int = 0,
    mat: int = 1,
) -> bytes:
    b0 = 0
    if page_st:
        b0 |= 0x02
    if page_end:
        b0 |= 0x04
    if prt_end:
        b0 |= 0x08
    b0 = (b0 & 0x0F) | ((cut & 0x07) << 4)
    if savepaper:
        b0 |= 0x80
    b1 = (first_cut & 0x03) | ((nodu & 0x0F) << 2) | ((mat & 0x03) << 6)
    return bytes((b0, b1))


def build_print_buffer(
    image_data: bytes,
    per_line_byte: int,
    cols_in_buf: int,
    *,
    page_st: bool,
    page_end: bool,
    prt_end: bool,
    margin_top: int,
    margin_bottom: int,
    density: int,
) -> bytearray:
    buf = bytearray(PRINT_BUF_SIZE)
    page_bits = build_page_reg_bits(
        page_st=page_st, page_end=page_end, prt_end=prt_end, nodu=density, mat=1
    )
    buf[2:4] = page_bits
    buf[4:6] = int(cols_in_buf).to_bytes(2, "little")
    buf[6] = per_line_byte & 0xFF
    mt = max(1, min(int(margin_top), MARGIN_MAX_DOTS))
    mb = max(1, min(int(margin_bottom), MARGIN_MAX_DOTS))
    buf[8:10] = mt.to_bytes(2, "little")
    buf[10:12] = mb.to_bytes(2, "little")
    buf[12] = min(int(density), MAX_DENSITY)
    data_len = min(len(image_data), PRINT_BUF_SIZE - PRINT_BUF_HEADER)
    buf[PRINT_BUF_HEADER : PRINT_BUF_HEADER + data_len] = image_data[:data_len]

    data_end = int(cols_in_buf) * int(per_line_byte) + PRINT_BUF_HEADER
    chk = sum(buf[2:14])
    n_strides = data_end // CHECKSUM_STRIDE
    for i in range(1, n_strides + 1):
        idx = i * CHECKSUM_STRIDE - 1
        if idx < len(buf):
            chk += buf[idx]
    buf[0:2] = (chk & 0xFFFF).to_bytes(2, "little")
    return buf


def split_into_buffers(
    image_data: bytes,
    per_line_byte: int,
    total_cols: int,
    margin_top: int,
    margin_bottom: int,
    density: int,
) -> list[bytearray]:
    max_cols = MAX_BUF_DATA // per_line_byte
    image_cols = total_cols - margin_top - margin_bottom
    if image_cols <= 0:
        raise ValueError("label too short for the configured margins")
    buffers: list[bytearray] = []
    cols_remaining = image_cols
    current_col = 0
    while cols_remaining > 0:
        cols_in_buf = min(cols_remaining, max_cols)
        is_first = current_col == 0
        is_last = cols_remaining <= max_cols
        img_start = (margin_top + current_col) * per_line_byte
        img_end = img_start + cols_in_buf * per_line_byte
        chunk = image_data[img_start : min(img_end, len(image_data))]
        buffers.append(
            build_print_buffer(
                chunk,
                per_line_byte,
                cols_in_buf,
                page_st=is_first,
                page_end=is_last,
                prt_end=is_last,
                margin_top=margin_top,
                margin_bottom=margin_bottom,
                density=density,
            )
        )
        current_col += cols_in_buf
        cols_remaining -= cols_in_buf
    return buffers
