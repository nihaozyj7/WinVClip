
import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板
# -*- coding: utf-8 -*-
# 弹面板并立刻点击标题栏的「临时固定」按钮，让面板置顶且不失焦隐藏，便于自动化验证


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
post_click(hwnd, 485, 17)     # 图钉按钮
print("pinned, hwnd=%s" % hex(hwnd))
