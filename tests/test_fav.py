# -*- coding: utf-8 -*-
"""收藏页回归：Ctrl+D 收藏一条 → 切到收藏页 → 点第一行，应粘贴到目标窗口。"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板


def main():
    proc = subprocess.Popen(["notepad.exe"])
    time.sleep(1.6)
    np = find_notepad()
    if not np:
        raise SystemExit("notepad not found")
    force_fg(np); time.sleep(0.3)

    set_clip("FEED-TEXT-777")     # 让历史里有一条独特文本
    time.sleep(0.6)

    h = panel()
    if vis(h):
        win_v(); time.sleep(0.4)
    for _ in range(6):
        win_v()
        for _ in range(30):
            if vis(h):
                break
            time.sleep(0.05)
        if vis(h):
            break
    if not vis(h):
        proc.terminate()
        raise SystemExit("panel did not show")

    post_click(h, 60, 17)              # 历史页
    time.sleep(0.5)
    # Ctrl 组合在非激活态整段放行（面板不抢前台程序的 Ctrl+S / Ctrl+F，排在钩子链
    # 后面的别家全局热键也才收得到），所以收藏快捷键要先进输入态：
    # 点一下搜索框让面板拿到前台，Ctrl+D 走系统 WM_KEYDOWN 直达面板。
    post_click(h, 120, 60)             # 搜索框 → 输入态
    time.sleep(0.4)
    key(0x11)                     # Ctrl
    key(0x44)                     # D
    key(0x44, True)
    key(0x11, True)
    time.sleep(0.6)
    print("visible after ctrl+D:", vis(h))

    # 输入态里 Esc 直达面板 → 收起；再唤起，去收藏页取那条
    key(0x1B); time.sleep(0.4)
    for _ in range(6):
        win_v()
        for _ in range(30):
            if vis(h):
                break
            time.sleep(0.05)
        if vis(h):
            break
    if not vis(h):
        proc.terminate()
        raise SystemExit("panel did not show (2nd)")

    post_click(h, 175, 17)             # 收藏页
    time.sleep(0.6)
    post_click(h, 300, 170)            # 第一行
    time.sleep(1.0)

    print("visible after row click:", vis(h))
    # Win11 记事本是 WinUI3，没有 Edit 子窗口，读回文本只能走剪贴板
    if force_fg(np):
        time.sleep(0.3)
        combo(0x11, 0x41)             # Ctrl+A
        combo(0x11, 0x43)             # Ctrl+C
        time.sleep(0.5)
        print("notepad content:", repr(get_clip()))
    else:
        print("FAIL 记事本拿不到前台")
    proc.terminate()


main()
