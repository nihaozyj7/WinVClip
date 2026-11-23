# -*- coding: utf-8 -*-
"""非激活面板下的键盘/粘贴链路：

1. Win+V 弹出面板（前台仍是目标窗口）；
2. Enter 把选中条目粘贴进记事本，面板收起；
3. 面板开着时按 Ctrl+2 切页、Esc 收起；
4. 鼠标点击面板条目（PostMessage 客户区坐标）也能粘贴。

用法：python test_noactivate_keys.py [目标窗口标题子串]
"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板


def caret_x(hwnd):
    tid = u.GetWindowThreadProcessId(hwnd, None)
    gi = GUITHREADINFO(); gi.cbSize = ctypes.sizeof(gi)
    if not u.GetGUIThreadInfo(tid, ctypes.byref(gi)) or not gi.hwndCaret:
        return None
    return gi.rcCaret.left


def main():
    sub = sys.argv[1] if len(sys.argv) > 1 else "记事本"
    ws = find_title_ex(sub)
    if not ws:
        print("FAIL 找不到", sub); return
    hwnd = ws[0][0]
    panel = u.FindWindowW("WinVClipPanelUI", None)
    if u.GetForegroundWindow() != hwnd:
        print("WARN 前台不是目标窗口 cls=%s" % cls(u.GetForegroundWindow()))

    set_clip("PASTED-1")
    time.sleep(0.4)

    x0 = caret_x(hwnd)
    win_v(); time.sleep(0.6)
    print("面板可见=%s 前台是记事本=%s" % (u.IsWindowVisible(panel), u.GetForegroundWindow() == hwnd))

    # 非激活态的 Ctrl 组合现在整段放行（Ctrl+S / Ctrl+F 属于前台程序，面板不抢，
    # 排在钩子链后面的别家全局热键也才收得到），切页改用 Tab：
    # 输入态（点搜索框）里 Ctrl+数字 依然有效，走的是系统 WM_KEYDOWN。
    tap(0x09); time.sleep(0.3)        # Tab -> 收藏页
    tap(0x09); time.sleep(0.3)        # Tab -> 表情页
    tap(0x09); time.sleep(0.3)        # Tab -> 符号页
    tap(0x09); time.sleep(0.3)        # Tab -> 历史页
    print("Tab 切页后面板仍可见=%s" % u.IsWindowVisible(panel))

    tap(VK["enter"])
    time.sleep(0.9)
    x1 = caret_x(hwnd)
    print("Enter 后面板可见=%s（应 False）前台是记事本=%s" % (u.IsWindowVisible(panel), u.GetForegroundWindow() == hwnd))
    print("插入符 x: %s -> %s  前进=%s" % (x0, x1, (x0 is not None and x1 is not None and x1 > x0)))

    # 鼠标点击第一条
    set_clip("PASTED-2")
    time.sleep(0.4)
    win_v(); time.sleep(0.6)
    r = rect(panel)
    # 列表首行：body 顶部往下约 改：直接点 body 区域垂直居中偏上
    cx = (r[2] - r[0]) // 2
    client_y = 150
    u.PostMessageW(panel, 0x0201, 1, (client_y << 16) | (cx & 0xFFFF))   # WM_LBUTTONDOWN
    u.PostMessageW(panel, 0x0202, 0, (client_y << 16) | (cx & 0xFFFF))   # WM_LBUTTONUP
    time.sleep(0.9)
    x2 = caret_x(hwnd)
    print("点击后面板可见=%s 前台是记事本=%s" % (u.IsWindowVisible(panel), u.GetForegroundWindow() == hwnd))
    print("插入符 x: %s -> %s  前进=%s" % (x1, x2, (x1 is not None and x2 is not None and x2 > x1)))


main()
