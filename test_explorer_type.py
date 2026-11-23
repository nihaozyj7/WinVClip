# -*- coding: utf-8 -*-
"""在资源管理器（没有文本输入区）里点表情：应当拒绝插入并提示，而不是注入字符。

以前的行为：Unicode 注入落到 Shell 的键入查找上 → 系统提示音叮一声、什么都没插入、
目标窗口失焦。现在应当只弹 toast「当前窗口不接受文本输入」，且 explorer 仍能正常获得焦点。
"""
import ctypes, subprocess, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
u.IsWindowVisible.argtypes = [ctypes.c_void_p]
u.SetForegroundWindow.argtypes = [ctypes.c_void_p]
u.GetForegroundWindow.restype = ctypes.c_void_p


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
    key(0x5B); time.sleep(0.05); key(0x56); time.sleep(0.05)
    key(0x56, True); time.sleep(0.05); key(0x5B, True)


def click(h, x, y):
    lp = ((y & 0xFFFF) << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0200, 0, lp)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


def explorer_window():
    for _ in range(80):
        h = u.FindWindowW("CabinetWClass", None)
        if h and u.IsWindowVisible(h):
            return h
        time.sleep(0.05)
    return None


def main():
    exp = u.FindWindowW("CabinetWClass", None)
    if not exp:
        subprocess.Popen(["explorer.exe", "C:\\Windows"])
        exp = explorer_window()
    if not exp:
        raise SystemExit("explorer not found")
    print("explorer hwnd=0x%X" % exp)

    u.SetForegroundWindow(exp)
    time.sleep(0.5)
    tid = u.GetWindowThreadProcessId(exp, None)
    g = G(); g.cbSize = ctypes.sizeof(g)
    u.GetGUIThreadInfo(tid, ctypes.byref(g))
    print("explorer hwndCaret = 0x%X (0 表示没有文本输入区)" % (g.hwndCaret or 0))

    panel = u.FindWindowW("WinVClipPanelUI", None)
    if not panel:
        raise SystemExit("panel not created")
    if u.IsWindowVisible(panel):
        winv(); time.sleep(0.4)
    for _ in range(6):
        winv()
        for _ in range(40):
            if u.IsWindowVisible(panel):
                break
            time.sleep(0.05)
        if u.IsWindowVisible(panel):
            break
    time.sleep(0.3)
    print("panel visible:", bool(u.IsWindowVisible(panel)))
    if not u.IsWindowVisible(panel):
        raise SystemExit("panel did not show")

    click(panel, 235, 17)          # 表情页签
    time.sleep(0.5)
    before = u.GetForegroundWindow()
    click(panel, 40, 170)          # 第一格 emoji
    time.sleep(0.6)
    after = u.GetForegroundWindow()
    print("点击后用户提示: 见截图")
    print("panel still visible:", bool(u.IsWindowVisible(panel)))
    print("foreground before/after: 0x%X / 0x%X" % (before, after))

    # 关闭面板，验证 explorer 还能正常拿回焦点
    winv()
    time.sleep(0.5)
    u.SetForegroundWindow(exp)
    time.sleep(0.4)
    print("面板关闭后 foreground = 0x%X (期望 0x%X)" % (u.GetForegroundWindow(), exp))
    print("explorer 可正常获得焦点:", u.GetForegroundWindow() == exp)


main()
