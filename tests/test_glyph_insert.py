# -*- coding: utf-8 -*-
"""验证「点表情 = 直插目标窗口，剪贴板不动」。

流程：起记事本 → Win+V（非固定态）→ 切表情页 → 搜索 cat → 点第一格
检查：剪贴板仍是哨兵；记事本多了一个彩色 emoji。
"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板


def char(h, cp):
    u.PostMessageW(h, 0x0102, cp, 0)


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
    time.sleep(0.3)
    print("panel visible:", vis(h))
    if not vis(h):
        proc.terminate()
        raise SystemExit("panel did not show")

    set_clip("SENTINEL-123")
    time.sleep(0.4)

    mode = sys.argv[1] if len(sys.argv) > 1 else "emo"
    post_click(h, 235 if mode == "emo" else 300, 17)
    time.sleep(0.5)
    if mode == "emo":
        for ch in b"cat":
            char(h, ch)
            time.sleep(0.12)
        time.sleep(0.5)
    print("visible after search:", vis(h))

    post_click(h, 40, 170)                 # 第一格
    time.sleep(0.9)

    print("visible after click:", vis(h))
    print("clip after click:", repr(get_clip()))
    print("notepad text:", repr(edit_text(np)))
    proc.terminate()


main()
