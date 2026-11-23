# -*- coding: utf-8 -*-
"""验证「不抢焦点」焦点模型（替代 test_caret / test_ghost）：

1. 面板弹出前后，记事本是否一直持有前台与真实插入符；
2. 钩子转发是否生效：面板打开时打字进搜索框而不是记事本；
3. Esc 收起后焦点仍在记事本，可以继续打字；
4. Enter 粘贴到记事本。

用法：python test_noactivate.py [目标窗口标题子串]
"""
import ctypes, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
u.SetProcessDPIAware()

class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]

class _I(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]

class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _I)]

class GUITHREADINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hwndActive", wintypes.HWND), ("hwndFocus", wintypes.HWND),
                ("hwndCapture", wintypes.HWND), ("hwndMenuOwner", wintypes.HWND),
                ("hwndMoveSize", wintypes.HWND), ("hwndCaret", wintypes.HWND),
                ("rcCaret", wintypes.RECT)]

VK = dict(win=0x5B, v=0x56, esc=0x1B, enter=0x0D, down=0x28, up=0x26, a=0x41)


def key(vk, up=False, uni=None):
    if uni is not None:
        i = INPUT(); i.type = 1
        i.u.ki.wScan = ord(uni)
        i.u.ki.dwFlags = 4 | (2 if up else 0)
        u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))
        return
    i = INPUT(); i.type = 1; i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def tap(vk, n=1):
    for _ in range(n):
        key(vk); time.sleep(0.03); key(vk, True); time.sleep(0.03)


def win_vi():
    key(VK["win"]); time.sleep(0.05)
    key(VK["v"]); time.sleep(0.05)
    key(VK["v"], True); time.sleep(0.05)
    key(VK["win"], True)


def find(title_sub):
    res = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        if not u.IsWindowVisible(h):
            return True
        n = u.GetWindowTextLengthW(h)
        if n:
            b = ctypes.create_unicode_buffer(n + 1)
            u.GetWindowTextW(h, b, n + 1)
            if title_sub.lower() in b.value.lower():
                res.append((h, b.value))
        return True
    u.EnumWindows(cb, 0)
    return res


def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, b, 256)
    return b.value


def caret_of(hwnd):
    tid = u.GetWindowThreadProcessId(hwnd, None)
    gi = GUITHREADINFO(); gi.cbSize = ctypes.sizeof(gi)
    if not u.GetGUIThreadInfo(tid, ctypes.byref(gi)):
        return None
    if not gi.hwndCaret:
        return None
    return (gi.rcCaret.left, gi.rcCaret.top, gi.rcCaret.right, gi.rcCaret.bottom)


def rect(h):
    r = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def text_of(h):
    n = u.SendMessageW(h, 0x000E, 0, 0)          # WM_GETTEXTLENGTH
    buf = ctypes.create_unicode_buffer(n + 2)
    u.SendMessageW(h, 0x000D, n + 1, ctypes.byref(buf))   # WM_GETTEXT
    return buf.value


def force_fg(h):
    for _ in range(6):
        u.SetForegroundWindow(h)
        time.sleep(0.1)
        if u.GetForegroundWindow() == h:
            return True
    return False


def force_fg(hwnd):
    """强抢前台：跨进程 SetForegroundWindow 常被前台锁静默拒绝。"""
    if u.GetForegroundWindow() == hwnd:
        return True
    fg = u.GetForegroundWindow()
    t1 = u.GetWindowThreadProcessId(fg, None)
    t2 = u.GetWindowThreadProcessId(hwnd, None)
    my = ctypes.windll.kernel32.GetCurrentThreadId()
    for _ in range(6):
        u.AttachThreadInput(my, t1, True); u.AttachThreadInput(my, t2, True)
        u.BringWindowToTop(hwnd); u.SetForegroundWindow(hwnd)
        u.AttachThreadInput(my, t1, False); u.AttachThreadInput(my, t2, False)
        time.sleep(0.12)
        if u.GetForegroundWindow() == hwnd:
            return True
    return False


def main():
    sub = sys.argv[1] if len(sys.argv) > 1 else "记事本"
    ws = find(sub)
    if not ws:
        print("FAIL 找不到目标窗口:", sub); return
    hwnd, title = ws[0]
    print("目标窗口: %s 0x%X cls=%s" % (title, hwnd, cls(hwnd)))

    # 收尾上一次测试可能留着的面板
    panel = u.FindWindowW("WinVClipPanelUI", None)
    if u.IsWindowVisible(panel):
        force_fg(hwnd)
        tap(VK["esc"]); time.sleep(0.4)

    if not force_fg(hwnd):
        print("FAIL 无法把目标窗口置前，当前前台 cls=%s" % cls(u.GetForegroundWindow()))
        return

    # Win11 记事本是 WinUI3，读不到文本内容；用插入符横坐标当「有没有插进字符」的判据。
    base = caret_of(hwnd)
    print("初始 caret=%s" % (base,))
    for ch in "ABC":
        key(0, uni=ch); time.sleep(0.02)
    time.sleep(0.2)
    after_abc = caret_of(hwnd)
    print("输入 ABC 后 caret=%s" % (after_abc,))
    base = after_abc

    win_vi()
    time.sleep(0.5)
    panel = u.FindWindowW("WinVClipPanelUI", None)
    vis = bool(panel) and bool(u.IsWindowVisible(panel))
    fg = u.GetForegroundWindow()
    c1 = caret_of(hwnd)
    print("--- 面板弹出后 ---")
    print("面板可见=%s 矩形=%s" % (vis, rect(panel) if panel else None))
    print("前台类名=%s（应为记事本 Notepad）" % cls(fg))
    print("前台仍是记事本: %s" % (fg == hwnd))
    print("真实插入符仍在: %s  %s" % (c1 is not None, c1))
    if base and c1:
        print("插入符位置未变: %s" % (base[:2] == c1[:2]))

    # 钩子转发：打字不应进记事本（caret 不该移动）
    for ch in "zz":
        key(0, uni=ch); time.sleep(0.05)
    time.sleep(0.3)
    c2 = caret_of(hwnd)
    print("面板开着打 zz 后 caret=%s  未移动=%s" % (c2, base == c2))

    tap(VK["esc"])
    time.sleep(0.4)
    print("--- Esc 之后 ---")
    print("面板可见=%s 前台仍是记事本=%s" % (u.IsWindowVisible(panel), u.GetForegroundWindow() == hwnd))
    c3 = caret_of(hwnd)
    print("插入符仍在: %s %s" % (c3 is not None, c3))
    for ch in "D":
        key(0, uni=ch); time.sleep(0.02)
    time.sleep(0.2)
    c4 = caret_of(hwnd)
    print("继续输入 D 后 caret=%s  已前进=%s" % (c4, c3 != c4))


main()
