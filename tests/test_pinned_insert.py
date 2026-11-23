# -*- coding: utf-8 -*-
"""验证临时固定态：连点多个表情，面板不消失、剪贴板不动、都能插进去。"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板


def main():
    proc = subprocess.Popen(["notepad.exe"])
    time.sleep(1.6)
    np = u.FindWindowW(None, "无标题 - 记事本") or u.FindWindowW(None, "Untitled - Notepad")
    if not np:
        raise SystemExit("notepad not found")
    if not force_fg(np):
        raise SystemExit("无法把记事本切到前台，测试不可信")
    time.sleep(0.3)

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

    set_clip("SENTINEL-123")
    time.sleep(0.4)

    # 固定（图钉在右上一排的左边那个：关闭贴右缘，向左依次是齿轮、图钉）
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    W = r.right - r.left
    post_click(h, W - 75, 17)
    time.sleep(0.4)
    print("pinned, visible:", vis(h))

    post_click(h, 184, 17)                 # 表情页
    time.sleep(0.4)
    for ch in b"cat":
        u.PostMessageW(h, 0x0102, ch, 0)
        time.sleep(0.12)
    time.sleep(0.5)

    for x in (34, 83, 132):
        post_click(h, x, 170)
        time.sleep(0.8)

    print("visible after 3 clicks:", vis(h))
    print("clip:", repr(get_clip()))
    print("notepad text:", repr(edit_text(np)))

    # 取消固定，再点一次应关闭面板
    post_click(h, W - 75, 17)
    time.sleep(0.3)
    print("unpinned, visible:", vis(h))
    proc.terminate()


main()
