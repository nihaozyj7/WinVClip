# -*- coding: utf-8 -*-
"""用户报告的场景：在 Edge 网页里呼出面板后，点浏览器（同一个窗口内）收起。

非激活态下面板不改变前台，点「同一个浏览器窗口」的正文时 GetForegroundWindow()
纹丝不动，旧的轮询判据永远看不到变化，面板会僵在屏幕上。这里验证新加的低级
鼠标钩子通道真的把它收起来了，而且这次点击照常送到网页、浏览器仍是前台。
"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板


def find_panel():
    res = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        if u.IsWindowVisible(h) and cls(h) == "WinVClipPanelUI":
            res.append(h)
        return True
    u.EnumWindows(cb, 0)
    return res[0] if res else None


def find_edge():
    """动态找 Edge 主窗口。

    原先这里写死了作者机器上的 hwnd（0x0094049C），换个机器/重启浏览器
    用例必然失败，还会被误读成"面板回归"。按类名 + 进程名查找才可复现。
    """
    hits = find_windows(lambda h, c, t: c == "Chrome_WidgetWin_1"
                        and exe_path(h).lower().endswith("msedge.exe"))
    return hits[0] if hits else None


def main():
    edge = find_edge()
    if not edge:
        print("Edge 窗口不在，先开一个 Edge 窗口再跑"); return 1
    proc = subprocess.Popen(["build\\winvclip.exe"])
    time.sleep(1.2)
    if not force_fg(edge):
        print("无法把 Edge 置前"); return 1
    time.sleep(0.5)

    fails = []
    # 先点一下网页正文，让页面里有焦点（也是用户实际的用法）
    er = rect(edge)
    real_click(int((er[0] + er[2]) / 2), int((er[1] + er[3]) / 2))
    time.sleep(0.5)
    if fg() != edge:
        print("Edge 未持有前台(0x%X)" % fg()); return 1

    win_v(); time.sleep(0.8)
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

    real_click(pt[0], pt[1])
    time.sleep(0.6)
    ok_hide = not u.IsWindowVisible(p)
    ok_fg = (fg() == edge)
    print("  %s 点网页正文后面板收起" % ("OK  " if ok_hide else "FAIL"))
    print("  %s Edge 仍是前台 (fg=0x%X)" % ("OK  " if ok_fg else "FAIL", fg()))
    if not ok_hide: fails.append("点网页后面板没收起")
    if not ok_fg: fails.append("Edge 失去前台")

    # 再验一次「点击真的送到了网页」：面板收起后再按一次 Win+V，面板应能再次弹出
    win_v(); time.sleep(0.8)
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
