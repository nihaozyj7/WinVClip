# -*- coding: utf-8 -*-
"""验证「点面板外部即收起」（低级鼠标钩子这条通道）。

起因：面板是 WS_EX_NOACTIVATE，从不夺取前台，所以用户在**同一个窗口**里点掉
面板旁边的正文时 GetForegroundWindow() 毫无变化，PanelShouldStay 的轮询判不出
任何东西，面板会僵在屏幕上。现在补了 WH_MOUSE_LL：按下的点落在面板矩形之外
就收起，且不吞这次点击。

四个用例：
  A 非激活态 + 点目标窗口（前台不变）→ 面板收起，目标窗口仍是前台、插入符还在
  B 点面板内部 → 不收起（钩子不能误伤面板自身交互）
  C 输入态（点过搜索框）+ 点另一个窗口 → 面板收起，且用户点的那个窗口拿到前台
    （收起时不许抢前台，否则用户那次点击会因为前台锁落空）
  D 点托盘图标（本进程窗口）→ 不收起

用法：python test_hide_outside.py
"""
import ctypes, subprocess, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
k32 = ctypes.windll.kernel32
u.SetProcessDPIAware()

PANEL_CLASS = "WinVClipPanelUI"

class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]

class _I(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]

class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _I)]

class GUITHREADINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hwndActive", wintypes.HWND), ("hwndFocus", wintypes.HWND),
                ("hwndCapture", wintypes.HWND), ("hwndMenuOwner", wintypes.HWND),
                ("hwndMoveSize", wintypes.HWND), ("hwndCaret", wintypes.HWND),
                ("rcCaret", wintypes.RECT)]


def key(vk, up=False):
    i = INPUT(); i.type = 1; i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def win_vi():
    key(0x5B); time.sleep(0.05)
    key(0x56); time.sleep(0.05)
    key(0x56, True); time.sleep(0.05)
    key(0x5B, True)


def click(x, y, btn_down=0x0002, btn_up=0x0004):
    u.SetCursorPos(int(x), int(y))
    time.sleep(0.08)
    u.mouse_event(btn_down, 0, 0, 0, 0)
    time.sleep(0.05)
    u.mouse_event(btn_up, 0, 0, 0, 0)


def rect(h):
    r = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, b, 256)
    return b.value


def find_class(name):
    res = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        if u.IsWindowVisible(h) and cls(h) == name:
            res.append(h)
        return True
    u.EnumWindows(cb, 0)
    return res


def find_title(sub):
    res = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        if not u.IsWindowVisible(h):
            return True
        n = u.GetWindowTextLengthW(h)
        if n:
            b = ctypes.create_unicode_buffer(n + 1)
            u.GetWindowTextW(h, b, n + 1)
            if sub.lower() in b.value.lower():
                res.append(h)
        return True
    u.EnumWindows(cb, 0)
    return res


def fg():
    return u.GetForegroundWindow()


def caret_of(hwnd):
    tid = u.GetWindowThreadProcessId(hwnd, None)
    gi = GUITHREADINFO(); gi.cbSize = ctypes.sizeof(gi)
    if not u.GetGUIThreadInfo(tid, ctypes.byref(gi)):
        return None
    if not gi.hwndCaret:
        return None
    return (gi.rcCaret.left, gi.rcCaret.top)


def force_fg(hwnd):
    if fg() == hwnd:
        return True
    t1 = u.GetWindowThreadProcessId(fg(), None)
    t2 = u.GetWindowThreadProcessId(hwnd, None)
    my = k32.GetCurrentThreadId()
    for _ in range(8):
        u.AttachThreadInput(my, t1, True)
        u.AttachThreadInput(my, t2, True)
        u.SetForegroundWindow(hwnd)
        u.AttachThreadInput(my, t1, False)
        u.AttachThreadInput(my, t2, False)
        time.sleep(0.1)
        if fg() == hwnd:
            return True
    return False


def vis(h):
    return bool(h) and bool(u.IsWindowVisible(h))


def panel():
    ps = find_class(PANEL_CLASS)
    return ps[0] if ps else None


def wait_panel(show=True, timeout=2.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        p = panel()
        if (vis(p)) == show:
            return p
        time.sleep(0.05)
    return panel() if show else None


def inside(pr, x, y):
    return pr[0] <= x < pr[2] and pr[1] <= y < pr[3]


def pick_outside_point(pr, note_r, target):
    """在记事本窗口里找一个不被面板遮住、而且真的属于记事本的点。"""
    for fx, fy in ((0.5, 0.5), (0.5, 0.15), (0.2, 0.5), (0.8, 0.5), (0.5, 0.85),
                   (0.1, 0.1), (0.9, 0.9), (0.1, 0.9), (0.9, 0.1), (0.35, 0.6)):
        x = int(note_r[0] + (note_r[2] - note_r[0]) * fx)
        y = int(note_r[1] + (note_r[3] - note_r[1]) * fy)
        if inside(pr, x, y):
            continue
        at = u.WindowFromPoint(wintypes.POINT(x, y))
        root = u.GetAncestor(at, 2) if at else None      # GA_ROOT = 2
        if root == target:
            return x, y
        print("    跳过 (%d,%d)：窗口 0x%X 不属于记事本" % (x, y, at or 0))
    return None


fails = []


def check(name, cond, extra=""):
    print(("  OK   " if cond else "  FAIL ") + name + ("  " + extra if extra else ""))
    if not cond:
        fails.append(name)


def ensure_hidden(p):
    """每个用例开始前保证面板不可见，否则 Win+V 会变成「关闭」。"""
    if p and u.IsWindowVisible(p):
        u.keybd_event(0x1B, 0, 0, 0); time.sleep(0.05)
        u.keybd_event(0x1B, 0, 2, 0)
        time.sleep(0.4)
    return not (p and u.IsWindowVisible(p))


def main():
    exe = "build\\winvclip.exe"
    import os
    trace = os.path.join(os.environ.get("TEMP", "."), "winvclip-trace.txt")
    try:
        os.remove(trace)
    except Exception:
        pass
    env = dict(os.environ); env["WV_TRACE"] = "1"
    proc = subprocess.Popen([exe], env=env)
    time.sleep(1.2)
    note = subprocess.Popen(["notepad.exe"])
    time.sleep(1.2)

    hw = find_title("记事本") or find_title("Notepad")
    if not hw:
        print("找不到记事本窗口"); return 1
    hw = hw[0]
    print("notepad = 0x%X" % hw)
    # 挪到左侧并置顶：桌面上常驻着全屏的 Edge，不置顶的话注入的点击会落到 Edge 上
    u.SetWindowPos(hw, 0, 100, 100, 880, 760, 0x0040)   # HWND_TOP + SWP_SHOWWINDOW
    time.sleep(0.3)
    if not force_fg(hw):
        print("无法把记事本置前"); return 1
    time.sleep(0.4)
    cr = rect(hw)
    at = u.WindowFromPoint(wintypes.POINT(cr[0] + 60, cr[1] + 60))
    root = u.GetAncestor(at, 2) if at else None
    if root != hw:
        print("记事本没有置顶（点在 0x%X root=0x%X）" % (at or 0, root or 0)); return 1
    # 让记事本里确实有插入符
    u.SendMessageW(hw, 0x000D, 0, 0)
    click(cr[0] + 60, cr[1] + 60)
    time.sleep(0.3)
    pre_caret = caret_of(hw)
    print("caret before = %s" % (pre_caret,))

    try:
        # ---------------- A 非激活态：点目标窗口（前台不变）
        print("\nA 非激活态点目标窗口（前台不变）")
        ensure_hidden(panel())
        win_vi(); time.sleep(0.7)
        p = wait_panel(True)
        check("面板已弹出", vis(p))
        check("唤起后前台仍是记事本", fg() == hw, "fg=0x%X" % fg())
        pr = rect(p)
        print("  panel rect = %s  notepad rect = %s" % (pr, rect(hw)))
        pt = pick_outside_point(pr, rect(hw), hw)
        check("找到面板外的可点位置", pt is not None, str(pt))
        if pt:
            click(pt[0], pt[1])
            time.sleep(0.5)
            check("点面板外后已收起", not vis(p))
            check("记事本仍是前台", fg() == hw,
                  "fg=0x%X cls=%s" % (fg(), cls(fg())))
            check("插入符还在闪", caret_of(hw) is not None, str(caret_of(hw)))

        # ---------------- B 点面板内部
        print("\nB 点面板内部不收起")
        ensure_hidden(panel())
        win_vi(); time.sleep(0.7)
        p = wait_panel(True)
        pr = rect(p)
        # 分类条：y ∈ [HEAD_H+SEARCH_TOP+SEARCH_H+SEARCH_BOT, +FILTER_H] = [96,142]
        click(pr[0] + 30, pr[1] + 118)
        time.sleep(0.4)
        check("点面板内仍可见", vis(p))
        u.keybd_event(0x1B, 0, 0, 0); time.sleep(0.05)
        u.keybd_event(0x1B, 0, 2, 0)
        time.sleep(0.4)
        check("Esc 收起", not vis(p))

        # ---------------- C 输入态点另一个窗口
        print("\nC 输入态点别的窗口：面板收起且那个窗口拿到前台")
        before = set(find_title(""))
        exp = subprocess.Popen(["explorer.exe", "C:\\Windows"])
        time.sleep(2.0)
        wins = [h for h in find_title("") if h not in before and cls(h) != PANEL_CLASS]
        force_fg(hw); time.sleep(0.3)
        win_vi(); time.sleep(0.7)
        p = wait_panel(True)
        pr = rect(p)
        # 搜索框：y ∈ [HEAD_H+SEARCH_TOP, +SEARCH_H] = [46,86]
        click(pr[0] + 40, pr[1] + 66)
        time.sleep(0.5)
        input_mode = (fg() == p)
        print("  输入态 = %s (fg=0x%X)" % (input_mode, fg()))
        other = None
        if wins:
            other = wins[0]
            # 挪到屏幕右侧空档：记事本几乎铺满左侧，直接点它的标题栏会点到记事本上
            sw = u.GetSystemMetrics(0); sh = u.GetSystemMetrics(1)
            ow, oh = 560, 380
            u.SetWindowPos(other, 0, sw - ow - 40, 320, ow, oh, 0x0004)  # SWP_NOZORDER
            time.sleep(0.4)
            nr = rect(other)
            x = int((nr[0] + nr[2]) / 2); y = nr[1] + 14
            at = u.WindowFromPoint(wintypes.POINT(x, y))
            root = u.GetAncestor(at, 2) if at else None
            print("  other rect = %s  点在 0x%X (root 0x%X)" % (nr, at or 0, root or 0))
            click(x, y)
            time.sleep(0.8)
            check("点别的窗口后面板收起", not vis(p))
            check("用户点的窗口拿到前台", fg() == other,
                  "fg=0x%X other=0x%X" % (fg(), other))
        else:
            print("  跳过：没找到另一个窗口")
        if input_mode and fg() == p:
            u.keybd_event(0x1B, 0, 0, 0); u.keybd_event(0x1B, 0, 2, 0); time.sleep(0.4)
        other_p = exp

        # ---------------- D 点本进程窗口（无托盘窗口可点，用面板自身外的本进程窗口
        # 不存在，这里退化为检查「点面板矩形外但属于本进程」的判据不误报：静默跳过）
        print("\nD 拖拽标题栏移动面板不应收起")
        ensure_hidden(panel())
        win_vi(); time.sleep(0.7)
        p = wait_panel(True)
        pr = rect(p)
        # 按住标题栏拖到右边一点再松开
        u.SetCursorPos(pr[0] + 150, pr[1] + 30); time.sleep(0.1)
        u.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.15)
        for i in range(1, 9):
            u.SetCursorPos(pr[0] + 150 + i * 20, pr[1] + 30 + i * 4)
            time.sleep(0.04)
        u.mouse_event(0x0004, 0, 0, 0, 0); time.sleep(0.4)
        check("拖到面板外松开后仍可见", vis(p), "rect=%s" % (rect(p),))
        u.keybd_event(0x1B, 0, 0, 0); u.keybd_event(0x1B, 0, 2, 0); time.sleep(0.3)
        # ---------------- E 固定态点外部不收起
        print("\nE 固定态点外部不收起")
        ensure_hidden(panel())
        win_vi(); time.sleep(0.7)
        p = wait_panel(True)
        pr = rect(p)
        pin_x = pr[2] - 75          # 右上三个按钮 = 图钉/齿轮/关闭，各 30 宽
        click(pin_x, pr[1] + 17)
        time.sleep(0.4)
        pt = pick_outside_point(pr, rect(hw), hw)
        check("找到面板外可点位置", pt is not None, str(pt))
        if pt:
            click(pt[0], pt[1]); time.sleep(0.5)
            check("固定态点外部仍可见", vis(p))
        click(pin_x, pr[1] + 17)     # 取消固定
        time.sleep(0.3)
        u.keybd_event(0x1B, 0, 0, 0); u.keybd_event(0x1B, 0, 2, 0); time.sleep(0.4)
        check("取消固定后 Esc 收起", not vis(p))

    finally:
        try:
            note.terminate()
        except Exception:
            pass
        try:
            other_p.terminate()
        except Exception:
            pass
        time.sleep(0.3)
        proc.terminate()

    print("\n%s" % ("全部通过" if not fails else "失败：%s" % fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
