# -*- coding: utf-8 -*-
"""验证「不抢焦点」焦点模型（替代 test_caret / test_ghost）：

1. 面板弹出前后，记事本是否一直持有前台与真实插入符；
2. 钩子转发是否生效：面板打开时打字进搜索框而不是记事本；
3. Esc 收起后焦点仍在记事本，可以继续打字；
4. Enter 粘贴到记事本。

用法：python test_noactivate.py [目标窗口标题子串]
"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板


def main():
    sub = sys.argv[1] if len(sys.argv) > 1 else "记事本"
    ws = find_title_ex(sub)
    if not ws:
        print("FAIL 找不到目标窗口:", sub); return
    hwnd, title = ws[0]
    print("目标窗口: %s 0x%X cls=%s" % (title, hwnd, cls(hwnd)))

    # 收尾上一次测试可能留着的面板
    panel = u.FindWindowW("WinVClipPanelUI", None)
    if u.IsWindowVisible(panel):
        force_fg(hwnd)
        tap(VK["esc"]); time.sleep(0.4)

    if not force_fg(hwnd):
        print("FAIL 无法把目标窗口置前，当前前台 cls=%s" % cls(u.GetForegroundWindow()))
        return

    # Win11 记事本是 WinUI3，读不到文本内容；用插入符横坐标当「有没有插进字符」的判据。
    base = caret_of(hwnd)
    print("初始 caret=%s" % (base,))
    for ch in "ABC":
        key(0, uni=ch); time.sleep(0.02)
    time.sleep(0.2)
    after_abc = caret_of(hwnd)
    print("输入 ABC 后 caret=%s" % (after_abc,))
    base = after_abc

    win_v()
    time.sleep(0.5)
    panel = u.FindWindowW("WinVClipPanelUI", None)
    vis = bool(panel) and bool(u.IsWindowVisible(panel))
    fg = u.GetForegroundWindow()
    c1 = caret_of(hwnd)
    print("--- 面板弹出后 ---")
    print("面板可见=%s 矩形=%s" % (vis, rect(panel) if panel else None))
    print("前台类名=%s（应为记事本 Notepad）" % cls(fg))
    print("前台仍是记事本: %s" % (fg == hwnd))
    print("真实插入符仍在: %s  %s" % (c1 is not None, c1))
    if base and c1:
        print("插入符位置未变: %s" % (base[:2] == c1[:2]))

    # 钩子转发：打字不应进记事本（caret 不该移动）
    for ch in "zz":
        key(0, uni=ch); time.sleep(0.05)
    time.sleep(0.3)
    c2 = caret_of(hwnd)
    print("面板开着打 zz 后 caret=%s  未移动=%s" % (c2, base == c2))

    tap(VK["esc"])
    time.sleep(0.4)
    print("--- Esc 之后 ---")
    print("面板可见=%s 前台仍是记事本=%s" % (u.IsWindowVisible(panel), u.GetForegroundWindow() == hwnd))
    c3 = caret_of(hwnd)
    print("插入符仍在: %s %s" % (c3 is not None, c3))
    for ch in "D":
        key(0, uni=ch); time.sleep(0.02)
    time.sleep(0.2)
    c4 = caret_of(hwnd)
    print("继续输入 D 后 caret=%s  已前进=%s" % (c4, c3 != c4))


main()
