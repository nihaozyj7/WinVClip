# -*- coding: utf-8 -*-
"""字形页自动化验证：弹面板 → 立刻点「临时固定」→ 之后可稳定截图/点击。

用法：
  python test_glyph_ui.py ping            # 弹面板并固定
  python test_glyph_ui.py click X Y       # 向面板投递一次左键单击（客户区坐标）
  python test_glyph_ui.py state           # 打印面板可见性 / 矩形
"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "ping"
    hwnd = panel()
    if cmd == "ping":
        if not u.IsWindowVisible(hwnd):
            key(0x5B); time.sleep(0.04)
            key(0x56); time.sleep(0.04)
            key(0x56, True); time.sleep(0.04)
            key(0x5B, True)
            time.sleep(0.10)
        post_click(hwnd, 485, 17)                 # 图钉 → 临时固定
        time.sleep(0.25)
        print("pinned=%s visible=%s" % (u.IsWindowVisible(hwnd), u.IsWindowVisible(hwnd)))
    elif cmd == "click":
        post_click(hwnd, int(sys.argv[2]), int(sys.argv[3]))
        print("clicked")
    elif cmd == "wheel":
        # wheel <delta>：正数向上 / 负数向下（lParam 用客户区坐标转屏幕坐标）
        delta = int(sys.argv[2])
        pt = wintypes.POINT(int(sys.argv[3]), int(sys.argv[4]))
        u.ClientToScreen(hwnd, ctypes.byref(pt))
        lp = ((pt.y & 0xFFFF) << 16) | (pt.x & 0xFFFF)
        wp = ((delta & 0xFFFF) << 16) & 0xFFFFFFFF
        u.PostMessageW(hwnd, 0x020A, wp, lp)
        print("wheel sent")
    elif cmd == "char":
        # 直接投递 WM_CHAR（绕开输入法），用于测试跨分类搜索
        for hexcp in sys.argv[2:]:
            u.PostMessageW(hwnd, 0x0102, int(hexcp, 16), 0)
        print("chars sent")
    elif cmd == "state":
        r = wintypes.RECT()
        u.GetWindowRect(hwnd, ctypes.byref(r))
        print("hwnd=%s visible=%s rect=(%d,%d,%d,%d)" % (
            hex(hwnd), u.IsWindowVisible(hwnd), r.left, r.top, r.right, r.bottom))


main()
