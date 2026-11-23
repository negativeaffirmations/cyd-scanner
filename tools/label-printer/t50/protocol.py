"""USB HID command framing for the SUPVAN T50 family.

Wire format reverse-engineered by heeen/supvan-cups (MIT) from the
Katasymbol Android app. USB uses 0xC0/0x40 framing, big-endian params,
and 64-byte HID reports.
"""

from __future__ import annotations

import time
from dataclasses import dataclass

from t50.hidwin import HidDevice

CMD_BUF_FULL = 0x10
CMD_INQUIRY_STA = 0x11
CMD_CHECK_DEVICE = 0x12
CMD_START_PRINT = 0x13
CMD_STOP_PRINT = 0x14
CMD_PAPER_SKIP = 0x2E
CMD_RETURN_MAT = 0x30
CMD_NEXT_ZIPPEDBULK = 0x5C


def make_cmd(cmd: int, param: int = 0) -> bytes:
    param &= 0xFFFF
    return bytes(
        [
            0xC0,
            0x40,
            (param >> 8) & 0xFF,
            param & 0xFF,
            cmd & 0xFF,
            0x00,
            0x08,
            0x00,
        ]
    )


def make_cmd_two(cmd: int, param1: int, param2: int) -> bytes:
    p1, p2 = param1 & 0xFFFF, param2 & 0xFFFF
    return bytes(
        [
            0xC0,
            0x40,
            (p1 >> 8) & 0xFF,
            p1 & 0xFF,
            cmd & 0xFF,
            0x00,
            0x08,
            0x00,
            (p2 >> 8) & 0xFF,
            p2 & 0xFF,
        ]
    )


@dataclass
class Status:
    buf_full: bool = False
    label_rw_error: bool = False
    label_end: bool = False
    label_mode_error: bool = False
    ribbon_rw_error: bool = False
    ribbon_end: bool = False
    low_battery: bool = False
    device_busy: bool = False
    head_temp_high: bool = False
    cover_open: bool = False
    insert_usb: bool = False
    printing: bool = False
    label_not_installed: bool = False
    print_count: int = 0

    def has_error(self) -> bool:
        return any(
            (
                self.label_rw_error,
                self.label_end,
                self.label_mode_error,
                self.ribbon_rw_error,
                self.ribbon_end,
                self.cover_open,
                self.label_not_installed,
            )
        )

    def error_keys(self) -> list[str]:
        keys: list[str] = []
        if self.label_not_installed:
            keys.append("err_no_tape")
        if self.label_end:
            keys.append("err_tape_end")
        if self.label_rw_error:
            keys.append("err_tape")
        if self.cover_open:
            keys.append("err_cover")
        if self.label_mode_error:
            keys.append("err_tape_mode")
        if self.ribbon_end:
            keys.append("err_ribbon_end")
        if self.ribbon_rw_error:
            keys.append("err_ribbon")
        if self.head_temp_high:
            keys.append("err_hot")
        if self.low_battery:
            keys.append("err_battery")
        return keys

    def error_description(self) -> str:
        bits = []
        if self.label_not_installed:
            bits.append("no tape")
        if self.label_end:
            bits.append("tape end")
        if self.label_rw_error:
            bits.append("tape error")
        if self.cover_open:
            bits.append("cover open")
        if self.label_mode_error:
            bits.append("tape mode error")
        if self.ribbon_end:
            bits.append("ribbon end")
        if self.ribbon_rw_error:
            bits.append("ribbon error")
        if self.head_temp_high:
            bits.append("head too hot")
        if self.low_battery:
            bits.append("low battery")
        return ", ".join(bits) or "ok"

    def __str__(self) -> str:
        if self.has_error():
            return f"error({self.error_description()})"
        return (
            f"ok(printing={self.printing} busy={self.device_busy} "
            f"buf_full={self.buf_full} count={self.print_count})"
        )


@dataclass
class Material:
    width_mm: int = 0
    height_mm: int = 0
    gap_mm: int = 0
    label_type: int = 0
    sn: int = 0
    device_sn: str = ""

    def __str__(self) -> str:
        h = self.height_mm or "continuous"
        return (
            f"{self.width_mm}x{h} mm gap={self.gap_mm} "
            f"type={self.label_type} sn={self.device_sn or '-'}"
        )


def parse_status(resp: bytes) -> Status:
    if len(resp) < 7:
        raise ValueError(f"status response too short: {len(resp)} bytes")
    b0, b1, b2, b3 = resp[1], resp[2], resp[3], resp[4]
    return Status(
        buf_full=bool(b0 & 0x01),
        label_rw_error=bool(b0 & 0x02),
        label_end=bool(b0 & 0x04),
        label_mode_error=bool(b0 & 0x08),
        ribbon_rw_error=bool(b0 & 0x10),
        ribbon_end=bool(b0 & 0x20),
        low_battery=bool(b0 & 0x40),
        device_busy=bool(b1 & 0x04),
        head_temp_high=bool(b1 & 0x08),
        cover_open=bool(b2 & 0x08),
        insert_usb=bool(b2 & 0x10),
        printing=bool(b2 & 0x40),
        label_not_installed=bool(b3 & 0x01),
        print_count=resp[5] | (resp[6] << 8),
    )


def parse_material(resp: bytes) -> Material:
    if len(resp) < 23:
        raise ValueError(f"material response too short: {len(resp)} bytes")
    device_sn = ""
    if len(resp) > 40:
        end = 40
        while end < len(resp) and resp[end] != 0:
            end += 1
        device_sn = resp[40:end].decode("ascii", "replace")
    return Material(
        width_mm=resp[19],
        height_mm=resp[20],
        gap_mm=resp[21],
        label_type=resp[22] if len(resp) > 22 else 0,
        sn=(resp[31] | (resp[32] << 8)) if len(resp) > 32 else 0,
        device_sn=device_sn,
    )


class Transport:
    def __init__(self, dev: HidDevice):
        self.dev = dev

    def send_cmd(self, cmd: int, param: int = 0) -> bytes:
        self.dev.write(make_cmd(cmd, param))
        return self.dev.read(2.0)

    def send_cmd_two(self, cmd: int, param1: int, param2: int) -> bytes:
        self.dev.write(make_cmd_two(cmd, param1, param2))
        return self.dev.read(2.0)

    def send_bulk(self, data: bytes) -> None:
        """Write compressed bytes as 64-byte HID reports. Do not wait for an
        ack after the last chunk — the printer acks via BUF_FULL.
        """
        for i in range(0, len(data), 64):
            chunk = data[i : i + 64]
            self.dev.write(chunk)
            if i + 64 < len(data):
                time.sleep(0.001)
