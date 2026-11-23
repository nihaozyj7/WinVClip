# -*- coding: utf-8 -*-
"""测量一次「点击 → 界面处理完成」的耗时（同步 SendMessage，等于把消息处理时间算在内）。

用法：python measure_click.py X Y
"""
import ctypes, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.SendMessageTimeoutW.restype = ctypes.c_void_p
u.SendMessageTimeoutW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p,
                                  ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint,
                                  ctypes.POINTER(ctypes.c_void_p)]


def send(hwnd, msg, wp, lp):
    out = ctypes.c_void_p()
    u.SendMessageTimeoutW(hwnd, msg, wp, lp, 0x0002, 5000, ctypes.byref(out))  # SMTO_ABORTIFHUNG


def main():
    x, y = int(sys.argv[1]), int(sys.argv[2])
    hwnd = u.FindWindowW("WinVClipPanelUI", None)
    if not hwnd:
        raise SystemExit("panel not created")
    lp = (y << 16) | (x & 0xFFFF)
    t0 = time.perf_counter()
    send(hwnd, 0x0200, 0, lp)        # WM_MOUSEMOVE
    send(hwnd, 0x0201, 1, lp)        # WM_LBUTTONDOWN
    send(hwnd, 0x0202, 0, lp)        # WM_LBUTTONUP
    dt = (time.perf_counter() - t0) * 1000
    print("click (%d,%d) handled in %.1f ms" % (x, y, dt))


main()
