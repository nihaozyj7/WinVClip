# -*- coding: utf-8 -*-
"""点一下目标窗口的客户区坐标，然后跑 uia_diag 打印焦点元素数据。

用法：python winvclip/tools/click_diag.py <hwnd> <cx> <cy> [root=winvclip 目录]
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
u.SetForegroundWindow.argtypes = [ctypes.c_void_p]


def to_screen(hwnd, x, y):
    pt = wintypes.POINT(int(x), int(y))
    u.ClientToScreen(ctypes.c_void_p(hwnd), ctypes.byref(pt))
    return pt.x, pt.y


def click(sx, sy):
    u.SetCursorPos(sx, sy)
    time.sleep(0.08)
    u.mouse_event(0x0002, 0, 0, 0, 0)   # LEFTDOWN
    time.sleep(0.05)
    u.mouse_event(0x0004, 0, 0, 0, 0)   # LEFTUP


def main():
    hwnd = int(sys.argv[1], 0)
    cx, cy = int(sys.argv[2]), int(sys.argv[3])
    root = sys.argv[4] if len(sys.argv) > 4 else os.path.join(os.path.dirname(__file__), "..")
    u.SetForegroundWindow(ctypes.c_void_p(hwnd))
    time.sleep(0.45)
    sx, sy = to_screen(hwnd, cx, cy)
    click(sx, sy)
    time.sleep(0.45)
    print("clicked screen=%d,%d" % (sx, sy))
    out = subprocess.run([os.path.join(root, "tools", "uia_diag.exe")],
                         capture_output=True, text=True)
    print(out.stdout, out.stderr)


main()
