# -*- coding: utf-8 -*-
"""非激活面板下的键盘/粘贴链路：

1. Win+V 弹出面板（前台仍是目标窗口）；
2. Enter 把选中条目粘贴进记事本，面板收起；
3. 面板开着时按 Ctrl+2 切页、Esc 收起；
4. 鼠标点击面板条目（PostMessage 客户区坐标）也能粘贴。

用法：python test_noactivate_keys.py [目标窗口标题子串]
"""
import ctypes, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
k32 = ctypes.windll.kernel32
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

VK = dict(win=0x5B, v=0x56, esc=0x1B, enter=0x0D, down=0x28, up=0x26, ctrl=0x11)


def key(vk, up=False):
    i = INPUT(); i.type = 1; i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def tap(vk):
    key(vk); time.sleep(0.04); key(vk, True); time.sleep(0.04)


def win_vi():
    key(VK["win"]); time.sleep(0.05)
    key(VK["v"]); time.sleep(0.05)
    key(VK["v"], True); time.sleep(0.05)
    key(VK["win"], True); time.sleep(0.1)


def set_clip(text):
    k32.GlobalAlloc.restype = ctypes.c_void_p
    k32.GlobalLock.restype = ctypes.c_void_p
    k32.GlobalLock.argtypes = [ctypes.c_void_p]
    k32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    u.SetClipboardData.restype = ctypes.c_void_p
    u.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
    u.OpenClipboard(None); u.EmptyClipboard()
    n = (len(text) + 1) * 2
    h = k32.GlobalAlloc(0x0042, n)
    p = k32.GlobalLock(h)
    ctypes.memmove(p, text.encode("utf-16-le") + b"\x00\x00", n)
    k32.GlobalUnlock(h)
    u.SetClipboardData(13, h)   # CF_UNICODETEXT
    u.CloseClipboard()


def cls(h):
    b = ctypes.create_unicode_buffer(256); u.GetClassNameW(h, b, 256); return b.value


def find(title_sub):
    res = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        if u.IsWindowVisible(h) and u.GetWindowTextLengthW(h):
            b = ctypes.create_unicode_buffer(512)
            u.GetWindowTextW(h, b, 512)
            if title_sub.lower() in b.value.lower():
                res.append((h, b.value))
        return True
    u.EnumWindows(cb, 0)
    return res


def caret_x(hwnd):
    tid = u.GetWindowThreadProcessId(hwnd, None)
    gi = GUITHREADINFO(); gi.cbSize = ctypes.sizeof(gi)
    if not u.GetGUIThreadInfo(tid, ctypes.byref(gi)) or not gi.hwndCaret:
        return None
    return gi.rcCaret.left


def rect(h):
    r = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def main():
    sub = sys.argv[1] if len(sys.argv) > 1 else "记事本"
    ws = find(sub)
    if not ws:
        print("FAIL 找不到", sub); return
    hwnd = ws[0][0]
    panel = u.FindWindowW("WinVClipPanelUI", None)
    if u.GetForegroundWindow() != hwnd:
        print("WARN 前台不是目标窗口 cls=%s" % cls(u.GetForegroundWindow()))

    set_clip("PASTED-1")
    time.sleep(0.4)

    x0 = caret_x(hwnd)
    win_vi(); time.sleep(0.6)
    print("面板可见=%s 前台是记事本=%s" % (u.IsWindowVisible(panel), u.GetForegroundWindow() == hwnd))

    # 非激活态的 Ctrl 组合现在整段放行（Ctrl+S / Ctrl+F 属于前台程序，面板不抢，
    # 排在钩子链后面的别家全局热键也才收得到），切页改用 Tab：
    # 输入态（点搜索框）里 Ctrl+数字 依然有效，走的是系统 WM_KEYDOWN。
    tap(0x09); time.sleep(0.3)        # Tab -> 收藏页
    tap(0x09); time.sleep(0.3)        # Tab -> 表情页
    tap(0x09); time.sleep(0.3)        # Tab -> 符号页
    tap(0x09); time.sleep(0.3)        # Tab -> 历史页
    print("Tab 切页后面板仍可见=%s" % u.IsWindowVisible(panel))

    tap(VK["enter"])
    time.sleep(0.9)
    x1 = caret_x(hwnd)
    print("Enter 后面板可见=%s（应 False）前台是记事本=%s" % (u.IsWindowVisible(panel), u.GetForegroundWindow() == hwnd))
    print("插入符 x: %s -> %s  前进=%s" % (x0, x1, (x0 is not None and x1 is not None and x1 > x0)))

    # 鼠标点击第一条
    set_clip("PASTED-2")
    time.sleep(0.4)
    win_vi(); time.sleep(0.6)
    r = rect(panel)
    # 列表首行：body 顶部往下约 改：直接点 body 区域垂直居中偏上
    cx = (r[2] - r[0]) // 2
    client_y = 150
    u.PostMessageW(panel, 0x0201, 1, (client_y << 16) | (cx & 0xFFFF))   # WM_LBUTTONDOWN
    u.PostMessageW(panel, 0x0202, 0, (client_y << 16) | (cx & 0xFFFF))   # WM_LBUTTONUP
    time.sleep(0.9)
    x2 = caret_x(hwnd)
    print("点击后面板可见=%s 前台是记事本=%s" % (u.IsWindowVisible(panel), u.GetForegroundWindow() == hwnd))
    print("插入符 x: %s -> %s  前进=%s" % (x1, x2, (x1 is not None and x2 is not None and x2 > x1)))


main()
