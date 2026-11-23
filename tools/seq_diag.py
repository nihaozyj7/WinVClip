# -*- coding: utf-8 -*-
"""时序探测：重载页面 -> 立即点输入框 -> 连续多次跑 uia_diag，看首次/后续差异。

用法：python winvclip/tools/seq_diag.py <hwnd> <cx> <cy> [root=winvclip] [reload=1]
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
    u.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    u.mouse_event(0x0004, 0, 0, 0, 0)


def key(vk):
    u.keybd_event(vk, 0, 0, 0)
    time.sleep(0.03)
    u.keybd_event(vk, 0, 2, 0)


def diag(root, tag):
    out = subprocess.run([os.path.join(root, "tools", "uia_diag.exe")],
                         capture_output=True, text=True)
    lines = [l for l in (out.stdout or "").splitlines()
             if any(k in l for k in ("el ", "caretRange", "sel", "rfp", "pattern TextPattern",
                                     "el box"))]
    print("---- diag %s ----" % tag)
    print("\n".join(lines))


def main():
    hwnd = int(sys.argv[1], 0)
    cx, cy = int(sys.argv[2]), int(sys.argv[3])
    root = sys.argv[4] if len(sys.argv) > 4 else os.path.join(os.path.dirname(__file__), "..")
    reload = (len(sys.argv) <= 5) or sys.argv[5] != "0"

    u.SetForegroundWindow(ctypes.c_void_p(hwnd))
    time.sleep(0.4)
    sx, sy = to_screen(hwnd, cx, cy)
    if reload:
        key(0x74)             # F5
        time.sleep(1.4)
    click(sx, sy)
    time.sleep(0.25)
    print("clicked %d,%d reload=%d" % (sx, sy, reload))
    diag(root, "t+0.25s")
    time.sleep(0.4)
    diag(root, "t+0.65s")
    time.sleep(1.5)
    diag(root, "t+2.1s")
    # 光标下移两行后
    key(0x28); time.sleep(0.15); key(0x28)
    time.sleep(0.4)
    diag(root, "after 2x Down")


main()
