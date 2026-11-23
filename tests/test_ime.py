# -*- coding: utf-8 -*-
"""真实输入法（微软拼音）验证：向面板发硬件级字母键，看组合串与候选窗。

用法：
  python test_ime.py compose weixiao   # 逐字母发送，面板应显示组合串（带下划线）
  python test_ime.py space             # 空格上屏
  python test_ime.py esc               # 取消组合

注意：这里必须用 SendInput 发 VK（而不是 KEYEVENTF_UNICODE），
      只有真实键盘事件才会被输入法拦截成组合串。
"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "compose"
    hwnd = u.FindWindowW("WinVClipPanelUI", None)
    if not hwnd:
        raise SystemExit("panel not created")
    u.SetForegroundWindow(hwnd)
    time.sleep(0.2)
    if cmd == "compose":
        for ch in sys.argv[2]:
            tap(ord(ch.upper()))
        print("composed %r" % sys.argv[2])
    elif cmd == "space":
        tap(0x20)
        print("space sent")
    elif cmd == "esc":
        tap(0x1B)
        print("esc sent")
    elif cmd == "enter":
        tap(0x0D)
        print("enter sent")
    elif cmd == "shift":
        tap(0x10)
        print("shift sent")


main()
