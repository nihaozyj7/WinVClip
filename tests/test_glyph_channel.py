# -*- coding: utf-8 -*-
"""回归：表情 / 符号的两条插入通道。

  - 有文本插入符的目标（记事本、Edit/RichEdit/Office）：KEYEVENTF_UNICODE 直插，剪贴板全程不动；
  - 取不到插入符的目标（Chromium/Electron 页面，插入符在渲染进程里）：借剪贴板 + Ctrl+V，
    粘完把原剪贴板还原，且借用过程不进历史；
  - 第三个参数 shift：按住 Shift 点格子，任何目标都强制走粘贴通道（注入无效时的手动兜底）。

期望：目标拿到 emoji；`clip after insert` 仍是哨兵 ORIGINAL-ABC。
用法: python test_glyph_channel.py notepad|web|explorer [shift]
"""
# --- 路径基准：从 tests/ 回到项目根（P0 结构整理时注入） ---
import os as _os, sys as _sys
_os.chdir(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import ctypes, subprocess, sys, time, urllib.parse
from ctypes import wintypes

u = ctypes.windll.user32
k = ctypes.windll.kernel32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.GetForegroundWindow.restype = ctypes.c_void_p
u.GetClipboardData.restype = ctypes.c_void_p
u.GetClipboardData.argtypes = [wintypes.UINT]
u.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
k.GlobalAlloc.restype = ctypes.c_void_p
k.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
k.GlobalLock.restype = ctypes.c_void_p
k.GlobalLock.argtypes = [ctypes.c_void_p]
k.GlobalUnlock.argtypes = [ctypes.c_void_p]

TAG = "WINVCLIP-CLIP"


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class _I(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _I)]


def key(vk, up=False):
    i = INPUT(); i.type = 1
    i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def tap(vk):
    key(vk); time.sleep(0.03); key(vk, True); time.sleep(0.03)


def combo(mod, vk):
    key(mod); time.sleep(0.03); tap(vk); key(mod, True); time.sleep(0.06)


def winv():
    key(0x5B); time.sleep(0.06); tap(0x56); time.sleep(0.06); key(0x5B, True)
    time.sleep(0.9)


def panel():
    for _ in range(60):
        h = u.FindWindowW("WinVClipPanelUI", None)
        if h:
            return h
        time.sleep(0.05)
    raise SystemExit("panel not created")


def vis(h):
    return bool(u.IsWindowVisible(h))


def click(h, x, y):
    lp = ((y & 0xFFFF) << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0200, 0, lp)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


def get_clip():
    for _ in range(20):
        if u.OpenClipboard(None):
            break
        time.sleep(0.03)
    try:
        h = u.GetClipboardData(13)
        if not h:
            return ""
        p = k.GlobalLock(h); s = ctypes.wstring_at(p); k.GlobalUnlock(h)
        return s
    finally:
        u.CloseClipboard()


def set_clip(s):
    u.OpenClipboard(None); u.EmptyClipboard()
    d = (s + "\0").encode("utf-16-le")
    h = k.GlobalAlloc(0x0042, len(d))
    p = k.GlobalLock(h); ctypes.memmove(p, d, len(d)); k.GlobalUnlock(h)
    u.SetClipboardData(13, h); u.CloseClipboard()


def force_fg(hwnd):
    if u.GetForegroundWindow() == hwnd:
        return True
    fg = u.GetForegroundWindow()
    tid = u.GetWindowThreadProcessId(fg, None)
    my = k.GetCurrentThreadId()
    u.AttachThreadInput(my, tid, True)
    u.BringWindowToTop(hwnd); u.SetForegroundWindow(hwnd)
    u.SetActiveWindow(hwnd); u.SetFocus(hwnd)
    u.AttachThreadInput(my, tid, False)
    for _ in range(8):
        if u.GetForegroundWindow() == hwnd:
            return True
        tap(0x12); u.SetForegroundWindow(hwnd); time.sleep(0.05)
    return u.GetForegroundWindow() == hwnd


def find_title(sub):
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, ctypes.c_void_p, wintypes.LPARAM)
    def cb(w, l):
        b = ctypes.create_unicode_buffer(256); u.GetClassNameW(w, b, 256)
        if sub == "" and b.value == "CabinetWClass":
            if u.IsWindowVisible(w):
                found.append(w)
            return True
        if b.value in ("Chrome_WidgetWin_1", "Notepad") or "Notepad" in b.value:
            if u.IsWindowVisible(w):
                n = ctypes.create_unicode_buffer(512); u.GetWindowTextW(w, n, 512)
                if sub in n.value:
                    found.append(w)
        return True

    u.EnumWindows(cb, 0)
    return found[0] if found else None


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "web"
    proc = None
    if mode == "notepad":
        proc = subprocess.Popen(["notepad.exe"]); time.sleep(1.6)
        w = u.FindWindowW(None, "无标题 - 记事本") or u.FindWindowW(None, "Untitled - Notepad")
    elif mode == "explorer":
        w = None
        for _ in range(80):
            w = find_title("")       # 见下面的 CabinetWClass 分支
            if w:
                break
            time.sleep(0.1)
        if not w:
            raise SystemExit("explorer not found")
    else:
        html = ("<html><head><title>%s</title></head><body style='margin:60px'>"
                "<textarea rows=10 cols=50 autofocus></textarea></body></html>" % TAG)
        subprocess.Popen(["cmd", "/c", "start", "", "msedge", "--new-window",
                          "data:text/html;charset=utf-8," + urllib.parse.quote(html)])
        w = None
        for _ in range(80):
            w = find_title(TAG)
            if w:
                break
            time.sleep(0.1)
    if not w:
        raise SystemExit("target window not found")
    if not force_fg(w):
        raise SystemExit("fg failed")
    time.sleep(0.8)
    if mode == "web":
        r = ctypes.wintypes.RECT(); u.GetWindowRect(w, ctypes.byref(r))
        u.SetCursorPos(r.left + 200, r.top + 200); time.sleep(0.15)
        for f in (0, 2, 4):
            u.mouse_event(f, 0, 0, 0, 0); time.sleep(0.08)
        time.sleep(0.4)
    if mode == "explorer":
        # 点一下文件视图，让焦点落在「没有插入符」的列表上（这才是要测的目标）
        r = ctypes.wintypes.RECT(); u.GetWindowRect(w, ctypes.byref(r))
        u.SetCursorPos(r.left + 400, r.top + 400); time.sleep(0.15)
        for f in (0, 2, 4):
            u.mouse_event(f, 0, 0, 0, 0); time.sleep(0.08)
        time.sleep(0.5)

    h = panel()
    if vis(h):
        winv()
    winv()
    if not vis(h):
        raise SystemExit("panel did not show")

    set_clip("ORIGINAL-ABC")
    time.sleep(0.5)
    click(h, 184, 17)          # 表情页签
    time.sleep(0.4)
    shift = len(sys.argv) > 2 and sys.argv[2] == "shift"
    if shift:
        key(0x10)              # 按住 Shift = 强制走剪贴板粘贴通道
        time.sleep(0.15)
    click(h, 40, 175)          # 第一格
    time.sleep(0.3)
    if shift:
        key(0x10, True)
    time.sleep(1.6)            # 等粘贴 + 还原定时器（600ms）跑完

    print("clip after insert:", repr(get_clip()))
    print("panel visible:", vis(h))

    force_fg(w); time.sleep(0.3)
    if mode != "explorer":       # 文件列表里 Ctrl+A 会全选文件，跳过读回
        combo(0x11, 0x41); combo(0x11, 0x43); time.sleep(0.4)
        print("target content:", repr(get_clip()))
    if proc:
        proc.terminate()


main()
