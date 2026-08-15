"""Development-only named-pipe smoke sender; it never opens or alters Armada II."""

from __future__ import annotations

import ctypes
import json
import sys
from ctypes import wintypes
from pathlib import Path

# This script lives two levels below the research root; the packaged client
# itself uses normal relative imports.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from apworld.star_trek_armada_ii.observer_protocol import OBSERVER_PIPE, STOCK_EXE_SHA256


PIPE_ACCESS_OUTBOUND = 0x00000002
PIPE_WAIT = 0
ERROR_PIPE_CONNECTED = 535
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                       wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID]
kernel32.CreateNamedPipeW.restype = wintypes.HANDLE
kernel32.ConnectNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID]
kernel32.ConnectNamedPipe.restype = wintypes.BOOL
kernel32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
                                ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
kernel32.WriteFile.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL


def main() -> int:
    pipe = kernel32.CreateNamedPipeW(OBSERVER_PIPE, PIPE_ACCESS_OUTBOUND, PIPE_WAIT, 1, 4096, 4096, 0, None)
    if pipe == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        connected = kernel32.ConnectNamedPipe(pipe, None)
        if not connected and ctypes.get_last_error() != ERROR_PIPE_CONNECTED:
            raise ctypes.WinError(ctypes.get_last_error())
        events = [
            {"type": "adapter_status", "adapter": "armada2_observer", "mode": "no_hook", "pinned": True},
            {"type": "objective_complete", "mission_module": "a2_fed01S.dsl", "objective_file": "a2_fed01_A.txt",
             "objective_index": 0, "executable_sha256": "invalid-smoke-hash"},
            {"type": "mission_result", "mission_module": "a2_fed02S.dsl", "result": "success",
             "executable_sha256": STOCK_EXE_SHA256},
        ]
        payload = "".join(json.dumps(event) + "\n" for event in events).encode("utf-8")
        written = wintypes.DWORD()
        if not kernel32.WriteFile(pipe, payload, len(payload), ctypes.byref(written), None):
            raise ctypes.WinError(ctypes.get_last_error())
        if written.value != len(payload):
            raise RuntimeError("partial named-pipe smoke write")
    finally:
        kernel32.CloseHandle(pipe)
    print("sent invalid and locked-mission observer smoke events")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
