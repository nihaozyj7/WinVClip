# -*- coding: utf-8 -*-
"""验证两件事：
1. 面板是否贴着目标窗口的文本插入符（caret）弹出；
2. 面板抢焦点后，影子光标（WinVClipCaretGhost）是否出现在原 caret 位置。

用法：python test_caret.py [目标窗口标题子串]
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


def key(vk, up=False):
    i = INPUT(); i.type = 1; i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def win_vi():
    key(0x5B); time.sleep(0.05)
    key(0x56); time.sleep(0.05)
    key(0x56, True); time.sleep(0.05)
    key(0x5B, True)


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


def caret_of(hwnd):
    tid = u.GetWindowThreadProcessId(hwnd, None)
    gi = GUITHREADINFO(); gi.cbSize = ctypes.sizeof(gi)
    if not u.GetGUIThreadInfo(tid, ctypes.byref(gi)):
        return None
    if not gi.hwndCaret:
        return None
    rc = gi.rcCaret
    pt = wintypes.POINT(0, 0)
    u.ClientToScreen(gi.hwndCaret, ctypes.byref(pt))
    return (rc.left + pt.x, rc.top + pt.y, rc.right + pt.x, rc.bottom + pt.y)


def rect(h):
    r = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def main():
    sub = sys.argv[1] if len(sys.argv) > 1 else "记事本"
    ws = find(sub)
    if not ws:
        print("找不到目标窗口:", sub); return
    hwnd, title = ws[0]
    print("目标窗口: %s  0x%X" % (title, hwnd))
    u.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    c0 = caret_of(hwnd)
    print("插入符(caret)屏幕矩形:", c0)
    if not c0:
        print("目标窗口没有 caret，面板应居中"); 
    win_vi()
    time.sleep(0.5)

    panels = find("WinVClip")
    # 面板窗口是 toolwindow，标题为空，改用类名找
    panel = u.FindWindowW("WinVClipPanelUI", None)
    pr = rect(panel) if panel else None
    print("面板矩形:", pr)
    if c0 and pr:
        print("面板左上 - caret 左下 = (%d, %d)" % (pr[0] - c0[0], pr[1] - c0[3]))

    ghost = u.FindWindowW("WinVClipCaretGhost", None)
    gv = bool(ghost) and bool(u.IsWindowVisible(ghost))
    gr = rect(ghost) if ghost else None
    print("影子光标: 存在=%s 可见=%s 矩形=%s" % (bool(ghost), gv, gr))
    if c0 and gr:
        print("影子 - caret 偏差 = (%d, %d)" % (gr[0] - c0[0], gr[1] - c0[1]))

    # 前台归属
    fg = u.GetForegroundWindow()
    nb = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(fg, nb, 256)
    print("当前前台窗口类名:", nb.value)


main()
