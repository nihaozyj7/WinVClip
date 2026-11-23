# -*- coding: utf-8 -*-
"""面板显示时不许把别家软件的全局键盘钩子饿死。

复现顺序是关键：先启动 hook_listen.py（装 WH_KEYBOARD_LL），再启动 winvclip。
低级键盘钩子是 LIFO，winvclip 的钩子因此排在监听之后被系统调用——它 return 1
的键，监听就收不到，正好等于用户机器上「别家软件先开机装钩子、面板后装」。

用例：
A 非激活态（默认，前台仍是记事本）
  · Ctrl+Shift+Q / F5 → 监听必须收到（面板不消费，放行）
  · 裸字母 a          → 监听收不到（面板要拿它过滤搜索），但 trace 里应有 'a'
B 输入态（点搜索框，面板真前台）：q / F5 直接投递给面板，监听仍应收到
C 非激活态 Esc → 监听收不到（面板消费），面板收起

用法：python test_hook_passthrough.py
"""
# --- 路径基准：从 tests/ 回到项目根（P0 结构整理时注入） ---
import os as _os, sys as _sys
_os.chdir(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
k32 = ctypes.windll.kernel32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # 项目根（P0 后脚本位于 tests/）
EXE = os.path.join(HERE, "build", "winvclip.exe")
TMP = os.environ.get("TEMP", ".")
HLOG = os.path.join(TMP, "hooklog.txt")
TLOG = os.path.join(TMP, "winvclip-trace.txt")


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


def tap(vk):
    key(vk); time.sleep(0.05); key(vk, True); time.sleep(0.05)


def chord(mods, vk):
    for m in mods:
        key(m); time.sleep(0.03)
    tap(vk)
    for m in reversed(mods):
        key(m, True); time.sleep(0.03)


def win_vi():
    key(0x5B); time.sleep(0.06); key(0x56); time.sleep(0.06)
    key(0x56, True); time.sleep(0.06); key(0x5B, True); time.sleep(0.5)


def cls(h):
    b = ctypes.create_unicode_buffer(256); u.GetClassNameW(h, b, 256); return b.value


def click_panel(panel, x, y):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(panel, 0x0201, 1, lp)
    u.PostMessageW(panel, 0x0202, 0, lp)


def force_fg(hwnd):
    """跨进程 SetForegroundWindow 常被前台锁静默拒绝，这里绕过前台锁。"""
    if u.GetForegroundWindow() == hwnd:
        return True
    fg = u.GetForegroundWindow()
    t1 = u.GetWindowThreadProcessId(fg, None)
    t2 = u.GetWindowThreadProcessId(hwnd, None)
    my = k32.GetCurrentThreadId()
    for _ in range(6):
        u.AttachThreadInput(my, t1, True)
        u.AttachThreadInput(my, t2, True)
        u.BringWindowToTop(hwnd)
        u.SetForegroundWindow(hwnd)
        u.AttachThreadInput(my, t1, False)
        u.AttachThreadInput(my, t2, False)
        time.sleep(0.12)
        if u.GetForegroundWindow() == hwnd:
            return True
    return False


def hlog_downs():
    """监听日志里所有按下事件的 vk 集合。"""
    vks = set()
    if not os.path.exists(HLOG):
        return vks
    for line in open(HLOG, encoding="utf-8"):
        if line.startswith("DOWN"):
            vks.add(int(line.split("vk=0x")[1][:2], 16))
    return vks


def trace_text():
    if not os.path.exists(TLOG):
        return ""
    return open(TLOG, encoding="utf-8", errors="replace").read()


def tally(name, ok, expect="", got=""):
    print("%-4s %s %s %s" % (name, "PASS" if ok else "FAIL", expect, got))
    return ok


def main():
    ok = True
    # 1. 先起监听（先装钩子），再起面板（后装钩子 → 排在前面先被调用）
    if os.path.exists(HLOG):
        os.remove(HLOG)
    if os.path.exists(TLOG):
        os.remove(TLOG)
    listen = subprocess.Popen([sys.executable, os.path.join(HERE, "hook_listen.py"), HLOG, "40"],
                              creationflags=0x08000000)
    time.sleep(1.2)

    subprocess.run(["taskkill", "/IM", "winvclip.exe", "/F"], capture_output=True)
    time.sleep(0.4)
    env = dict(os.environ, WV_TRACE="1")
    panel_proc = subprocess.Popen([EXE], cwd=os.path.join(HERE, "build"), env=env)
    time.sleep(1.5)

    np = subprocess.Popen(["notepad.exe"])
    nphwnd = None
    for _ in range(40):
        time.sleep(0.15)
        nphwnd = u.FindWindowW("Notepad", None)
        if nphwnd:
            break
    if not nphwnd:
        print("FAIL 记事本没起来")
        return 1
    time.sleep(0.6)

    panel = u.FindWindowW("WinVClipPanelUI", None)
    if not panel:
        print("FAIL 面板窗口不存在（winvclip 没跑起来？）")
        return 1
    if u.IsWindowVisible(panel):
        force_fg(nphwnd); tap(0x1B); time.sleep(0.4)

    # ---------- 用例 A：非激活态 ----------
    force_fg(nphwnd)
    win_vi(); time.sleep(0.5)
    print("A 面板可见=%s 非激活态=%s" % (u.IsWindowVisible(panel),
                                     u.GetForegroundWindow() == nphwnd))
    chord([0x11, 0x10], 0x51)         # Ctrl+Shift+Q
    tap(0x74)                          # F5
    time.sleep(0.2)
    tap(0x41)                          # 裸字母 a：面板要消费它
    time.sleep(0.6)
    d = hlog_downs()
    ok &= tally("A1", 0x51 in d, "Ctrl+Shift+Q 监听收到", "got=%s" % sorted(hex(x) for x in d))
    ok &= tally("A2", 0x74 in d, "F5 监听收到", "")
    ok &= tally("A3", 0x41 not in d, "裸字母 a 监听收不到（面板消费）", "")
    ok &= tally("A4", "panel char 'a'" in trace_text(), "面板确实收到了 a", "")

    # ---------- 用例 B：输入态 ----------
    click_panel(panel, 120, 60)        # 点搜索框 → 面板拿到前台
    time.sleep(0.4)
    input_mode = (u.GetForegroundWindow() == panel)
    print("B  输入态=%s (前台 cls=%s)" % (input_mode, cls(u.GetForegroundWindow())))
    before = hlog_downs()
    tap(0x71)                          # q
    chord([0x10], 0x74)                # Shift+F5（随便一个组合，只看有没有被饿死）
    time.sleep(0.5)
    d2 = hlog_downs()
    ok &= tally("B1", 0x71 in d2, "输入态 q 监听收到", "")
    ok &= tally("B2", 0x74 in d2, "输入态 F5 监听收到", "")

    # ---------- 用例 C：非激活态 Esc ----------
    tap(0x1B)                          # 先退出输入态（输入态 Esc 直达面板）
    time.sleep(0.4)
    if not u.IsWindowVisible(panel):
        win_vi(); time.sleep(0.5)
    force_fg(nphwnd)
    time.sleep(0.3)
    ok &= tally("C0", bool(u.IsWindowVisible(panel)) and u.GetForegroundWindow() == nphwnd,
                "面板可见且非激活", "")
    d3_before = hlog_downs()
    tap(0x1B)
    time.sleep(0.5)
    ok &= tally("C1", not u.IsWindowVisible(panel), "Esc 后面板收起", "")
    # 只比较这一段新增的键：前面退出输入态的那次 Esc 是直达面板的，会进日志
    ok &= tally("C2", 0x1B not in (hlog_downs() - d3_before), "Esc 监听收不到（面板消费）", "")

    print("\n结果：%s" % ("全部通过" if ok else "有失败项"))
    listen.terminate()
    np.terminate()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
