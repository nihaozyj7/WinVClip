# -*- coding: utf-8 -*-
"""验证面板尺寸调整与记忆。

  python test_resize.py grow      # 拖右边缘 +120，左下角 -400（应停在最小尺寸）
  python test_resize.py state
  python test_resize.py reset     # 删掉记忆文件（下次启动回到默认尺寸）
"""
import ctypes, os, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
u.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.RECT)]
u.IsWindowVisible.argtypes = [ctypes.c_void_p]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class _I(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _I)]


def key(vk, up=False):
    i = INPUT()
    i.type = 1
    i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def winv():
    key(0x5B); time.sleep(0.05); key(0x56); time.sleep(0.05)
    key(0x56, True); time.sleep(0.05); key(0x5B, True)


def panel():
    for _ in range(60):
        h = u.FindWindowW("WinVClipPanelUI", None)
        if h:
            return h
        time.sleep(0.05)
    raise SystemExit("panel not created")


def rect(h):
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r


def post(h, msg, x, y, wp=0):
    lp = ((y & 0xFFFF) << 16) | (x & 0xFFFF)
    u.PostMessageW(h, msg, wp, lp)


def drag(h, x0, y0, x1, y1):
    post(h, 0x0200, x0, y0)          # move
    post(h, 0x0201, x0, y0, 1)       # down
    time.sleep(0.05)
    post(h, 0x0200, x1, y1)          # move（resize 在这一条里生效）
    time.sleep(0.05)
    post(h, 0x0202, x1, y1)          # up
    time.sleep(0.25)


def size_file():
    d = os.path.join(os.environ["LOCALAPPDATA"], "WinVClip", "view-size.txt")
    return d


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "state"
    h = panel()
    if not u.IsWindowVisible(h):
        winv(); time.sleep(0.6)

    if cmd == "state":
        r = rect(h)
        print("rect=(%d,%d) size=%dx%d" % (r.left, r.top, r.right - r.left, r.bottom - r.top))
        p = size_file()
        print("file:", open(p).read().strip() if os.path.exists(p) else "<none>")
        return

    if cmd == "reset":
        p = size_file()
        if os.path.exists(p):
            os.remove(p)
        print("removed", p)
        return

    if cmd == "minh":
        # 拖下边缘一路往上 → 应停在最小高度
        r = rect(h)
        W, H = r.right - r.left, r.bottom - r.top
        drag(h, W // 2, H - 2, W // 2, H - 2 - 500)
        r = rect(h)
        print("after shrink-h: %dx%d (expect height 340)" % (r.right - r.left, r.bottom - r.top))
        p = size_file()
        print("file:", open(p).read().strip() if os.path.exists(p) else "<none>")
        return

    r = rect(h)
    W, H = r.right - r.left, r.bottom - r.top
    print("before: %dx%d" % (W, H))

    # 1) 拖右边缘 +120
    drag(h, W - 2, H // 2, W - 2 + 120, H // 2)
    r = rect(h)
    W2, H2 = r.right - r.left, r.bottom - r.top
    print("after grow: %dx%d  (expect %dx%d)" % (W2, H2, W + 120, H))

    # 2) 拖右下角往外 60/40
    drag(h, W2 - 2, H2 - 2, W2 - 2 + 60, H2 - 2 + 40)
    r = rect(h)
    W3, H3 = r.right - r.left, r.bottom - r.top
    print("after corner: %dx%d  (expect %dx%d)" % (W3, H3, W2 + 60, H2 + 40))

    # 3) 反向拖右边缘到极小 → 应停在最小宽度 360
    drag(h, W3 - 2, H3 // 2, W3 - 2 - 600, H3 // 2)
    r = rect(h)
    print("after shrink: %dx%d  (expect 360 宽)" % (r.right - r.left, r.bottom - r.top))

    p = size_file()
    print("file:", open(p).read().strip() if os.path.exists(p) else "<none>")


main()
