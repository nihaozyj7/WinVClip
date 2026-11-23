# -*- coding: utf-8 -*-
"""切分类压力测试：反复点分类 chip / 抽屉项，看有没有长帧。"""
import ctypes, os, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
LOG = os.path.join(os.environ.get("TEMP", "."), "winvclip_perf.log")


def click(hwnd, x, y):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(hwnd, 0x0200, 0, lp)
    u.PostMessageW(hwnd, 0x0201, 1, lp)
    u.PostMessageW(hwnd, 0x0202, 0, lp)


def wait_panel(timeout=5.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        h = u.FindWindowW("WinVClipPanelUI", None)
        if h and u.IsWindowVisible(h):
            return h
        time.sleep(0.05)
    return None


h = wait_panel()
if not h:
    raise SystemExit("panel not visible")
click(h, 235, 20)          # 表情页签
time.sleep(0.4)
pts = [(300, 120), (350, 120), (400, 120), (330, 120)]
for k in range(8):
    for (x, y) in pts:
        click(h, x, y)
        time.sleep(0.05)
time.sleep(0.6)
if os.path.exists(LOG):
    with open(LOG, "r", encoding="utf-8", errors="replace") as f:
        sys.stdout.write(f.read())
else:
    print("no log")
