"""Small, typed Win32 boundary. Handles must stay pointer-sized on Windows x64."""
from __future__ import annotations

import ctypes
from ctypes import wintypes as w
from functools import lru_cache
import sys


@lru_cache(maxsize=1)
def user32():
    if sys.platform != "win32":
        raise RuntimeError("This capture backend requires Windows 11.")
    dll = ctypes.WinDLL("user32", use_last_error=True)
    signatures = {
        "IsWindow": ([w.HWND], w.BOOL),
        "IsWindowVisible": ([w.HWND], w.BOOL),
        "IsIconic": ([w.HWND], w.BOOL),
        "GetClientRect": ([w.HWND, ctypes.POINTER(w.RECT)], w.BOOL),
        "ClientToScreen": ([w.HWND, ctypes.POINTER(w.POINT)], w.BOOL),
        "GetWindowRect": ([w.HWND, ctypes.POINTER(w.RECT)], w.BOOL),
        "GetWindowTextLengthW": ([w.HWND], ctypes.c_int),
        "GetWindowTextW": ([w.HWND, w.LPWSTR, ctypes.c_int], ctypes.c_int),
        "GetWindowThreadProcessId": ([w.HWND, ctypes.POINTER(w.DWORD)], w.DWORD),
        "GetDC": ([w.HWND], w.HDC),
        "ReleaseDC": ([w.HWND, w.HDC], ctypes.c_int),
        "PrintWindow": ([w.HWND, w.HDC, w.UINT], w.BOOL),
        "RegisterHotKey": ([w.HWND, ctypes.c_int, w.UINT, w.UINT], w.BOOL),
        "UnregisterHotKey": ([w.HWND, ctypes.c_int], w.BOOL),
        "PeekMessageW": ([ctypes.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT], w.BOOL),
        "SetWindowDisplayAffinity": ([w.HWND, w.DWORD], w.BOOL),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(dll, name)
        function.argtypes = arguments
        function.restype = result
    return dll


@lru_cache(maxsize=1)
def gdi32():
    user32()
    dll = ctypes.WinDLL("gdi32", use_last_error=True)
    signatures = {
        "CreateCompatibleDC": ([w.HDC], w.HDC),
        "DeleteDC": ([w.HDC], w.BOOL),
        "CreateCompatibleBitmap": ([w.HDC, ctypes.c_int, ctypes.c_int], w.HBITMAP),
        "SelectObject": ([w.HDC, w.HGDIOBJ], w.HGDIOBJ),
        "DeleteObject": ([w.HGDIOBJ], w.BOOL),
        "GetDIBits": ([w.HDC, w.HBITMAP, w.UINT, w.UINT, ctypes.c_void_p, ctypes.c_void_p, w.UINT], ctypes.c_int),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(dll, name)
        function.argtypes = arguments
        function.restype = result
    return dll


def window_cloaked(hwnd: int) -> bool:
    # A window on another virtual desktop can still have WS_VISIBLE set.
    dll = ctypes.WinDLL("dwmapi")
    function = dll.DwmGetWindowAttribute
    function.argtypes = [w.HWND, w.DWORD, ctypes.c_void_p, w.DWORD]
    function.restype = ctypes.c_long
    cloaked = w.DWORD()
    return function(hwnd, 14, ctypes.byref(cloaked), ctypes.sizeof(cloaked)) == 0 and bool(cloaked.value)


def exclude_from_capture(hwnd: int) -> bool:
    return bool(user32().SetWindowDisplayAffinity(hwnd, 0x11))
