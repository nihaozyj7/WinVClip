# -*- coding: utf-8 -*-
"""验证「点表情 = 直插目标窗口，剪贴板不动」。

流程：起记事本 → Win+V（非固定态）→ 切表情页 → 搜索 cat → 点第一格
检查：剪贴板仍是哨兵；记事本多了一个彩色 emoji。
"""
# --- 路径基准：从 tests/ 回到项目根（P0 结构整理时注入） ---
import os as _os, sys as _sys
_os.chdir(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import ctypes, subprocess, sys, time
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


def char(h, cp):
    u.PostMessageW(h, 0x0102, cp, 0)


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
    n = u.SendMessageW(ed, 0x000E, 0, 0)      # WM_GETTEXTLENGTH
    buf = ctypes.create_unicode_buffer(n + 2)
    u.SendMessageW(ed, 0x000D, n + 1, ctypes.addressof(buf))   # WM_GETTEXT
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
    time.sleep(0.3)
    print("panel visible:", vis(h))
    if not vis(h):
        proc.terminate()
        raise SystemExit("panel did not show")

    set_clip("SENTINEL-123")
    time.sleep(0.4)

    mode = sys.argv[1] if len(sys.argv) > 1 else "emo"
    click(h, 235 if mode == "emo" else 300, 17)
    time.sleep(0.5)
    if mode == "emo":
        for ch in b"cat":
            char(h, ch)
            time.sleep(0.12)
        time.sleep(0.5)
    print("visible after search:", vis(h))

    click(h, 40, 170)                 # 第一格
    time.sleep(0.9)

    print("visible after click:", vis(h))
    print("clip after click:", repr(get_clip()))
    print("notepad text:", repr(edit_text(np)))
    proc.terminate()


main()
