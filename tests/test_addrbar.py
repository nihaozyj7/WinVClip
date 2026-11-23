# -*- coding: utf-8 -*-
"""验证：在资源管理器地址栏里唤出面板并粘贴，内容要真的进到地址栏。
步骤：地址栏输入 ZZZ -> Win+V -> Enter 粘第一条 -> Ctrl+A/Ctrl+C 读回地址栏文本。
"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板


class G(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hwndActive", wintypes.HWND), ("hwndFocus", wintypes.HWND),
                ("hwndCapture", wintypes.HWND), ("hwndMenuOwner", wintypes.HWND),
                ("hwndMoveSize", wintypes.HWND), ("hwndCaret", wintypes.HWND),
                ("rcCaret", wintypes.RECT)]


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

    payload = "PASTEFROMHISTORY"
    set_clip(payload)
    time.sleep(0.8)          # 让程序把它记进历史

    ok_all = True
    for rnd in range(1, 4):
        # 置前 + Ctrl+L 聚焦地址栏，两者都可能静默失败（前台锁 / 窗口刚就绪），
        # 失败就要重试——否则键盘输入会落在这个窗口的其他部位，
        # 读回的「地址栏内容」其实是整窗文本，看起来像功能坏了。
        for attempt in range(8):
            u.SetForegroundWindow(exp)
            time.sleep(0.3)
            if u.GetForegroundWindow() != exp:
                continue
            combo(0x11, 0x4C)
            time.sleep(0.35)
            if cls(focus_of(exp) or 0) in ("Edit", "RichEditD2DPT", "DirectUIHWND",
                                           "InputSiteWindowClass"):
                break
        f0 = focus_of(exp)
        uni('Z'); uni('Z'); uni('Z')
        time.sleep(0.3)
        print("round %d: 地址栏 focus=0x%X (%s), 输入 ZZZ" % (rnd, f0 or 0, cls(f0 or 0)))

        win_v()
        time.sleep(0.8)
        vis = bool(u.IsWindowVisible(panel))
        if not vis:
            print("    !! 面板没弹出")
            ok_all = False
            continue
        f1 = focus_of(exp)
        key(0x0D); time.sleep(0.03); key(0x0D, True)   # Enter 粘贴第一条
        time.sleep(1.0)

        # 读回地址栏内容
        combo(0x11, 0x41)    # Ctrl+A
        time.sleep(0.15)
        combo(0x11, 0x43)    # Ctrl+C
        time.sleep(0.4)
        got = get_clip()
        ok = got is not None and payload in got
        if not ok:
            ok_all = False
        print("    面板=%s 粘贴前 explorer focus=0x%X | 地址栏内容=%r -> %s" %
              (vis, f1 or 0, got, "OK" if ok else "FAIL"))
        set_clip(payload)
        time.sleep(0.6)
        if u.IsWindowVisible(panel):
            win_v(); time.sleep(0.3)
    print("ALL OK" if ok_all else "有失败")


main()
