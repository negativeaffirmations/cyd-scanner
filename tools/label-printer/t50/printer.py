"""High-level T50 print jobs over USB HID."""

from __future__ import annotations

import logging
import threading
import time

from t50 import DEFAULT_DENSITY, DEFAULT_LABEL_HEIGHT_MM, DEFAULT_MARGIN_DOTS, PRINTHEAD_WIDTH_MM
from t50.bitmap import create_test_pattern, pack_page
from t50.buffer import split_into_buffers
from t50.compress import calc_speed, compress_buffers
from t50.hidwin import HidDevice, open_printer
from t50.protocol import (
    CMD_BUF_FULL,
    CMD_CHECK_DEVICE,
    CMD_INQUIRY_STA,
    CMD_NEXT_ZIPPEDBULK,
    CMD_RETURN_MAT,
    CMD_START_PRINT,
    CMD_STOP_PRINT,
    Material,
    Status,
    Transport,
    parse_material,
    parse_status,
)

log = logging.getLogger("t50")


class PrinterError(RuntimeError):
    pass


class Printer:
    def __init__(self, dev: HidDevice):
        self.dev = dev
        self.t = Transport(dev)
        self._io = threading.RLock()

    @classmethod
    def open(cls) -> "Printer":
        return cls(open_printer())

    def close(self) -> None:
        self.dev.close()

    def __enter__(self) -> "Printer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def check_device(self) -> bool:
        with self._io:
            resp = self.t.send_cmd(CMD_CHECK_DEVICE, 0)
            return bool(resp)

    def query_status(self) -> Status:
        with self._io:
            resp = self.t.send_cmd(CMD_INQUIRY_STA, 0)
            if not resp:
                raise PrinterError("no response to INQUIRY_STA")
            st = parse_status(resp)
            if st.has_error():
                log.info("status %s raw=%s", st, resp[:8].hex())
            return st

    def query_material(self) -> Material:
        with self._io:
            resp = self.t.send_cmd(CMD_RETURN_MAT, 0)
            if not resp:
                raise PrinterError("no response to RETURN_MAT")
            return parse_material(resp)

    def _wait_ready(self, attempts: int = 60) -> Status:
        for _ in range(attempts):
            st = self.query_status()
            if not st.device_busy and not st.printing:
                return st
            time.sleep(0.1)
        raise PrinterError("timeout waiting for printer ready")

    def _wait_printing(self, attempts: int = 60) -> Status:
        for _ in range(attempts):
            st = self.query_status()
            if st.has_error():
                raise PrinterError(f"printer error after START_PRINT: {st}")
            if st.printing:
                return st
            time.sleep(0.1)
        raise PrinterError("timeout waiting for printing station")

    def _wait_buffer(self, attempts: int = 200) -> Status:
        for _ in range(attempts):
            time.sleep(0.02)
            st = self.query_status()
            if st.has_error():
                raise PrinterError(f"printer error while waiting for buffer: {st}")
            if not st.buf_full:
                return st
        raise PrinterError("timeout waiting for buffer space")

    def transfer_compressed(self, compressed: bytes, speed: int) -> None:
        if len(compressed) > 0xFFFF:
            raise PrinterError(f"compressed job too large: {len(compressed)} bytes")
        comp_len = len(compressed)
        log.info("NEXT_ZIPPEDBULK len=%s speed=%s", comp_len, speed)
        resp = self.t.send_cmd(CMD_NEXT_ZIPPEDBULK, comp_len)
        if not resp:
            raise PrinterError("no response to NEXT_ZIPPEDBULK")
        self.t.send_bulk(compressed)
        time.sleep(0.02)
        self.t.send_cmd_two(CMD_BUF_FULL, comp_len, speed)

    def print_compressed(self, compressed: bytes, speed: int) -> None:
        with self._io:
            if not self.check_device():
                raise PrinterError("CHECK_DEVICE failed — is the printer on and USB connected?")
            st = self._wait_ready()
            if st.has_error():
                raise PrinterError(f"printer error before print: {st}")
            self.t.send_cmd(CMD_START_PRINT, 0)
            try:
                self._wait_printing()
                buf_st = self._wait_buffer()
                if buf_st.has_error():
                    raise PrinterError(f"printer error before transfer: {buf_st}")
                self.transfer_compressed(compressed, speed)
                for _ in range(300):
                    time.sleep(0.1)
                    done = self.query_status()
                    if not done.printing and not done.device_busy:
                        log.info("print complete")
                        return
                raise PrinterError("timeout waiting for print completion")
            except Exception:
                try:
                    self.t.send_cmd(CMD_STOP_PRINT, 0)
                except Exception:
                    pass
                raise

    def print_column_major(
        self,
        image_data: bytes,
        cols: int,
        bytes_per_line: int,
        *,
        density: int = DEFAULT_DENSITY,
        margin: int = DEFAULT_MARGIN_DOTS,
    ) -> None:
        buffers = split_into_buffers(image_data, bytes_per_line, cols, margin, margin, density)
        compressed, avg = compress_buffers(buffers)
        speed = calc_speed(avg)
        log.info(
            "job: %s buffers, compressed %s bytes, avg=%s, speed=%s",
            len(buffers),
            len(compressed),
            avg,
            speed,
        )
        self.print_compressed(compressed, speed)

    def print_page(
        self,
        row_major_msb: bytes,
        width: int,
        height: int,
        *,
        density: int = DEFAULT_DENSITY,
    ) -> None:
        col, cols, bpl = pack_page(row_major_msb, width, height)
        self.print_column_major(col, cols, bpl, density=density)

    def test_print(self, *, density: int = DEFAULT_DENSITY) -> Material:
        mat = self.query_material()
        width_mm = min(mat.width_mm or PRINTHEAD_WIDTH_MM, PRINTHEAD_WIDTH_MM)
        height_mm = mat.height_mm or DEFAULT_LABEL_HEIGHT_MM
        log.info("test print %s x %s mm density=%s", width_mm, height_mm, density)
        data, _w, h, bpl = create_test_pattern(width_mm, height_mm)
        self.print_column_major(data, h, bpl, density=density)
        return mat
