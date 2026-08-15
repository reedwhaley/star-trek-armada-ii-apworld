"""Send the harmless Armada II UI-thread probe through the observer pipe."""

from __future__ import annotations

import ctypes
import sys
from ctypes import wintypes


PIPE = r"\\.\pipe\archipelago_armada2_control_v1"
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                  wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
kernel32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
                                ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]


def main() -> int:
    handle = kernel32.CreateFileW(PIPE, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
    if handle == INVALID_HANDLE_VALUE:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        command = sys.argv[1] if len(sys.argv) > 1 else "probe_ui_thread"
        payload = (command.rstrip() + "\n").encode("ascii")
        written = wintypes.DWORD()
        if not kernel32.WriteFile(handle, payload, len(payload), ctypes.byref(written), None):
            raise ctypes.WinError(ctypes.get_last_error())
        reply = ctypes.create_string_buffer(128)
        received = wintypes.DWORD()
        if not kernel32.ReadFile(handle, reply, len(reply), ctypes.byref(received), None):
            raise ctypes.WinError(ctypes.get_last_error())
        print(reply.raw[:received.value].decode("ascii", errors="replace").strip())
    finally:
        kernel32.CloseHandle(handle)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
