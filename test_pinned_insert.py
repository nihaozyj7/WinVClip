# -*- coding: utf-8 -*-
"""验证临时固定态：连点多个表情，面板不消失、剪贴板不动、都能插进去。"""
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


def get_clip():
    for _ in range(20):
        if u.OpenClipboard(None):
            break
        time.sleep(0.05)
    try:
        h = u.GetClipboardData(13)
        if not h:
            return ""
        p = k.GlobalLock(h)
        s = ctypes.wstring_at(p)
        k.GlobalUnlock(h)
        return s
    finally:
        u.CloseClipboard()


def edit_text(np):
    ed = u.FindWindowExW(np, None, "Edit", None)
    if not ed:
        return "<no edit>"
    n = u.SendMessageW(ed, 0x000E, 0, 0) or 0
    buf = ctypes.create_unicode_buffer(n + 2)
    u.SendMessageW(ed, 0x000D, n + 1, ctypes.addressof(buf))
    return buf.value


def force_fg(hwnd):
    """跨进程 SetForegroundWindow 经常静默失败（还带前台锁），这里强制并校验。"""
    if u.GetForegroundWindow() == hwnd:
        return True
    fg = u.GetForegroundWindow()
    tid = u.GetWindowThreadProcessId(fg, None)
    my = k.GetCurrentThreadId()
    u.AttachThreadInput(my, tid, True)
    u.BringWindowToTop(hwnd)
    u.SetForegroundWindow(hwnd)
    u.SetActiveWindow(hwnd)
    u.SetFocus(hwnd)
    u.AttachThreadInput(my, tid, False)
    for _ in range(8):
        if u.GetForegroundWindow() == hwnd:
            return True
        key(0x12); key(0x12, True)
        u.SetForegroundWindow(hwnd)
        time.sleep(0.05)
    return u.GetForegroundWindow() == hwnd


def main():
    proc = subprocess.Popen(["notepad.exe"])
    time.sleep(1.6)
    np = u.FindWindowW(None, "无标题 - 记事本") or u.FindWindowW(None, "Untitled - Notepad")
    if not np:
        raise SystemExit("notepad not found")
    if not force_fg(np):
        raise SystemExit("无法把记事本切到前台，测试不可信")
    time.sleep(0.3)

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

    set_clip("SENTINEL-123")
    time.sleep(0.4)

    # 固定（图钉在右上一排的左边那个：关闭贴右缘，向左依次是齿轮、图钉）
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    W = r.right - r.left
    click(h, W - 75, 17)
    time.sleep(0.4)
    print("pinned, visible:", vis(h))

    click(h, 235, 17)                 # 表情页
    time.sleep(0.4)
    for ch in b"cat":
        u.PostMessageW(h, 0x0102, ch, 0)
        time.sleep(0.12)
    time.sleep(0.5)

    for x in (34, 83, 132):
        click(h, x, 170)
        time.sleep(0.8)

    print("visible after 3 clicks:", vis(h))
    print("clip:", repr(get_clip()))
    print("notepad text:", repr(edit_text(np)))

    # 取消固定，再点一次应关闭面板
    click(h, W - 75, 17)
    time.sleep(0.3)
    print("unpinned, visible:", vis(h))
    proc.terminate()


main()
