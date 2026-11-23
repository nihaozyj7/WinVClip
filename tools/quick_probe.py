# -*- coding: utf-8 -*-
"""点一下屏幕坐标、发 Win+V、读 trace 末尾。用于验证面板落点。

用法：python winvclip/tools/quick_probe.py <screenX> <screenY> [收起来=1]
"""
import ctypes, os, sys, time
from ctypes import wintypes

u = ctypes.windll.user32


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
    time.sleep(0.06)
    i = INPUT(); i.type = 0; i.u.mi.dwFlags = 0x0002
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))
    time.sleep(0.05)
    i.u.mi.dwFlags = 0x0004
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def winv():
    key(0x5B, True)          # 先补一个 Win up，防上一轮 keyup 被钩子吞掉卡住系统状态
    time.sleep(0.03)
    key(0x5B); time.sleep(0.04)
    key(0x56); time.sleep(0.04)
    key(0x56, True); time.sleep(0.04)
    key(0x5B, True)


def main():
    x = int(sys.argv[1]); y = int(sys.argv[2])
    click(x, y)
    time.sleep(0.35)
    p = os.path.join(os.environ["TEMP"], "winvclip-trace.txt")
    off = os.path.getsize(p) if os.path.exists(p) else 0
    winv()
    time.sleep(0.9)
    if os.path.exists(p):
        with open(p, "rb") as f:
            f.seek(off)
            print(f.read().decode("utf-8", "replace"))
    if len(sys.argv) > 3:
        winv()
        time.sleep(0.4)


main()
