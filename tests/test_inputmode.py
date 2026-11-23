# -*- coding: utf-8 -*-
"""输入态（点搜索框才激活面板）验证：

1. 面板弹出后是「非激活态」：前台仍是目标窗口；
2. 点击搜索框 → 面板真正拿到前台（输入态），输入法才能工作；
3. 输入态下按 Esc 收起面板，前台还给目标窗口。

用法：python test_inputmode.py
"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板


def click_panel(panel, x, y):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(panel, 0x0201, 1, lp)
    u.PostMessageW(panel, 0x0202, 0, lp)


hwnd = u.FindWindowW("Notepad", None)
panel = u.FindWindowW(PANEL_CLASS, None)   # 可能还开着的面板

# 收尾上一次测试可能留着的面板
if u.IsWindowVisible(panel):
    force_fg(hwnd)
    tap(0x1B); time.sleep(0.4)

if not force_fg(hwnd):
    print("FAIL 无法把记事本置前，当前前台 cls=%s" % cls(u.GetForegroundWindow()))
    raise SystemExit(1)

win_v(); time.sleep(0.5)
print("面板可见=%s 非激活态(前台仍是记事本)=%s" % (u.IsWindowVisible(panel),
      u.GetForegroundWindow() == hwnd))

click_panel(panel, 120, 60)          # 搜索框
time.sleep(0.4)
cur = fg()
print("点搜索框后 前台类名=%s 面板拿到前台=%s" % (cls(cur), cur == panel))

tap(0x1B)                             # Esc 收起
time.sleep(0.5)
print("Esc 后 面板可见=%s 前台回到记事本=%s (cls=%s)" % (
      u.IsWindowVisible(panel), u.GetForegroundWindow() == hwnd, cls(u.GetForegroundWindow())))
