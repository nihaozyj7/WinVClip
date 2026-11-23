# -*- coding: utf-8 -*-
"""资源管理器文件列表（没有文本输入区）里点表情：走「借剪贴板 + Ctrl+V」通道。

历史演变（这一条曲线的教训值得留着）：
  1. 最早一律用 KEYEVENTF_UNICODE 直插 → 文件列表被 Shell 当成「键入查找」，
     叮一声系统提示音，什么都没插入；
  2. 于是改成「没有插入符就拒绝插入」——但「取不到插入符」不能推出「不能粘」：
     Edge 地址栏、Chromium 页面这类目标的插入符在渲染进程里，一律取不到，
     结果连它们也一起被拒了，用户看到的就是「系统剪贴板能粘、表情点不动」；
  3. 现在按通道分流：有插入符 → 直插；取不到插入符 → 借剪贴板 + Ctrl+V。
     文件列表收到 Ctrl+V 对文本剪贴板是无操作（不响铃），剪贴板粘完还原。

期望：面板收起（正常走完插入流程）、剪贴板仍是哨兵、explorer 前台不乱。
"""
import ctypes, subprocess, time
from ctypes import wintypes

u = ctypes.windll.user32
k = ctypes.windll.kernel32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.GetForegroundWindow.restype = ctypes.c_void_p
u.GetClipboardData.restype = ctypes.c_void_p
u.GetClipboardData.argtypes = [wintypes.UINT]
u.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
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


class G(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hwndActive", wintypes.HWND), ("hwndFocus", wintypes.HWND),
                ("hwndCapture", wintypes.HWND), ("hwndMenuOwner", wintypes.HWND),
                ("hwndMoveSize", wintypes.HWND), ("hwndCaret", wintypes.HWND),
                ("rcCaret", wintypes.RECT)]


def key(vk, up=False):
    i = INPUT(); i.type = 1; i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def winv():
    key(0x5B); time.sleep(0.06); key(0x56); time.sleep(0.06)
    key(0x56, True); time.sleep(0.06); key(0x5B, True)
    time.sleep(0.9)


def click(h, x, y):
    lp = ((y & 0xFFFF) << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0200, 0, lp)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


def set_clip(s):
    u.OpenClipboard(None); u.EmptyClipboard()
    d = (s + "\0").encode("utf-16-le")
    h = k.GlobalAlloc(0x0042, len(d))
    p = k.GlobalLock(h); ctypes.memmove(p, d, len(d)); k.GlobalUnlock(h)
    u.SetClipboardData(13, h); u.CloseClipboard()


def get_clip():
    for _ in range(20):
        if u.OpenClipboard(None):
            break
        time.sleep(0.03)
    try:
        h = u.GetClipboardData(13)
        if not h:
            return ""
        p = k.GlobalLock(h); s = ctypes.wstring_at(p); k.GlobalUnlock(h)
        return s
    finally:
        u.CloseClipboard()


def explorer_window():
    for _ in range(80):
        h = u.FindWindowW("CabinetWClass", None)
        if h and u.IsWindowVisible(h):
            return h
        time.sleep(0.05)
    return None


def force_fg(hwnd):
    if u.GetForegroundWindow() == hwnd:
        return True
    fg = u.GetForegroundWindow()
    tid = u.GetWindowThreadProcessId(fg, None)
    my = k.GetCurrentThreadId()
    u.AttachThreadInput(my, tid, True)
    u.BringWindowToTop(hwnd); u.SetForegroundWindow(hwnd)
    u.SetActiveWindow(hwnd); u.SetFocus(hwnd)
    u.AttachThreadInput(my, tid, False)
    for _ in range(8):
        if u.GetForegroundWindow() == hwnd:
            return True
        key(0x12); key(0x12, True); u.SetForegroundWindow(hwnd); time.sleep(0.05)
    return u.GetForegroundWindow() == hwnd


def main():
    exp = u.FindWindowW("CabinetWClass", None)
    if not exp:
        subprocess.Popen(["explorer.exe", "C:\\Windows"])
        exp = explorer_window()
    if not exp:
        raise SystemExit("explorer not found")
    print("explorer hwnd=0x%X" % exp)
    if not force_fg(exp):
        raise SystemExit("无法把资源管理器切到前台，测试不可信")
    time.sleep(0.5)

    # 点一下文件视图，让焦点落在「没有插入符」的列表上
    r = wintypes.RECT(); u.GetWindowRect(exp, ctypes.byref(r))
    u.SetCursorPos(r.left + 400, r.top + 400); time.sleep(0.15)
    for f in (0, 2, 4):
        u.mouse_event(f, 0, 0, 0, 0); time.sleep(0.08)
    time.sleep(0.5)

    tid = u.GetWindowThreadProcessId(exp, None)
    g = G(); g.cbSize = ctypes.sizeof(g)
    u.GetGUIThreadInfo(tid, ctypes.byref(g))
    print("explorer hwndCaret = 0x%X (0 表示没有文本输入区)" % (g.hwndCaret or 0))

    panel = u.FindWindowW("WinVClipPanelUI", None)
    if not panel:
        raise SystemExit("panel not created")
    if u.IsWindowVisible(panel):
        winv()
    winv()
    if not u.IsWindowVisible(panel):
        raise SystemExit("panel did not show")

    set_clip("ORIGINAL-ABC")
    time.sleep(0.5)
    click(panel, 235, 17)          # 表情页签
    time.sleep(0.5)
    click(panel, 40, 175)          # 第一格 emoji
    time.sleep(1.6)

    print("panel still visible:", bool(u.IsWindowVisible(panel)), "(期望 False)")
    print("clipboard after:", repr(get_clip()), "(期望 'ORIGINAL-ABC')")
    print("foreground = 0x%X (期望 0x%X)" % (u.GetForegroundWindow() or 0, exp))


main()
