
import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板
# -*- coding: utf-8 -*-
# 直接向面板窗口投递 WM_RBUTTONUP(x,y)，排除 desktop 工具点击语义的干扰
hwnd = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0
x, y = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (280, 112)
lp = (y << 16) | (x & 0xFFFF)
u.PostMessageW(hwnd, 0x0205, 0, lp)   # WM_RBUTTONUP
print("posted WM_RBUTTONUP", x, y)
