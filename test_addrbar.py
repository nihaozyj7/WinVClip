# -*- coding: utf-8 -*-
"""验证：在资源管理器地址栏里唤出面板并粘贴，内容要真的进到地址栏。
步骤：地址栏输入 ZZZ -> Win+V -> Enter 粘第一条 -> Ctrl+A/Ctrl+C 读回地址栏文本。
"""
import ctypes, subprocess, time
from ctypes import wintypes

u = ctypes.windll.user32
k = ctypes.windll.kernel32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.IsWindowVisible.argtypes = [ctypes.c_void_p]
u.GetForegroundWindow.restype = ctypes.c_void_p
u.SetForegroundWindow.argtypes = [ctypes.c_void_p]
u.OpenClipboard.argtypes = [ctypes.c_void_p]
u.SetClipboardData.argtypes = [ctypes.c_uint, ctypes.c_void_p]
u.GetClipboardData.restype = ctypes.c_void_p
k.GlobalLock.restype = ctypes.c_void_p
k.GlobalAlloc.restype = ctypes.c_void_p
k.GlobalAlloc.argtypes = [ctypes.c_uint, ctypes.c_size_t]
k.GlobalLock.argtypes = [ctypes.c_void_p]
k.GlobalUnlock.argtypes = [ctypes.c_void_p]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class _I(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _I)]


class G(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hwndActive", wintypes.HWND), ("hwndFocus", wintypes.HWND),
                ("hwndCapture", wintypes.HWND), ("hwndMenuOwner", wintypes.HWND),
                ("hwndMoveSize", wintypes.HWND), ("hwndCaret", wintypes.HWND),
                ("rcCaret", wintypes.RECT)]


def key(vk, up=False):
    i = INPUT(); i.type = 1; i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def uni(ch):
    i = INPUT(); i.type = 1; i.u.ki.wVk = 0; i.u.ki.wScan = ord(ch)
    i.u.ki.dwFlags = 0x0004                     # KEYEVENTF_UNICODE
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))
    i = INPUT(); i.type = 1; i.u.ki.wVk = 0; i.u.ki.wScan = ord(ch)
    i.u.ki.dwFlags = 0x0004 | 0x0002
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def combo(mod, vk):
    key(mod); time.sleep(0.03); key(vk); time.sleep(0.03)
    key(vk, True); time.sleep(0.03); key(mod, True)


def winv():
    key(0x5B); time.sleep(0.03); key(0x56); time.sleep(0.03)
    key(0x56, True); time.sleep(0.03); key(0x5B, True)


def set_clip(text):
    u.OpenClipboard(None)
    u.EmptyClipboard()
    n = (len(text) + 1) * 2
    h = k.GlobalAlloc(0x0042, n)
    p = k.GlobalLock(h)
    ctypes.memmove(p, text.encode("utf-16-le") + b"\0\0", n)
    k.GlobalUnlock(h)
    u.SetClipboardData(13, h)
    u.CloseClipboard()


def get_clip():
    if not u.OpenClipboard(None):
        return None
    try:
        h = u.GetClipboardData(13)
        if not h:
            return None
        p = k.GlobalLock(h)
        try:
            return ctypes.wstring_at(p)
        finally:
            k.GlobalUnlock(h)
    finally:
        u.CloseClipboard()


def focus_of(hwnd):
    tid = u.GetWindowThreadProcessId(hwnd, None)
    g = G(); g.cbSize = ctypes.sizeof(g)
    u.GetGUIThreadInfo(tid, ctypes.byref(g))
    return g.hwndFocus


def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, b, 256)
    return b.value


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
        winv(); time.sleep(0.4)

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

        winv()
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
            winv(); time.sleep(0.3)
    print("ALL OK" if ok_all else "有失败")


main()
