# -*- coding: utf-8 -*-
"""验证：唤出面板再关掉之后，资源管理器地址栏的焦点要回来，能继续打字。
做法：地址栏输入 ZZZ -> Win+V -> Esc -> 输入 AAA -> Ctrl+A/Ctrl+C 读回。"""
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
u.GetClipboardData.argtypes = [wintypes.UINT]
k.GlobalAlloc.restype = ctypes.c_void_p
k.GlobalAlloc.argtypes = [ctypes.c_uint, ctypes.c_size_t]
k.GlobalLock.restype = ctypes.c_void_p
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
    for up in (False, True):
        i = INPUT(); i.type = 1; i.u.ki.wVk = 0; i.u.ki.wScan = ord(ch)
        i.u.ki.dwFlags = 0x0004 | (0x0002 if up else 0)
        u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def combo(mod, vk):
    key(mod); time.sleep(0.03); key(vk); time.sleep(0.03)
    key(vk, True); time.sleep(0.03); key(mod, True)


def winv():
    key(0x5B); time.sleep(0.03); key(0x56); time.sleep(0.03)
    key(0x56, True); time.sleep(0.03); key(0x5B, True)


def esc():
    key(0x1B); time.sleep(0.03); key(0x1B, True)


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

    ok_all = True
    for rnd in range(1, 4):
        u.SetForegroundWindow(exp); time.sleep(0.4)
        combo(0x11, 0x4C)                    # Ctrl+L 聚焦地址栏
        time.sleep(0.35)
        f0 = focus_of(exp)
        uni('Z'); uni('Z'); uni('Z')
        time.sleep(0.25)

        winv(); time.sleep(0.7)
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
