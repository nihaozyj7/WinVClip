# -*- coding: utf-8 -*-
"""固定态连续插入：每插一个，影子光标应当跟着插入符右移。"""
import ctypes, time
from ctypes import wintypes

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
u.IsWindowVisible.argtypes = [ctypes.c_void_p]
u.GetForegroundWindow.restype = ctypes.c_void_p


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


def winv():
    key(0x5B); time.sleep(0.05); key(0x56); time.sleep(0.05)
    key(0x56, True); time.sleep(0.05); key(0x5B, True)


def click(h, x, y):
    lp = ((y & 0xFFFF) << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0200, 0, lp)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


def ghost_rect():
    g = u.FindWindowW("WinVClipCaretGhost", None)
    r = wintypes.RECT(); u.GetWindowRect(g, ctypes.byref(r))
    return (bool(u.IsWindowVisible(g)), r.left, r.top, r.right, r.bottom)


np = u.FindWindowW(None, "*无标题 - 记事本") or u.FindWindowW(None, "无标题 - 记事本")
if u.GetForegroundWindow() != np:
    raise SystemExit("请先把记事本置前再跑（前台=0x%X 记事本=0x%X）"
                     % (u.GetForegroundWindow(), np))
print("foreground = 0x%X notepad = 0x%X" % (u.GetForegroundWindow(), np))

panel = u.FindWindowW("WinVClipPanelUI", None)
if u.IsWindowVisible(panel):
    winv(); time.sleep(0.5)
winv(); time.sleep(0.8)
r = wintypes.RECT(); u.GetWindowRect(panel, ctypes.byref(r))
w = r.right - r.left
print("panel w =", w)

click(panel, 235, 17); time.sleep(0.4)          # 表情页
click(panel, w - 75, 20); time.sleep(0.4)       # 图钉：临时固定
print("pinned (toast):", bool(u.IsWindowVisible(panel)))
print("ghost before insert:", ghost_rect())

click(panel, 40, 170); time.sleep(0.7)
print("ghost after 1st:", ghost_rect())
click(panel, 96, 170); time.sleep(0.7)
print("ghost after 2nd:", ghost_rect())
print("panel visible:", bool(u.IsWindowVisible(panel)))

click(panel, w - 75, 20); time.sleep(0.3)       # 取消固定
winv(); time.sleep(0.4)
print("ghost after close:", ghost_rect())
