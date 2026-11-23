# -*- coding: utf-8 -*-
"""收藏页回归：Ctrl+D 收藏一条 → 切到收藏页 → 点第一行，应粘贴到目标窗口。"""
import ctypes, subprocess, time
from ctypes import wintypes

u = ctypes.windll.user32
k = ctypes.windll.kernel32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.FindWindowExW.restype = ctypes.c_void_p
u.FindWindowExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.LPCWSTR, wintypes.LPCWSTR]
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
u.IsWindowVisible.argtypes = [ctypes.c_void_p]
u.SetForegroundWindow.argtypes = [ctypes.c_void_p]
u.OpenClipboard.argtypes = [ctypes.c_void_p]
u.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
u.GetClipboardData.restype = ctypes.c_void_p
u.GetClipboardData.argtypes = [wintypes.UINT]
k.GlobalAlloc.restype = ctypes.c_void_p
k.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
k.GlobalLock.restype = ctypes.c_void_p
k.GlobalLock.argtypes = [ctypes.c_void_p]
k.GlobalUnlock.argtypes = [ctypes.c_void_p]


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


def vis(h):
    return bool(u.IsWindowVisible(h))


def click(h, x, y):
    lp = ((y & 0xFFFF) << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0200, 0, lp)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


def set_clip(s):
    u.OpenClipboard(None)
    u.EmptyClipboard()
    data = (s + "\0").encode("utf-16-le")
    h = k.GlobalAlloc(0x0042, len(data))
    p = k.GlobalLock(h)
    ctypes.memmove(p, data, len(data))
    k.GlobalUnlock(h)
    u.SetClipboardData(13, h)
    u.CloseClipboard()


def edit_text(np):
    ed = u.FindWindowExW(np, None, "Edit", None)
    n = u.SendMessageW(ed, 0x000E, 0, 0) or 0
    buf = ctypes.create_unicode_buffer(n + 2)
    u.SendMessageW(ed, 0x000D, n + 1, ctypes.addressof(buf))
    return buf.value


def main():
    proc = subprocess.Popen(["notepad.exe"])
    time.sleep(1.6)
    np = u.FindWindowW(None, "无标题 - 记事本") or u.FindWindowW(None, "Untitled - Notepad")
    u.SetForegroundWindow(np); time.sleep(0.3)

    set_clip("FEED-TEXT-777")     # 让历史里有一条独特文本
    time.sleep(0.6)

    h = panel()
    if vis(h):
        winv(); time.sleep(0.4)
    for _ in range(6):
        winv()
        for _ in range(30):
            if vis(h):
                break
            time.sleep(0.05)
        if vis(h):
            break
    if not vis(h):
        proc.terminate()
        raise SystemExit("panel did not show")

    click(h, 60, 17)              # 历史页
    time.sleep(0.5)
    key(0x11)                     # Ctrl
    key(0x44)                     # D
    key(0x44, True)
    key(0x11, True)
    time.sleep(0.6)
    print("visible after ctrl+D:", vis(h))

    click(h, 175, 17)             # 收藏页
    time.sleep(0.6)
    click(h, 300, 170)            # 第一行
    time.sleep(1.0)

    print("visible after row click:", vis(h))
    print("notepad text:", repr(edit_text(np)))
    proc.terminate()


main()
