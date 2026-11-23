# -*- coding: utf-8 -*-
"""真实输入法（微软拼音）验证：向面板发硬件级字母键，看组合串与候选窗。

用法：
  python test_ime.py compose weixiao   # 逐字母发送，面板应显示组合串（带下划线）
  python test_ime.py space             # 空格上屏
  python test_ime.py esc               # 取消组合

注意：这里必须用 SendInput 发 VK（而不是 KEYEVENTF_UNICODE），
      只有真实键盘事件才会被输入法拦截成组合串。
"""
import ctypes, sys, time
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
    i = INPUT()
    i.type = 1
    i.u.ki.wVk = vk
    i.u.ki.dwFlags = 2 if up else 0
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def tap(vk, hold=0.03):
    key(vk)
    time.sleep(hold)
    key(vk, True)
    time.sleep(hold)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "compose"
    hwnd = u.FindWindowW("WinVClipPanelUI", None)
    if not hwnd:
        raise SystemExit("panel not created")
    u.SetForegroundWindow(hwnd)
    time.sleep(0.2)
    if cmd == "compose":
        for ch in sys.argv[2]:
            tap(ord(ch.upper()))
        print("composed %r" % sys.argv[2])
    elif cmd == "space":
        tap(0x20)
        print("space sent")
    elif cmd == "esc":
        tap(0x1B)
        print("esc sent")
    elif cmd == "enter":
        tap(0x0D)
        print("enter sent")
    elif cmd == "shift":
        tap(0x10)
        print("shift sent")


main()
