# -*- coding: utf-8 -*-
"""验证：唤出面板再关掉之后，资源管理器地址栏的焦点要回来，能继续打字。
做法：地址栏输入 ZZZ -> Win+V -> Esc -> 输入 AAA -> Ctrl+A/Ctrl+C 读回。"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板


class G(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hwndActive", wintypes.HWND), ("hwndFocus", wintypes.HWND),
                ("hwndCapture", wintypes.HWND), ("hwndMenuOwner", wintypes.HWND),
                ("hwndMoveSize", wintypes.HWND), ("hwndCaret", wintypes.HWND),
                ("rcCaret", wintypes.RECT)]


def esc():
    key(0x1B); time.sleep(0.03); key(0x1B, True)


def main():
    exp = u.FindWindowW("CabinetWClass", None)
    if not exp:
        subprocess.Popen(["explorer.exe", "C:\\Windows"])
        for _ in range(60):
            exp = u.FindWindowW("CabinetWClass", None)
            if exp:
                break
            time.sleep(0.05)
    panel = u.FindWindowW("WinVClipPanelUI", None)
    if not panel:
        raise SystemExit("start winvclip.exe first")
    if u.IsWindowVisible(panel):
        win_v(); time.sleep(0.4)

    ok_all = True
    for rnd in range(1, 4):
        u.SetForegroundWindow(exp); time.sleep(0.4)
        combo(0x11, 0x4C)                    # Ctrl+L 聚焦地址栏
        time.sleep(0.35)
        f0 = focus_of(exp)
        uni('Z'); uni('Z'); uni('Z')
        time.sleep(0.25)

        win_v(); time.sleep(0.7)
        esc(); time.sleep(0.7)
        f1 = focus_of(exp)

        set_clip("SENTINEL")
        time.sleep(0.15)
        uni('A'); uni('A'); uni('A')
        time.sleep(0.3)
        combo(0x11, 0x41)                    # Ctrl+A
        time.sleep(0.15)
        combo(0x11, 0x43)                    # Ctrl+C
        time.sleep(0.4)
        got = get_clip()
        ok = got is not None and "AAA" in got
        if not ok:
            ok_all = False
        print("round %d: 唤起前 focus=0x%X -> 关闭后 focus=0x%X | 关闭后继续输入得到 %r -> %s"
              % (rnd, f0 or 0, f1 or 0, got, "OK" if ok else "FAIL"))
        # 收尾：清掉地址栏里输入的东西
        combo(0x11, 0x41); key(0x2E); key(0x2E, True)
        time.sleep(0.3)
    print("ALL OK" if ok_all else "有失败")


main()
