# -*- coding: utf-8 -*-
"""独立进程装 WH_KEYBOARD_LL，把收到的按键记进日志。

用途：验证「面板显示期间，排在钩子链后面的其它软件的全局键盘钩子还能不能收到键」。
低级键盘钩子按 LIFO 调用（最后安装的最先调用），所以要**先于 winvclip 启动**，
它才排在 winvclip 的钩子后面——正是用户机器上「别家软件开机比面板先装钩子」的情形。

用法：python hook_listen.py <日志路径> [监听秒数]
"""
import ctypes, sys, time
from ctypes import wintypes

u = ctypes.WinDLL('user32', use_last_error=True)
LRESULT = ctypes.c_ssize_t
HOOKPROC = ctypes.WINFUNCTYPE(LRESULT, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM)

MINE = 0x57494E56   # winvclip 注入键的 dwExtraInfo 魔数


class KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("vkCode", wintypes.DWORD), ("scanCode", wintypes.DWORD),
                ("flags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


path = sys.argv[1]
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
log = open(path, "a", encoding="utf-8", buffering=1)


@HOOKPROC
def proc(nCode, wp, lp):
    if nCode == 0:
        kb = ctypes.cast(lp, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents
        if wp in (0x100, 0x104, 0x101, 0x105):
            log.write("%s vk=0x%02X%s\n" % (
                "DOWN" if wp in (0x100, 0x104) else "UP",
                kb.vkCode, " mine" if kb.dwExtraInfo == MINE else ""))
    return u.CallNextHookEx(None, nCode, wp, lp)


u.SetWindowsHookExW.restype = ctypes.c_void_p
u.SetWindowsHookExW.argtypes = [ctypes.c_int, HOOKPROC, ctypes.c_void_p, wintypes.DWORD]
u.CallNextHookEx.argtypes = [ctypes.c_void_p, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM]
u.CallNextHookEx.restype = LRESULT
hhk = u.SetWindowsHookExW(13, proc, None, 0)   # 13 = WH_KEYBOARD_LL
log.write("# hook %s\n" % ("installed" if hhk else "FAILED"))

msg = wintypes.MSG()
end = time.time() + dur
while time.time() < end:
    if u.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
        u.TranslateMessage(ctypes.byref(msg))
        u.DispatchMessageW(ctypes.byref(msg))
    time.sleep(0.004)

u.UnhookWindowsHookEx(ctypes.c_void_p(hhk))
log.write("# removed\n")
