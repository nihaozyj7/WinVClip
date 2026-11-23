# -*- coding: utf-8 -*-
"""输入态（点搜索框才激活面板）验证：

1. 面板弹出后是「非激活态」：前台仍是目标窗口；
2. 点击搜索框 → 面板真正拿到前台（输入态），输入法才能工作；
3. 输入态下按 Esc 收起面板，前台还给目标窗口。

用法：python test_inputmode.py
"""
# --- 路径基准：从 tests/ 回到项目根（P0 结构整理时注入） ---
import os as _os, sys as _sys
_os.chdir(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import ctypes, time
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]

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
    key(vk); time.sleep(0.04); key(vk, True); time.sleep(0.04)

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
    """强抢前台：跨进程 SetForegroundWindow 常被前台锁静默拒绝。"""
    fg = u.GetForegroundWindow()
    if fg == hwnd:
        return True
    t1 = u.GetWindowThreadProcessId(fg, None)
    t2 = u.GetWindowThreadProcessId(hwnd, None)
    my = ctypes.windll.kernel32.GetCurrentThreadId()
    ok = False
    for _ in range(6):
        u.AttachThreadInput(my, t1, True)
        u.AttachThreadInput(my, t2, True)
        u.BringWindowToTop(hwnd)
        u.SetForegroundWindow(hwnd)
        u.AttachThreadInput(my, t1, False)
        u.AttachThreadInput(my, t2, False)
        time.sleep(0.12)
        if u.GetForegroundWindow() == hwnd:
            ok = True
            break
    return ok


hwnd = u.FindWindowW("Notepad", None)
panel = u.FindWindowW("WinVClipPanelUI", None)

# 收尾上一次测试可能留着的面板
if u.IsWindowVisible(panel):
    force_fg(hwnd)
    tap(0x1B); time.sleep(0.4)

if not force_fg(hwnd):
    print("FAIL 无法把记事本置前，当前前台 cls=%s" % cls(u.GetForegroundWindow()))
    raise SystemExit(1)

win_vi(); time.sleep(0.5)
print("面板可见=%s 非激活态(前台仍是记事本)=%s" % (u.IsWindowVisible(panel),
      u.GetForegroundWindow() == hwnd))

click_panel(panel, 120, 60)          # 搜索框
time.sleep(0.4)
fg = u.GetForegroundWindow()
print("点搜索框后 前台类名=%s 面板拿到前台=%s" % (cls(fg), fg == panel))

tap(0x1B)                             # Esc 收起
time.sleep(0.5)
print("Esc 后 面板可见=%s 前台回到记事本=%s (cls=%s)" % (
      u.IsWindowVisible(panel), u.GetForegroundWindow() == hwnd, cls(u.GetForegroundWindow())))
