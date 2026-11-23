"""1-bit raster packing for the T50 printhead.

Input is row-major MSB-first (CUPS / PNG / Windows). Output is
column-major LSB-first, then centered on the 384-dot head.
"""

from __future__ import annotations

from t50 import (
    DEFAULT_MARGIN_DOTS,
    DOTS_PER_MM,
    PRINTHEAD_BYTES_PER_LINE,
    PRINTHEAD_WIDTH_DOTS,
    PRINTHEAD_WIDTH_MM,
)

MAX_BUF_DATA = 4074


def raster_to_column_major(data: bytes, width: int, height: int) -> tuple[bytearray, int, int]:
    in_bpl = (width + 7) // 8
    out_bpl = (width + 7) // 8
    out_cols = height
    output = bytearray(out_cols * out_bpl)
    for y in range(height):
        for x in range(width):
            in_idx = y * in_bpl + (x // 8)
            if in_idx >= len(data):
                continue
            in_bit = 7 - (x % 8)
            if (data[in_idx] >> in_bit) & 1:
                out_idx = y * out_bpl + (x // 8)
                output[out_idx] |= 1 << (x % 8)
    return output, out_cols, out_bpl


def center_in_printhead(
    data: bytes, num_cols: int, input_width_dots: int, canvas_width_dots: int = PRINTHEAD_WIDTH_DOTS
) -> tuple[bytearray, int]:
    canvas_bpl = canvas_width_dots // 8
    input_bpl = (input_width_dots + 7) // 8
    if input_width_dots >= canvas_width_dots:
        output = bytearray(num_cols * canvas_bpl)
        copy_len = min(canvas_bpl, input_bpl)
        for col in range(num_cols):
            src = col * input_bpl
            dst = col * canvas_bpl
            if src + copy_len <= len(data):
                output[dst : dst + copy_len] = data[src : src + copy_len]
        return output, canvas_bpl

    x_off = (canvas_width_dots - input_width_dots) // 2
    output = bytearray(num_cols * canvas_bpl)
    for col in range(num_cols):
        for dot in range(input_width_dots):
            in_byte = col * input_bpl + (dot // 8)
            if in_byte >= len(data):
                continue
            if (data[in_byte] >> (dot % 8)) & 1:
                out_dot = x_off + dot
                out_byte = col * canvas_bpl + (out_dot // 8)
                if out_byte < len(output):
                    output[out_byte] |= 1 << (out_dot % 8)
    return output, canvas_bpl


def pack_page(row_major_msb: bytes, width: int, height: int) -> tuple[bytearray, int, int]:
    """Fit a page onto the 384-dot head and return column-major data."""
    col, cols, bpl = raster_to_column_major(row_major_msb, width, height)
    if width != PRINTHEAD_WIDTH_DOTS:
        col, bpl = center_in_printhead(col, cols, width, PRINTHEAD_WIDTH_DOTS)
    return col, cols, bpl


def create_test_pattern(label_width_mm: int, height_mm: int) -> tuple[bytearray, int, int, int]:
    canvas_w = PRINTHEAD_WIDTH_DOTS
    height_dots = height_mm * DOTS_PER_MM
    bpl = PRINTHEAD_BYTES_PER_LINE
    label_w = min(label_width_mm, PRINTHEAD_WIDTH_MM) * DOTS_PER_MM
    x_off = (canvas_w - label_w) // 2
    max_cols = MAX_BUF_DATA // bpl
    buf_regions = []
    col = DEFAULT_MARGIN_DOTS
    end_limit = max(height_dots - DEFAULT_MARGIN_DOTS, col + 1)
    while col < end_limit:
        end = min(col + max_cols, end_limit)
        buf_regions.append((col, end))
        col = end

    buf = bytearray(bpl * height_dots)
    for c in range(height_dots):
        for row in range(canvas_w):
            pixel = False
            lr = row - x_off
            if 0 <= lr < label_w:
                if lr < 2 or lr >= label_w - 2 or c < 2 or c >= height_dots - 2:
                    pixel = True
                for i, (bs, be) in enumerate(buf_regions):
                    if bs <= c < be:
                        bh = be - bs
                        bw = label_w
                        local = c - bs
                        if local < 2 or local >= bh - 2:
                            pixel = True
                        if bh:
                            expected = (local * bw) // bh
                            if abs(lr - expected) < 2:
                                pixel = True
                            expected2 = bw - 1 - expected
                            if abs(lr - expected2) < 2:
                                pixel = True
                        for d in range(i + 1):
                            dx = 10 + d * 12
                            dy = 10
                            if dx <= lr < dx + 8 and dy <= local < dy + 8:
                                pixel = True
                        break
            if pixel:
                idx = c * bpl + (row // 8)
                buf[idx] |= 1 << (row % 8)
    return buf, canvas_w, height_dots, bpl
