# -*- coding: utf-8 -*-
# 注入一次真实的 Win+V（SendInput），用于弹出面板做视觉验证
import ctypes, time
from ctypes import wintypes

u = ctypes.windll.user32

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

time.sleep(0.2)
key(0x5B)          # LWIN
time.sleep(0.05)
key(0x56)          # V
time.sleep(0.05)
key(0x56, True)
time.sleep(0.05)
key(0x5B, True)
print("sent")
