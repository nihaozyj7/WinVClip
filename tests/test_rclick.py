# -*- coding: utf-8 -*-
# 直接向面板窗口投递 WM_RBUTTONUP(x,y)，排除 desktop 工具点击语义的干扰
# --- 路径基准：从 tests/ 回到项目根（P0 结构整理时注入） ---
import os as _os, sys as _sys
_os.chdir(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import ctypes, sys
u = ctypes.windll.user32
hwnd = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0
x, y = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (280, 112)
u.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
lp = (y << 16) | (x & 0xFFFF)
u.PostMessageW(hwnd, 0x0205, 0, lp)   # WM_RBUTTONUP
print("posted WM_RBUTTONUP", x, y)
