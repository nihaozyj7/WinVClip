# -*- coding: utf-8 -*-
# 依次往剪贴板塞若干条文本，每条之间留出时间给监听程序捕获
# --- 路径基准：从 tests/ 回到项目根（P0 结构整理时注入） ---
import os as _os, sys as _sys
_os.chdir(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import ctypes, time
from ctypes import wintypes

u = ctypes.windll.user32
k = ctypes.windll.kernel32
k.GlobalAlloc.restype = ctypes.c_void_p
k.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
k.GlobalLock.restype = ctypes.c_void_p
k.GlobalLock.argtypes = [ctypes.c_void_p]
k.GlobalUnlock.argtypes = [ctypes.c_void_p]
u.OpenClipboard.argtypes = [ctypes.c_void_p]
u.OpenClipboard.restype = wintypes.BOOL
u.SetClipboardData.restype = ctypes.c_void_p
u.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]

CF_UNICODETEXT = 13

TEXTS = [
    "https://www.bilibili.com/video/BV1xx411c7mD",
    "git commit -m \"feat: 增加另存为功能\"",
    "设计稿里的橙色是 #FF6B35，纸感底是 #FFFDF8",
    "这是一条比较长的文本，用来检查列表里单行标题的省略号截断是否正常工作，后面还有很多很多的字……",
]


def put(text):
    data = text.encode("utf-16-le") + b"\x00\x00"
    for _ in range(20):
        if u.OpenClipboard(None):
            break
        time.sleep(0.05)
    else:
        raise RuntimeError("OpenClipboard failed")
    u.EmptyClipboard()
    h = k.GlobalAlloc(2, len(data))
    p = k.GlobalLock(h)
    ctypes.memmove(p, data, len(data))
    k.GlobalUnlock(h)
    u.SetClipboardData(CF_UNICODETEXT, h)
    u.CloseClipboard()


for t in TEXTS:
    put(t)
    time.sleep(0.45)
print("ok")
