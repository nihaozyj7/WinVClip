# -*- coding: utf-8 -*-
# 弹面板并立刻点击标题栏的「临时固定」按钮，让面板置顶且不失焦隐藏，便于自动化验证
# --- 路径基准：从 tests/ 回到项目根（P0 结构整理时注入） ---
import os as _os, sys as _sys
_os.chdir(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import ctypes, time
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]

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


def click(hwnd, x, y):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(hwnd, 0x0201, 1, lp)   # WM_LBUTTONDOWN
    u.PostMessageW(hwnd, 0x0202, 0, lp)   # WM_LBUTTONUP


key(0x5B); time.sleep(0.05)
key(0x56); time.sleep(0.05)
key(0x56, True); time.sleep(0.05)
key(0x5B, True)

hwnd = None
for _ in range(40):
    hwnd = u.FindWindowW("WinVClipPanelUI", None)
    if hwnd:
        break
    time.sleep(0.05)
if not hwnd:
    raise SystemExit("panel not created")
time.sleep(0.35)
click(hwnd, 485, 17)     # 图钉按钮
print("pinned, hwnd=%s" % hex(hwnd))
