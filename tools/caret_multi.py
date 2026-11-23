# -*- coding: utf-8 -*-
"""对多种网页控件做插入符探测：Tab 逐个聚焦，发 Win+V，读 winvclip 的 trace 增量。

用法：python winvclip/tools/caret_multi.py 4
"""
import ctypes, os, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowExW.restype = ctypes.c_void_p
u.SetForegroundWindow.argtypes = [ctypes.c_void_p]
u.GetForegroundWindow.restype = ctypes.c_void_p


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class _I(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("pad", ctypes.c_byte * 40)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _I)]


def key(vk, up=False):
    i = INPUT(); i.type = 1; i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def click(x, y):
    u.SetCursorPos(int(x), int(y))
    time.sleep(0.05)
    i = INPUT(); i.type = 0
    i.u.mi.dwFlags = 0x0002          # LEFTDOWN
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))
    time.sleep(0.04)
    i.u.mi.dwFlags = 0x0004          # LEFTUP
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def winv():
    key(0x5B); time.sleep(0.04)
    key(0x56); time.sleep(0.04)
    key(0x56, True); time.sleep(0.04)
    key(0x5B, True)


def trace_path():
    return os.path.join(os.environ.get("TEMP", "."), "winvclip-trace.txt")


def tail(path, off):
    if not os.path.exists(path):
        return "", 0
    with open(path, "rb") as f:
        f.seek(off)
        data = f.read()
    return data.decode("utf-8", "replace"), off + len(data)


def find_window(sub):
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, ctypes.c_void_p, wintypes.LPARAM)
    def cb(h, lp):
        n = u.GetWindowTextLengthW(h)
        if n:
            b = ctypes.create_unicode_buffer(n + 1)
            u.GetWindowTextW(h, b, n + 1)
            if sub.lower() in b.value.lower():
                found.append(h)
        return True

    u.EnumWindows(cb, 0)
    return found[0] if found else None


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    hwnd = find_window("caret probe2")
    if not hwnd:
        raise SystemExit("probe window not found")
    u.SetForegroundWindow(hwnd)
    time.sleep(0.4)
    if u.GetForegroundWindow() != hwnd:
        # 前台受限：点一下标题栏强制激活
        r = wintypes.RECT()
        u.GetWindowRect(hwnd, ctypes.byref(r))
        click((r.left + r.right) // 2, r.top + 12)
        time.sleep(0.4)
    # 点页面标题空白处，让焦点落到 document 上，Tab 从头开始
    click(200, 90)
    time.sleep(0.3)
    off = os.path.getsize(trace_path()) if os.path.exists(trace_path()) else 0
    for i in range(n):
        key(0x09)                       # Tab
        time.sleep(0.25)
        winv()
        time.sleep(0.5)
        out, off = tail(trace_path(), off)
        lines = [l for l in out.splitlines() if ("caret:" in l or "uia:" in l or "place:" in l)]
        print("=== Tab #%d ===" % (i + 1))
        print("\n".join(lines) if lines else "(no caret/uia lines)")
        winv()                          # 收起
        time.sleep(0.35)


main()
