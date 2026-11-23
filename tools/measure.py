# -*- coding: utf-8 -*-
"""一次跑完：必要时注入 Win+V 弹面板 -> 驱动 -> 打印 perf 日志。"""
import ctypes, os, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
u.ClientToScreen.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.POINT)]

LOG = os.path.join(os.environ.get("TEMP", "."), "winvclip_perf.log")


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


def send_winv():
    key(0x5B)
    time.sleep(0.05)
    key(0x56)
    time.sleep(0.05)
    key(0x56, True)
    time.sleep(0.05)
    key(0x5B, True)


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


def wait_panel(timeout=5.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        h = u.FindWindowW("WinVClipPanelUI", None)
        if h and u.IsWindowVisible(h):
            return h
        time.sleep(0.05)
    return None


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "emo"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    h = wait_panel(0.4)
    if not h:
        for _ in range(4):
            send_winv()
            h = wait_panel(1.2)
            if h:
                break
    if not h:
        raise SystemExit("panel not visible")
    if what == "emo":
        click(h, 184, 20)
    elif what == "sym":
        click(h, 313, 20)
    elif what == "hist":
        click(h, 100, 20)
    time.sleep(0.25)
    for i in range(n):
        wheel(h, -120, 300, 400)
        if i % 3 == 0:
            time.sleep(0.01)
    time.sleep(0.5)
    if os.path.exists(LOG):
        with open(LOG, "r", encoding="utf-8", errors="replace") as f:
            sys.stdout.write(f.read())
    else:
        print("no log")


main()
