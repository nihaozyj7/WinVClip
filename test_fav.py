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


def get_clip():
    u.OpenClipboard(None)
    h = u.GetClipboardData(13)
    if not h:
        u.CloseClipboard()
        return ""
    p = k.GlobalLock(h)
    s = ctypes.wstring_at(p)
    k.GlobalUnlock(h)
    u.CloseClipboard()
    return s


def find_notepad():
    """Win11 记事本标题会带 '*'（有改动），不能按标题精确匹配。"""
    res = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, ctypes.c_void_p, wintypes.LPARAM)
    def cb(h, l):
        b = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, b, 64)
        if b.value == "Notepad" and u.IsWindowVisible(h):
            res.append(h)
        return True

    u.EnumWindows(cb, 0)
    return res[0] if res else None


def force_fg(hwnd):
    """跨进程 SetForegroundWindow 常被前台锁静默拒绝。"""
    if u.GetForegroundWindow() == hwnd:
        return True
    fg = u.GetForegroundWindow()
    t1 = u.GetWindowThreadProcessId(fg, None)
    t2 = u.GetWindowThreadProcessId(hwnd, None)
    my = k.GetCurrentThreadId()
    for _ in range(6):
        u.AttachThreadInput(my, t1, True)
        u.AttachThreadInput(my, t2, True)
        u.BringWindowToTop(hwnd)
        u.SetForegroundWindow(hwnd)
        u.AttachThreadInput(my, t1, False)
        u.AttachThreadInput(my, t2, False)
        time.sleep(0.12)
        if u.GetForegroundWindow() == hwnd:
            return True
    return False


def combo(mod, vk):
    key(mod); time.sleep(0.04); key(vk); time.sleep(0.04)
    key(vk, True); time.sleep(0.04); key(mod, True); time.sleep(0.04)


def main():
    proc = subprocess.Popen(["notepad.exe"])
    time.sleep(1.6)
    np = find_notepad()
    if not np:
        raise SystemExit("notepad not found")
    force_fg(np); time.sleep(0.3)

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
    # Ctrl 组合在非激活态整段放行（面板不抢前台程序的 Ctrl+S / Ctrl+F，排在钩子链
    # 后面的别家全局热键也才收得到），所以收藏快捷键要先进输入态：
    # 点一下搜索框让面板拿到前台，Ctrl+D 走系统 WM_KEYDOWN 直达面板。
    click(h, 120, 60)             # 搜索框 → 输入态
    time.sleep(0.4)
    key(0x11)                     # Ctrl
    key(0x44)                     # D
    key(0x44, True)
    key(0x11, True)
    time.sleep(0.6)
    print("visible after ctrl+D:", vis(h))

    # 输入态里 Esc 直达面板 → 收起；再唤起，去收藏页取那条
    key(0x1B); time.sleep(0.4)
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
        raise SystemExit("panel did not show (2nd)")

    click(h, 175, 17)             # 收藏页
    time.sleep(0.6)
    click(h, 300, 170)            # 第一行
    time.sleep(1.0)

    print("visible after row click:", vis(h))
    # Win11 记事本是 WinUI3，没有 Edit 子窗口，读回文本只能走剪贴板
    if force_fg(np):
        time.sleep(0.3)
        combo(0x11, 0x41)             # Ctrl+A
        combo(0x11, 0x43)             # Ctrl+C
        time.sleep(0.5)
        print("notepad content:", repr(get_clip()))
    else:
        print("FAIL 记事本拿不到前台")
    proc.terminate()


main()
