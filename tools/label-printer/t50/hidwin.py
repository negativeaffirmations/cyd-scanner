"""Windows HID transport for SUPVAN VID 0x1820 devices.

Uses SetupAPI + hid.dll. No extra native packages required.
"""

from __future__ import annotations

import ctypes
import time
from ctypes import wintypes
from dataclasses import dataclass

from t50 import HID_REPORT_SIZE, PID, VID

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10
ERROR_NO_MORE_ITEMS = 259
WAIT_TIMEOUT = 0x00000102
WAIT_OBJECT_0 = 0

hid = ctypes.WinDLL("hid")
setupapi = ctypes.WinDLL("setupapi")
kernel32 = ctypes.WinDLL("kernel32")


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", wintypes.BYTE * 8),
    ]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("InterfaceClassGuid", GUID),
        ("Flags", wintypes.DWORD),
        ("Reserved", ctypes.POINTER(ctypes.c_ulong)),
    ]


class SP_DEVICE_INTERFACE_DETAIL_DATA_W(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("DevicePath", wintypes.WCHAR * 1),
    ]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("Size", wintypes.ULONG),
        ("VendorID", wintypes.USHORT),
        ("ProductID", wintypes.USHORT),
        ("VersionNumber", wintypes.USHORT),
    ]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ("Usage", wintypes.USHORT),
        ("UsagePage", wintypes.USHORT),
        ("InputReportByteLength", wintypes.USHORT),
        ("OutputReportByteLength", wintypes.USHORT),
        ("FeatureReportByteLength", wintypes.USHORT),
        ("Reserved", wintypes.USHORT * 17),
        ("NumberLinkCollectionNodes", wintypes.USHORT),
        ("NumberInputButtonCaps", wintypes.USHORT),
        ("NumberInputValueCaps", wintypes.USHORT),
        ("NumberInputDataIndices", wintypes.USHORT),
        ("NumberOutputButtonCaps", wintypes.USHORT),
        ("NumberOutputValueCaps", wintypes.USHORT),
        ("NumberOutputDataIndices", wintypes.USHORT),
        ("NumberFeatureButtonCaps", wintypes.USHORT),
        ("NumberFeatureValueCaps", wintypes.USHORT),
        ("NumberFeatureDataIndices", wintypes.USHORT),
    ]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_ulonglong),
        ("InternalHigh", ctypes.c_ulonglong),
        ("Offset", wintypes.DWORD),
        ("OffsetHigh", wintypes.DWORD),
        ("hEvent", wintypes.HANDLE),
    ]


class SECURITY_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("nLength", wintypes.DWORD),
        ("lpSecurityDescriptor", wintypes.LPVOID),
        ("bInheritHandle", wintypes.BOOL),
    ]


HidD_GetHidGuid = hid.HidD_GetHidGuid
HidD_GetHidGuid.argtypes = [ctypes.POINTER(GUID)]
HidD_GetHidGuid.restype = None

HidD_GetAttributes = hid.HidD_GetAttributes
HidD_GetAttributes.argtypes = [wintypes.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
HidD_GetAttributes.restype = wintypes.BOOLEAN

HidD_GetPreparsedData = hid.HidD_GetPreparsedData
HidD_GetPreparsedData.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.LPVOID)]
HidD_GetPreparsedData.restype = wintypes.BOOLEAN

HidD_FreePreparsedData = hid.HidD_FreePreparsedData
HidD_FreePreparsedData.argtypes = [wintypes.LPVOID]
HidD_FreePreparsedData.restype = wintypes.BOOLEAN

HidP_GetCaps = hid.HidP_GetCaps
HidP_GetCaps.argtypes = [wintypes.LPVOID, ctypes.POINTER(HIDP_CAPS)]
HidP_GetCaps.restype = ctypes.c_long

HidD_SetOutputReport = hid.HidD_SetOutputReport
HidD_SetOutputReport.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.ULONG]
HidD_SetOutputReport.restype = wintypes.BOOLEAN

SetupDiGetClassDevsW = setupapi.SetupDiGetClassDevsW
SetupDiGetClassDevsW.argtypes = [
    ctypes.POINTER(GUID),
    wintypes.LPCWSTR,
    wintypes.HWND,
    wintypes.DWORD,
]
SetupDiGetClassDevsW.restype = wintypes.HANDLE

SetupDiEnumDeviceInterfaces = setupapi.SetupDiEnumDeviceInterfaces
SetupDiEnumDeviceInterfaces.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    ctypes.POINTER(GUID),
    wintypes.DWORD,
    ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
]
SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL

SetupDiGetDeviceInterfaceDetailW = setupapi.SetupDiGetDeviceInterfaceDetailW
SetupDiGetDeviceInterfaceDetailW.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
    wintypes.LPVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.LPVOID,
]
SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL

SetupDiDestroyDeviceInfoList = setupapi.SetupDiDestroyDeviceInfoList
SetupDiDestroyDeviceInfoList.argtypes = [wintypes.HANDLE]
SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL

CreateFileW = kernel32.CreateFileW
CreateFileW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.DWORD,
    wintypes.DWORD,
    ctypes.POINTER(SECURITY_ATTRIBUTES),
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.HANDLE,
]
CreateFileW.restype = wintypes.HANDLE

CloseHandle = kernel32.CloseHandle
CloseHandle.argtypes = [wintypes.HANDLE]
CloseHandle.restype = wintypes.BOOL

ReadFile = kernel32.ReadFile
ReadFile.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(OVERLAPPED),
]
ReadFile.restype = wintypes.BOOL

WriteFile = kernel32.WriteFile
WriteFile.argtypes = [
    wintypes.HANDLE,
    wintypes.LPCVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(OVERLAPPED),
]
WriteFile.restype = wintypes.BOOL

CreateEventW = kernel32.CreateEventW
CreateEventW.argtypes = [
    ctypes.POINTER(SECURITY_ATTRIBUTES),
    wintypes.BOOL,
    wintypes.BOOL,
    wintypes.LPCWSTR,
]
CreateEventW.restype = wintypes.HANDLE

WaitForSingleObject = kernel32.WaitForSingleObject
WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
WaitForSingleObject.restype = wintypes.DWORD

GetOverlappedResult = kernel32.GetOverlappedResult
GetOverlappedResult.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(OVERLAPPED),
    ctypes.POINTER(wintypes.DWORD),
    wintypes.BOOL,
]
GetOverlappedResult.restype = wintypes.BOOL

CancelIo = kernel32.CancelIo
CancelIo.argtypes = [wintypes.HANDLE]
CancelIo.restype = wintypes.BOOL

GetLastError = kernel32.GetLastError
GetLastError.restype = wintypes.DWORD

ERROR_IO_PENDING = 997


class HidError(RuntimeError):
    pass


@dataclass
class HidInfo:
    path: str
    vendor_id: int
    product_id: int
    version: int
    usage_page: int
    usage: int
    input_report_len: int
    output_report_len: int


def _hid_guid() -> GUID:
    guid = GUID()
    HidD_GetHidGuid(ctypes.byref(guid))
    return guid


def _open_path(path: str, overlapped: bool = True) -> int:
    flags = FILE_FLAG_OVERLAPPED if overlapped else 0
    handle = CreateFileW(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        None,
        OPEN_EXISTING,
        flags,
        None,
    )
    if handle == INVALID_HANDLE_VALUE or handle is None:
        raise HidError(f"CreateFile failed for {path} (err={GetLastError()})")
    return handle


def _caps(handle: int) -> HIDP_CAPS:
    preparsed = wintypes.LPVOID()
    if not HidD_GetPreparsedData(handle, ctypes.byref(preparsed)):
        raise HidError("HidD_GetPreparsedData failed")
    try:
        caps = HIDP_CAPS()
        status = HidP_GetCaps(preparsed, ctypes.byref(caps))
        if status != 0x00110000:  # HIDP_STATUS_SUCCESS
            raise HidError(f"HidP_GetCaps failed: 0x{status:08X}")
        return caps
    finally:
        HidD_FreePreparsedData(preparsed)


def list_devices(vendor_id: int = VID, product_id: int = PID) -> list[HidInfo]:
    guid = _hid_guid()
    devs = SetupDiGetClassDevsW(
        ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    )
    if devs == INVALID_HANDLE_VALUE:
        raise HidError("SetupDiGetClassDevs failed")
    found: list[HidInfo] = []
    try:
        index = 0
        while True:
            iface = SP_DEVICE_INTERFACE_DATA()
            iface.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
            if not SetupDiEnumDeviceInterfaces(
                devs, None, ctypes.byref(guid), index, ctypes.byref(iface)
            ):
                if GetLastError() == ERROR_NO_MORE_ITEMS:
                    break
                index += 1
                continue
            needed = wintypes.DWORD()
            SetupDiGetDeviceInterfaceDetailW(
                devs, ctypes.byref(iface), None, 0, ctypes.byref(needed), None
            )
            detail_buf = ctypes.create_string_buffer(needed.value)
            detail = ctypes.cast(detail_buf, ctypes.POINTER(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            detail.contents.cbSize = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
            if not SetupDiGetDeviceInterfaceDetailW(
                devs,
                ctypes.byref(iface),
                detail_buf,
                needed,
                None,
                None,
            ):
                index += 1
                continue
            path = ctypes.wstring_at(ctypes.addressof(detail.contents) + 4)
            handle = None
            try:
                handle = _open_path(path, overlapped=False)
                attrs = HIDD_ATTRIBUTES()
                attrs.Size = ctypes.sizeof(HIDD_ATTRIBUTES)
                if not HidD_GetAttributes(handle, ctypes.byref(attrs)):
                    index += 1
                    continue
                if attrs.VendorID != vendor_id or attrs.ProductID != product_id:
                    index += 1
                    continue
                caps = _caps(handle)
                found.append(
                    HidInfo(
                        path=path,
                        vendor_id=attrs.VendorID,
                        product_id=attrs.ProductID,
                        version=attrs.VersionNumber,
                        usage_page=caps.UsagePage,
                        usage=caps.Usage,
                        input_report_len=caps.InputReportByteLength,
                        output_report_len=caps.OutputReportByteLength,
                    )
                )
            except HidError:
                pass
            finally:
                if handle:
                    CloseHandle(handle)
            index += 1
    finally:
        SetupDiDestroyDeviceInfoList(devs)
    return found


class HidDevice:
    def __init__(self, info: HidInfo):
        self.info = info
        self.handle = _open_path(info.path, overlapped=True)
        self._has_report_id = info.output_report_len in (HID_REPORT_SIZE + 1, 65)
        self.out_len = info.output_report_len or (HID_REPORT_SIZE + (1 if self._has_report_id else 0))
        if self.out_len == 0:
            self.out_len = HID_REPORT_SIZE
        self.in_len = info.input_report_len or self.out_len
        if self.in_len == 0:
            self.in_len = HID_REPORT_SIZE

    def close(self) -> None:
        if getattr(self, "handle", None):
            CloseHandle(self.handle)
            self.handle = None

    def __enter__(self) -> "HidDevice":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _pack_out(self, payload: bytes) -> bytes:
        if self._has_report_id or self.out_len == HID_REPORT_SIZE + 1:
            buf = bytearray(self.out_len)
            buf[0] = 0
            data = payload[: self.out_len - 1]
            buf[1 : 1 + len(data)] = data
            return bytes(buf)
        buf = bytearray(self.out_len)
        data = payload[: self.out_len]
        buf[: len(data)] = data
        return bytes(buf)

    def _unpack_in(self, raw: bytes) -> bytes:
        if not raw:
            return b""
        if self.in_len == HID_REPORT_SIZE + 1 or raw[0] == 0 and len(raw) > HID_REPORT_SIZE:
            return raw[1:]
        return raw

    def write(self, payload: bytes) -> None:
        packet = self._pack_out(payload)
        ov = OVERLAPPED()
        ov.hEvent = CreateEventW(None, True, False, None)
        try:
            written = wintypes.DWORD()
            ok = WriteFile(
                self.handle,
                packet,
                len(packet),
                ctypes.byref(written),
                ctypes.byref(ov),
            )
            if not ok:
                err = GetLastError()
                if err != ERROR_IO_PENDING:
                    raise HidError(f"WriteFile failed (err={err})")
                if WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0:
                    CancelIo(self.handle)
                    raise HidError("HID write timed out")
                if not GetOverlappedResult(
                    self.handle, ctypes.byref(ov), ctypes.byref(written), False
                ):
                    raise HidError(f"HID write overlapped failed (err={GetLastError()})")
        finally:
            CloseHandle(ov.hEvent)

    def read(self, timeout_s: float = 2.0) -> bytes:
        buf = ctypes.create_string_buffer(self.in_len)
        ov = OVERLAPPED()
        ov.hEvent = CreateEventW(None, True, False, None)
        try:
            nread = wintypes.DWORD()
            ok = ReadFile(
                self.handle,
                buf,
                self.in_len,
                ctypes.byref(nread),
                ctypes.byref(ov),
            )
            if not ok:
                err = GetLastError()
                if err != ERROR_IO_PENDING:
                    raise HidError(f"ReadFile failed (err={err})")
                ms = max(1, int(timeout_s * 1000))
                wait = WaitForSingleObject(ov.hEvent, ms)
                if wait == WAIT_TIMEOUT:
                    CancelIo(self.handle)
                    # Finish the cancelled I/O so the next command's reply is
                    # not delivered to this overlapped read.
                    GetOverlappedResult(
                        self.handle, ctypes.byref(ov), ctypes.byref(nread), True
                    )
                    if nread.value:
                        return self._unpack_in(buf.raw[: nread.value])
                    return b""
                if wait != WAIT_OBJECT_0:
                    CancelIo(self.handle)
                    raise HidError(f"HID read wait failed ({wait})")
                if not GetOverlappedResult(
                    self.handle, ctypes.byref(ov), ctypes.byref(nread), False
                ):
                    raise HidError(f"HID read overlapped failed (err={GetLastError()})")
            return self._unpack_in(buf.raw[: nread.value])
        finally:
            CloseHandle(ov.hEvent)


def open_printer() -> HidDevice:
    devices = list_devices()
    if not devices:
        raise HidError(
            "No SUPVAN T50 HID device found (VID 1820 / PID 207F). "
            "Is the USB-C cable connected? Stop Supvan_T50_Service if it is running."
        )
    # Prefer the vendor-defined usage page interface over any sibling HID.
    devices.sort(key=lambda d: (0 if d.usage_page >= 0xFF00 else 1, -d.output_report_len))
    last_err: Exception | None = None
    for info in devices:
        try:
            return HidDevice(info)
        except HidError as exc:
            last_err = exc
    raise HidError(f"Could not open T50 HID device: {last_err}")
