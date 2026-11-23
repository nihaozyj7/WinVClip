# -*- coding: utf-8 -*-
"""性能场景驱动：切页签 + 连续滚轮 / 抽屉滚动，用于采集 perf 日志。"""
import sys, time, ctypes
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]


def panel():
    h = u.FindWindowW("WinVClipPanelUI", None)
    if not h:
        raise SystemExit("panel not found")
    return h


def click(hwnd, x, y):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(hwnd, 0x0200, 0, lp)
    u.PostMessageW(hwnd, 0x0201, 1, lp)
    u.PostMessageW(hwnd, 0x0202, 0, lp)


def wheel(hwnd, delta, x, y):
    pt = wintypes.POINT(x, y)
    u.ClientToScreen(hwnd, ctypes.byref(pt))
    lp = ((pt.y & 0xFFFF) << 16) | (pt.x & 0xFFFF)
    wp = ((delta & 0xFFFF) << 16) & 0xFFFFFFFF
    u.PostMessageW(hwnd, 0x020A, wp, lp)


def main():
    hwnd = panel()
    what = sys.argv[1] if len(sys.argv) > 1 else "sym"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    if what == "sym":
        click(hwnd, 313, 20)
    elif what == "emo":
        click(hwnd, 184, 20)
    elif what == "hist":
        click(hwnd, 100, 20)
    elif what == "cats":
        # 循环点分类 chip：测「切分类」的响应（每次点击 = 一次重排 + 整窗重绘）
        pts = [(400, 120), (350, 120), (300, 120), (440, 120)]
        for k in range(12):
            for (x, y) in pts:
                click(hwnd, x, y)
                time.sleep(0.02)
        time.sleep(0.3)
        print("done cats")
        return
    elif what == "drawer":
        click(hwnd, 531, 120)          # ⋯ 打开分类抽屉
        time.sleep(0.15)
        for i in range(n):
            wheel(hwnd, -120, 300, 300)
            if i % 4 == 0:
                time.sleep(0.005)
        time.sleep(0.3)
        print("done drawer", n)
        return
    time.sleep(0.15)
    for i in range(n):
        wheel(hwnd, -120, 300, 400)
        if i % 4 == 0:
            time.sleep(0.005)      # 让 WM_PAINT 有机会真的合成一帧
    time.sleep(0.3)
    print("done", what, n)


main()
