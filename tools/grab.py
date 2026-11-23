# -*- coding: utf-8 -*-
"""弹面板 -> 切到指定页/分类 -> 全屏截图并按窗口 rect 裁剪保存。"""
import ctypes, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))   # PER_MONITOR_AWARE_V2
except Exception:
    u.SetProcessDPIAware()

from PIL import ImageGrab


class KBD(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class UN(ctypes.Union):
    _fields_ = [("ki", KBD), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", UN)]


def key(vk, up=False):
    i = INPUT()
    i.type = 1
    i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def winv():
    key(0x5B); time.sleep(0.05); key(0x56); time.sleep(0.05)
    key(0x56, True); time.sleep(0.05); key(0x5B, True)


def click(h, x, y):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0200, 0, lp)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


def wait_visible(timeout=6.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        h = u.FindWindowW("WinVClipPanelUI", None)
        if h and u.IsWindowVisible(h):
            return h
        time.sleep(0.05)
    return None


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "panel.png"
    clicks = []
    wheels = 0
    wait_after = 0.5
    for a in sys.argv[2:]:
        if a.startswith("w"):          # w20 = 滚 20 次后立刻截图
            wheels = int(a[1:])
        elif a.startswith("s"):        # s800 = 截图前多等 800ms
            wait_after = int(a[1:]) / 1000.0
        else:
            x, y = a.split(",")
            clicks.append((int(x), int(y)))
    h = wait_visible(0.3)
    for _ in range(5):
        if h:
            break
        winv()
        h = wait_visible(1.5)
    if not h:
        raise SystemExit("panel not visible")
    for (x, y) in clicks:
        click(h, x, y)
        time.sleep(0.35)
    for _ in range(wheels):
        pt = wintypes.POINT(300, 400)
        u.ClientToScreen(h, ctypes.byref(pt))
        lp = ((pt.y & 0xFFFF) << 16) | (pt.x & 0xFFFF)
        wp = ((0xFF88 & 0xFFFF) << 16) & 0xFFFFFFFF
        u.PostMessageW(h, 0x020A, wp, lp)
        time.sleep(0.02)
    time.sleep(wait_after)
    if not u.IsWindowVisible(h):
        raise SystemExit("panel hidden before grab")
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    img = ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom))
    img.save(out)
    print("saved", out, img.size)


main()
