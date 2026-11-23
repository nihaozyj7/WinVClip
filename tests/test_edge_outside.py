# -*- coding: utf-8 -*-
"""用户报告的场景：在 Edge 网页里呼出面板后，点浏览器（同一个窗口内）收起。

非激活态下面板不改变前台，点「同一个浏览器窗口」的正文时 GetForegroundWindow()
纹丝不动，旧的轮询判据永远看不到变化，面板会僵在屏幕上。这里验证新加的低级
鼠标钩子通道真的把它收起来了，而且这次点击照常送到网页、浏览器仍是前台。
"""
# --- 路径基准：从 tests/ 回到项目根（P0 结构整理时注入） ---
import os as _os, sys as _sys
_os.chdir(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import ctypes, subprocess, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
u.SetProcessDPIAware()

class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]

class _I(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]

class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _I)]


def key(vk, up=False):
    i = INPUT(); i.type = 1; i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def win_vi():
    key(0x5B); time.sleep(0.05)
    key(0x56); time.sleep(0.05)
    key(0x56, True); time.sleep(0.05)
    key(0x5B, True)


def click(x, y):
    u.SetCursorPos(int(x), int(y)); time.sleep(0.08)
    u.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.05)
    u.mouse_event(0x0004, 0, 0, 0, 0)


def rect(h):
    r = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, b, 256)
    return b.value


def find_panel():
    res = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        if u.IsWindowVisible(h) and cls(h) == "WinVClipPanelUI":
            res.append(h)
        return True
    u.EnumWindows(cb, 0)
    return res[0] if res else None


def fg():
    return u.GetForegroundWindow()


def force_fg(hwnd):
    if fg() == hwnd:
        return True
    fg0 = fg()
    t1 = u.GetWindowThreadProcessId(fg0, None)
    t2 = u.GetWindowThreadProcessId(hwnd, None)
    my = ctypes.windll.kernel32.GetCurrentThreadId()
    for _ in range(8):
        u.AttachThreadInput(my, t1, True); u.AttachThreadInput(my, t2, True)
        u.SetForegroundWindow(hwnd)
        u.AttachThreadInput(my, t1, False); u.AttachThreadInput(my, t2, False)
        time.sleep(0.1)
        if fg() == hwnd:
            return True
    return False


def inside(r, x, y):
    return r[0] <= x < r[2] and r[1] <= y < r[3]


def main():
    edge = 0x0094049C
    if not u.IsWindow(edge):
        print("Edge 窗口不在，先跑一次再试"); return 1
    proc = subprocess.Popen(["build\\winvclip.exe"])
    time.sleep(1.2)
    if not force_fg(edge):
        print("无法把 Edge 置前"); return 1
    time.sleep(0.5)

    fails = []
    # 先点一下网页正文，让页面里有焦点（也是用户实际的用法）
    er = rect(edge)
    click(int((er[0] + er[2]) / 2), int((er[1] + er[3]) / 2))
    time.sleep(0.5)
    if fg() != edge:
        print("Edge 未持有前台(0x%X)" % fg()); return 1

    win_vi(); time.sleep(0.8)
    p = find_panel()
    if not u.IsWindowVisible(p or 0):
        print("面板没弹出"); return 1
    pr = rect(p)
    print("edge rect = %s\npanel rect = %s" % (er, pr))
    if fg() != edge:
        fails.append("唤起后 Edge 仍是前台")

    # 在网页里找一个面板挡不到、且确实属于 Edge 的点
    pt = None
    for fx, fy in ((0.5, 0.75), (0.75, 0.5), (0.5, 0.15), (0.2, 0.7), (0.85, 0.8)):
        x = int(er[0] + (er[2] - er[0]) * fx); y = int(er[1] + (er[3] - er[1]) * fy)
        if inside(pr, x, y):
            continue
        at = u.WindowFromPoint(wintypes.POINT(x, y))
        root = u.GetAncestor(at, 2) if at else None
        if root == edge:
            pt = (x, y); break
    print("点 %s" % (pt,))
    if not pt:
        print("找不到可点的网页区域"); proc.terminate(); return 1

    click(pt[0], pt[1])
    time.sleep(0.6)
    ok_hide = not u.IsWindowVisible(p)
    ok_fg = (fg() == edge)
    print("  %s 点网页正文后面板收起" % ("OK  " if ok_hide else "FAIL"))
    print("  %s Edge 仍是前台 (fg=0x%X)" % ("OK  " if ok_fg else "FAIL", fg()))
    if not ok_hide: fails.append("点网页后面板没收起")
    if not ok_fg: fails.append("Edge 失去前台")

    # 再验一次「点击真的送到了网页」：面板收起后再按一次 Win+V，面板应能再次弹出
    win_vi(); time.sleep(0.8)
    p2 = find_panel()
    ok_again = bool(u.IsWindowVisible(p2 or 0))
    print("  %s 收起后能再次唤起" % ("OK  " if ok_again else "FAIL"))
    if not ok_again: fails.append("收起后唤不起来")
    if ok_again:
        u.keybd_event(0x1B, 0, 0, 0); time.sleep(0.05)
        u.keybd_event(0x1B, 0, 2, 0); time.sleep(0.3)

    proc.terminate()
    print("\n%s" % ("全部通过" if not fails else "失败：%s" % fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
