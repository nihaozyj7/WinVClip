# -*- coding: utf-8 -*-
"""winvclip 回归测试公共层。

为什么有这个文件：22 个脚本原先各自抄一份 ctypes 样板（key/tap/set_clip/
force_fg/panel …），同一功能存在两套命名（winv() / win_vi()），
test_noactivate.py 里 force_fg 甚至被定义了两次、前者被静默覆盖。
样板抄错一次就是"用例假失败 → 以为是回归"的长期噪音源。

三条硬规则（改这个文件前先读）：

1. **所有 Win32 函数必须在导入时声明 argtypes/restype。**
   ctypes 默认 restype 是 c_int，64 位下 HWND/HANDLE 会被截断，
   表现为随机崩溃或句柄比较永不相等。原先只有部分脚本声明了，
   这正是某些用例"偶发假失败"的来源之一。

2. **两种鼠标点击语义必须走不同函数，不能共用 `click`。**
   - `post_click(hwnd, x, y)`：PostMessage 投递坐标，用于驱动面板。
     面板是 WS_EX_NOACTIVATE，无法 focus，真实鼠标会落到前台窗口，
     所以面板交互一律用这个。
   - `real_click(x, y)`：SetCursorPos + mouse_event，屏幕绝对坐标，
     用于验证低级鼠标钩子（面板外点击收起）这类必须走真实输入的用例。

3. **导入本模块即把工作目录切到项目根。**
   脚本里 `build\\winvclip.exe` 这类相对路径因此始终有效，
   各脚本不再需要自己的 chdir 样板。

用法（脚本头部）::

    import wvclip as w
    w.win_v()                 # 唤起面板
    p = w.panel()             # 等面板出现
    w.post_click(p, 100, 200) # 点面板内部
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

# --------------------------------------------------------------------------
# 路径与环境
# --------------------------------------------------------------------------
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_EXE = os.path.join(ROOT, "build", "winvclip.exe")
os.chdir(ROOT)

u = ctypes.WinDLL("user32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
U = u  # 旧脚本里的短名，保留以免全量改调用
k = k32
WT = wintypes

u.SetProcessDPIAware()


# --------------------------------------------------------------------------
# 常量
# --------------------------------------------------------------------------
PANEL_CLASS = "WinVClipPanelUI"

VK = {
    "win": 0x5B, "v": 0x56, "esc": 0x1B, "enter": 0x0D, "tab": 0x09, "space": 0x20,
    "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27,
    "shift": 0x10, "ctrl": 0x11, "alt": 0x12,
    "a": 0x41, "c": 0x43, "d": 0x44, "x": 0x58, "z": 0x5A, "f": 0x46,
    "back": 0x08, "del": 0x2E, "home": 0x24, "end": 0x23,
    "pgup": 0x21, "pgdn": 0x22,
}

CF_UNICODETEXT = 13
CF_HDROP = 15

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
WM_RBUTTONDOWN, WM_RBUTTONUP = 0x0204, 0x0205
WM_GETTEXT, WM_GETTEXTLENGTH = 0x000D, 0x000E

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010

KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004


# --------------------------------------------------------------------------
# Win32 绑定（argtypes/restype 必须齐全，见文件头规则 1）
# --------------------------------------------------------------------------
def _bind(fn, restype, *argtypes):
    fn.restype = restype
    fn.argtypes = list(argtypes)


HWND = ctypes.c_void_p
_bind(u.FindWindowW, HWND, wintypes.LPCWSTR, wintypes.LPCWSTR)
_bind(u.FindWindowExW, HWND, HWND, HWND, wintypes.LPCWSTR, wintypes.LPCWSTR)
_bind(u.GetForegroundWindow, HWND)
_bind(u.GetDesktopWindow, HWND)
_bind(u.GetWindow, HWND, HWND, wintypes.UINT)
_bind(u.GetParent, HWND, HWND)
_bind(u.GetAncestor, HWND, HWND, wintypes.UINT)
_bind(u.WindowFromPoint, HWND, wintypes.POINT)
_bind(u.IsWindowVisible, wintypes.BOOL, HWND)
_bind(u.IsWindow, wintypes.BOOL, HWND)
_bind(u.SetForegroundWindow, wintypes.BOOL, HWND)
_bind(u.BringWindowToTop, wintypes.BOOL, HWND)
_bind(u.SetActiveWindow, HWND, HWND)
_bind(u.SetFocus, HWND, HWND)
_bind(u.GetFocus, HWND)
_bind(u.AttachThreadInput, wintypes.BOOL, wintypes.DWORD, wintypes.DWORD, wintypes.BOOL)
_bind(u.GetWindowThreadProcessId, wintypes.DWORD, HWND, ctypes.POINTER(wintypes.DWORD))
_bind(u.GetWindowRect, wintypes.BOOL, HWND, ctypes.POINTER(wintypes.RECT))
_bind(u.GetClientRect, wintypes.BOOL, HWND, ctypes.POINTER(wintypes.RECT))
_bind(u.ClientToScreen, wintypes.BOOL, HWND, ctypes.POINTER(wintypes.POINT))
_bind(u.ScreenToClient, wintypes.BOOL, HWND, ctypes.POINTER(wintypes.POINT))
_bind(u.GetClassNameW, ctypes.c_int, HWND, wintypes.LPWSTR, ctypes.c_int)
_bind(u.GetWindowTextW, ctypes.c_int, HWND, wintypes.LPWSTR, ctypes.c_int)
_bind(u.GetWindowTextLengthW, ctypes.c_int, HWND)
_bind(u.PostMessageW, wintypes.BOOL, HWND, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p)
_bind(u.SendMessageW, ctypes.c_ssize_t, HWND, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p)
_bind(u.SendInput, wintypes.UINT, wintypes.UINT, ctypes.c_void_p, ctypes.c_int)
_bind(u.OpenClipboard, wintypes.BOOL, HWND)
_bind(u.CloseClipboard, wintypes.BOOL)
_bind(u.EmptyClipboard, wintypes.BOOL)
_bind(u.SetClipboardData, ctypes.c_void_p, wintypes.UINT, ctypes.c_void_p)
_bind(u.GetClipboardData, ctypes.c_void_p, wintypes.UINT)
_bind(u.GetClipboardSequenceNumber, wintypes.DWORD)
_bind(u.SetCursorPos, wintypes.BOOL, ctypes.c_int, ctypes.c_int)
_bind(u.GetCursorPos, wintypes.BOOL, ctypes.POINTER(wintypes.POINT))
_bind(u.mouse_event, None, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
      wintypes.DWORD, ctypes.c_size_t)
_bind(u.GetGUIThreadInfo, wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p)
_bind(u.EnumWindows, wintypes.BOOL, ctypes.c_void_p, wintypes.LPARAM)
_bind(u.EnumChildWindows, wintypes.BOOL, HWND, ctypes.c_void_p, wintypes.LPARAM)
_bind(u.ShowWindow, wintypes.BOOL, HWND, ctypes.c_int)
_bind(u.SetWindowPos, wintypes.BOOL, HWND, HWND, ctypes.c_int, ctypes.c_int,
      ctypes.c_int, ctypes.c_int, wintypes.UINT)
_bind(u.GetShellWindow, HWND)
_bind(u.GetSystemMetrics, ctypes.c_int, ctypes.c_int)

_bind(k32.GlobalAlloc, ctypes.c_void_p, wintypes.UINT, ctypes.c_size_t)
_bind(k32.GlobalLock, ctypes.c_void_p, ctypes.c_void_p)
_bind(k32.GlobalUnlock, wintypes.BOOL, ctypes.c_void_p)
_bind(k32.GlobalFree, ctypes.c_void_p, ctypes.c_void_p)
_bind(k32.GetCurrentThreadId, wintypes.DWORD)
_bind(k32.OpenProcess, ctypes.c_void_p, wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
_bind(k32.CloseHandle, wintypes.BOOL, ctypes.c_void_p)
_bind(k32.QueryFullProcessImageNameW, wintypes.BOOL, ctypes.c_void_p,
      wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))

ENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, HWND, wintypes.LPARAM)


# --------------------------------------------------------------------------
# 输入结构
# --------------------------------------------------------------------------
class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("pad", ctypes.c_byte * 32)]


_I = _INPUTUNION  # 旧脚本里的别名


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUTUNION)]


class GUITHREADINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hwndActive", HWND), ("hwndFocus", HWND),
                ("hwndCapture", HWND), ("hwndMenuOwner", HWND),
                ("hwndMoveSize", HWND), ("hwndCaret", HWND),
                ("rcCaret", wintypes.RECT)]


# --------------------------------------------------------------------------
# 键盘
# --------------------------------------------------------------------------
def _send(inp):
    u.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def key(vk, up=False, uni=None):
    """按下一个虚拟键（up=True 抬起）。

    uni= 是历史调用形态（`key(0, uni="A")`），等价于 key_uni。
    保留它是为了让旧用例逐字不改——注入语义存在两种写法时，
    统一命名带来的收益远小于改错调用点的风险。
    """
    if uni is not None:
        key_uni(uni, up)
        return
    i = INPUT()
    i.type = 1
    i.u.ki.wVk = vk
    i.u.ki.dwFlags = KEYEVENTF_KEYUP if up else 0
    _send(i)


key_down = lambda vk: key(vk, False)
key_up = lambda vk: key(vk, True)


def key_uni(ch, up=False):
    """以 KEYEVENTF_UNICODE 注入单个 UTF-16 码元（不受键盘布局影响）。"""
    i = INPUT()
    i.type = 1
    i.u.ki.wScan = ord(ch) & 0xFFFF
    i.u.ki.wVk = 0
    i.u.ki.dwFlags = KEYEVENTF_UNICODE | (KEYEVENTF_KEYUP if up else 0)
    _send(i)


def tap(vk, n=1, gap=0.03):
    for _ in range(n):
        key(vk)
        time.sleep(gap)
        key(vk, True)
        time.sleep(gap)


def uni(text, gap=0.02):
    """逐码元注入一段文本。注意：不支持输入法组合语义，只适合 ASCII/单码点。"""
    for ch in text:
        key_uni(ch)
        time.sleep(0.01)
        key_uni(ch, True)
        time.sleep(gap)


def combo(mod, vk, settle=0.06):
    """mod 按下 → tap(vk) → mod 抬起。"""
    key(mod)
    time.sleep(0.03)
    tap(vk)
    key(mod, True)
    time.sleep(settle)


def win_v(settle=0.9):
    """Win+V 唤起面板。

    单独实现而不复用 combo，是因为面板热键走 WH_KEYBOARD_LL，
    按键必须真实成对下发，且 Win 抬起前不能有其他键，否则会被判成组合键。
    """
    key(VK["win"])
    time.sleep(0.06)
    tap(VK["v"])
    time.sleep(0.06)
    key(VK["win"], True)
    time.sleep(settle)


# 旧名别名：统一后逐步淘汰，新脚本一律用 win_v
winv = win_v
win_vi = win_v


# --------------------------------------------------------------------------
# 剪贴板
# --------------------------------------------------------------------------
def _open_clip(retries=20, gap=0.03):
    for _ in range(retries):
        if u.OpenClipboard(None):
            return True
        time.sleep(gap)
    return False


def set_clip(text):
    """写入 UNICODETEXT。失败返回 False（剪贴板被别的进程占着）。"""
    if not _open_clip():
        return False
    try:
        u.EmptyClipboard()
        data = (text + "\0").encode("utf-16-le")
        h = k32.GlobalAlloc(0x0042, len(data))  # GMEM_MOVEABLE|GMEM_ZEROINIT
        if not h:
            return False
        p = k32.GlobalLock(h)
        ctypes.memmove(p, data, len(data))
        k32.GlobalUnlock(h)
        u.SetClipboardData(CF_UNICODETEXT, h)
        return True
    finally:
        u.CloseClipboard()


def get_clip():
    if not _open_clip():
        return ""
    try:
        h = u.GetClipboardData(CF_UNICODETEXT)
        if not h:
            return ""
        p = k32.GlobalLock(h)
        s = ctypes.wstring_at(p)
        k32.GlobalUnlock(h)
        return s
    finally:
        u.CloseClipboard()


def clip_seq():
    """剪贴板序列号。用来断言"一次插入没碰剪贴板"。"""
    return u.GetClipboardSequenceNumber()


# --------------------------------------------------------------------------
# 窗口查询
# --------------------------------------------------------------------------
def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, b, 256)
    return b.value


def title(h):
    n = u.GetWindowTextLengthW(h)
    if n <= 0:
        return ""
    b = ctypes.create_unicode_buffer(n + 1)
    u.GetWindowTextW(h, b, n + 1)
    return b.value


class Rect(tuple):
    """窗口矩形。

    既支持 `l, t, r, b = rect(h)` 解包 / `r[0]` 索引，也支持 `r.left` 属性访问。
    历史脚本两种写法都有（有的按属性读，有的按索引读），统一成一种会无谓
    地改到十几个调用点，而这类改动最容易引入"看起来对、算错了"的错误。
    """
    __slots__ = ()

    def __new__(cls, l, t, r, b):
        return super().__new__(cls, (l, t, r, b))

    left = property(lambda s: s[0])
    top = property(lambda s: s[1])
    right = property(lambda s: s[2])
    bottom = property(lambda s: s[3])
    w = property(lambda s: s[2] - s[0])
    h = property(lambda s: s[3] - s[1])


def rect(h):
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return Rect(r.left, r.top, r.right, r.bottom)


def size(h):
    r = rect(h)
    return (r.w, r.h)


def fg():
    return u.GetForegroundWindow()


def fg_desc():
    """当前前台的「类名/标题/hwnd」。失败时先打印它，别急着怀疑回归。"""
    h = fg()
    return "%s | %r | %s" % (cls(h), title(h), h)


def force_fg(hwnd, tries=8):
    """强抢前台并校验。

    跨进程 SetForegroundWindow 会被前台锁静默拒绝（Edge 尤其顽固），
    必须 AttachThreadInput + 校验循环；返回是否真的拿到。
    """
    if not hwnd:
        return False
    if u.GetForegroundWindow() == hwnd:
        return True
    my = k32.GetCurrentThreadId()
    for _ in range(tries):
        cur = u.GetForegroundWindow()
        t_cur = u.GetWindowThreadProcessId(cur, None)
        t_dst = u.GetWindowThreadProcessId(hwnd, None)
        u.AttachThreadInput(my, t_cur, True)
        u.AttachThreadInput(my, t_dst, True)
        u.BringWindowToTop(hwnd)
        u.SetForegroundWindow(hwnd)
        u.SetActiveWindow(hwnd)
        u.SetFocus(hwnd)
        u.AttachThreadInput(my, t_cur, False)
        u.AttachThreadInput(my, t_dst, False)
        time.sleep(0.08)
        if u.GetForegroundWindow() == hwnd:
            return True
        # 屏保/前台锁的常见绕过：先按一次无副作用的修饰键
        key(VK["alt"])
        key(VK["alt"], True)
        time.sleep(0.05)
        u.SetForegroundWindow(hwnd)
    return u.GetForegroundWindow() == hwnd


def find_class(name, visible_only=True):
    res = []

    @ENUMPROC
    def cb(h, l):
        if (not visible_only or u.IsWindowVisible(h)) and cls(h) == name:
            res.append(h)
        return True

    u.EnumWindows(cb, 0)
    return res


def find_title(sub, visible_only=True):
    """按标题子串查窗口，返回 [hwnd, ...]（不区分大小写）。

    空串表示"所有有标题的可见窗口"。多数用例只用 hwnd，所以主签名返回
    hwnd 列表；需要标题文本时用 find_title_ex。
    """
    return [h for h, _t in find_title_ex(sub, visible_only)]


def find_title_ex(sub, visible_only=True):
    """同 find_title，但返回 [(hwnd, title), ...]。"""
    out = []
    sub = sub.lower()

    @ENUMPROC
    def cb(h, l):
        if not visible_only or u.IsWindowVisible(h):
            t = title(h)
            if t and sub in t.lower():
                out.append((h, t))
        return True

    u.EnumWindows(cb, 0)
    return out


def find_windows(pred, visible_only=True):
    """自定义谓词枚举。pred(hwnd, cls, title) -> bool。"""
    res = []

    @ENUMPROC
    def cb(h, l):
        if not visible_only or u.IsWindowVisible(h):
            if pred(h, cls(h), title(h)):
                res.append(h)
        return True

    u.EnumWindows(cb, 0)
    return res


def child_windows(h):
    res = []

    @ENUMPROC
    def cb(c, l):
        res.append(c)
        return True

    u.EnumChildWindows(h, cb, 0)
    return res


def find_notepad():
    """Win11 记事本进程名下可能有多窗口，取第一个可见的。

    注意不要按标题精确匹配：Win11 记事本有改动时标题带 '*'。
    """
    hits = find_windows(lambda h, c, t: c == "Notepad"
                        or "记事本" in t or "Notepad" in t)
    return hits[0] if hits else None


def panel(timeout=3.0):
    """等面板窗口出现（不要求可见），超时抛 SystemExit。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        h = u.FindWindowW(PANEL_CLASS, None)
        if h:
            return h
        time.sleep(0.05)
    raise SystemExit("panel not created")


def wait_panel(visible=True, timeout=3.0):
    """等面板出现且（可见/不可见）状态达成，返回 hwnd 或 None。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        h = u.FindWindowW(PANEL_CLASS, None)
        if h and bool(u.IsWindowVisible(h)) == visible:
            return h
        time.sleep(0.05)
    return None


def vis(h):
    return bool(h) and bool(u.IsWindowVisible(h))


def inside(r, x, y):
    """点是否落在矩形内。

    r 是矩形（Rect/tuple），**不是窗口句柄**——历史用例一律这么调，
    句柄版本见 inside_win。参数含义搞反时 ctypes 会在 GetWindowRect
    处抛 ArgumentError，所以两种用法必须分开命名。
    """
    return r[0] <= x < r[2] and r[1] <= y < r[3]


def inside_win(h, x, y):
    """点是否落在窗口矩形内（屏幕坐标）。"""
    return inside(rect(h), x, y)


def client_origin(h):
    r = wintypes.RECT()
    u.GetClientRect(h, ctypes.byref(r))
    p = wintypes.POINT(0, 0)
    u.ClientToScreen(h, ctypes.byref(p))
    return (p.x, p.y, r.right, r.bottom)


# --------------------------------------------------------------------------
# 鼠标：两种语义，别混用（见文件头规则 2）
# --------------------------------------------------------------------------
def _lp(x, y):
    return ((int(y) & 0xFFFF) << 16) | (int(x) & 0xFFFF)


def post(h, msg, x=0, y=0, wp=0):
    """向窗口投递一条带坐标的消息（坐标为客户端坐标）。"""
    u.PostMessageW(h, msg, wp, _lp(x, y))


def post_click(h, x, y, button="left"):
    """消息投递式点击：驱动 NOACTIVATE 面板的唯一可靠手段。"""
    down, up = ((WM_LBUTTONDOWN, WM_LBUTTONUP) if button == "left"
                else (WM_RBUTTONDOWN, WM_RBUTTONUP))
    u.PostMessageW(h, WM_MOUSEMOVE, 0, _lp(x, y))
    u.PostMessageW(h, down, 1, _lp(x, y))
    time.sleep(0.05)
    u.PostMessageW(h, up, 0, _lp(x, y))


def real_click(x, y, button="left", settle=0.08):
    """真实鼠标点击（屏幕绝对坐标）。会经过全局低级钩子。"""
    u.SetCursorPos(int(x), int(y))
    time.sleep(settle)
    if button == "left":
        u.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.05)
        u.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    else:
        u.mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0)
        time.sleep(0.05)
        u.mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0)


def real_drag(x0, y0, x1, y1, steps=12, settle=0.05):
    """真实鼠标拖拽（屏幕绝对坐标），用于测拖动/缩放。"""
    u.SetCursorPos(int(x0), int(y0))
    time.sleep(0.12)
    u.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.1)
    for i in range(1, steps + 1):
        u.SetCursorPos(int(x0 + (x1 - x0) * i / steps),
                       int(y0 + (y1 - y0) * i / steps))
        time.sleep(settle / 3)
    time.sleep(settle)
    u.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.1)


# 旧名：click 在旧脚本里有两种语义，别名统一指向消息投递（面板是最常见场景）
post_click_ = post_click


# --------------------------------------------------------------------------
# 控件与文本
# --------------------------------------------------------------------------
def caret_of(hwnd):
    """取目标线程的插入符矩形（屏幕坐标）。

    Chromium/Electron 的插入符在渲染进程里，hwndCaret 恒为 0 ——
    取不到**不代表**目标不能接收文本，见 wvclip 项目记忆。
    """
    tid = u.GetWindowThreadProcessId(hwnd, None)
    gi = GUITHREADINFO()
    gi.cbSize = ctypes.sizeof(gi)
    if not u.GetGUIThreadInfo(tid, ctypes.byref(gi)):
        return None
    if not gi.hwndCaret:
        return None
    return (gi.rcCaret.left, gi.rcCaret.top, gi.rcCaret.right, gi.rcCaret.bottom)


def text_of(h):
    n = u.SendMessageW(h, WM_GETTEXTLENGTH, 0, 0)
    if n <= 0:
        return ""
    buf = ctypes.create_unicode_buffer(n + 2)
    u.SendMessageW(h, WM_GETTEXT, n + 1, buf)
    return buf.value


def edit_of(hwnd):
    """找窗口里可编辑的子控件。

    Win11 记事本不再有 class="Edit" 的子窗口（换成 RichEditD2DPT），
    所以这里按类型列表逐个试，而不是只 FindWindowExW("Edit")。
    """
    for klass in ("Edit", "RichEditD2DPT", "RichEdit20W", "RichEdit50W"):
        e = u.FindWindowExW(hwnd, None, klass, None)
        if e:
            return e
    for c in child_windows(hwnd):
        if cls(c) in ("Edit", "RichEditD2DPT", "RichEdit20W", "RichEdit50W"):
            return c
    return None


def edit_text(hwnd):
    """读窗口内文本（自身或子控件），取不到返回 ""。"""
    e = edit_of(hwnd) or hwnd
    return text_of(e)


def focus_of(hwnd):
    """目标线程的焦点控件 hwnd（无则 None）。"""
    tid = u.GetWindowThreadProcessId(hwnd, None)
    gi = GUITHREADINFO()
    gi.cbSize = ctypes.sizeof(gi)
    if not u.GetGUIThreadInfo(tid, ctypes.byref(gi)):
        return None
    return gi.hwndFocus or None


# --------------------------------------------------------------------------
# 进程
# --------------------------------------------------------------------------
def exe_path(hwnd):
    pid = wintypes.DWORD()
    u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if not pid.value:
        return ""
    h = k32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return ""
    try:
        n = wintypes.DWORD(1024)
        buf = ctypes.create_unicode_buffer(1024)
        if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
            return buf.value
        return ""
    finally:
        k32.CloseHandle(h)


def start_notepad(wait=1.6):
    p = subprocess.Popen(["notepad.exe"])
    time.sleep(wait)
    h = find_notepad()
    if not h:
        raise SystemExit("notepad 没起来（或标题/类名变了）")
    return p, h


def kill(proc):
    """温和收尾：先 terminate，避免留下后台进程。"""
    if not proc:
        return
    try:
        proc.terminate()
        proc.wait(timeout=3)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


def require_build():
    """确认 exe 存在；不存在直接给出该怎么构建。"""
    if not os.path.exists(BUILD_EXE):
        raise SystemExit("找不到 %s，先跑：cmake --build build" % BUILD_EXE)
    return BUILD_EXE


# --------------------------------------------------------------------------
# 断言辅助
# --------------------------------------------------------------------------
_fails = []


def section(name):
    print("\n== %s ==" % name)


def chk(cond, msg, detail=""):
    """记录一条断言。失败不中断，便于一次看全部结论。"""
    if cond:
        print("  [OK]   %s" % msg)
    else:
        line = "  [FAIL] %s%s" % (msg, ("  <- " + detail) if detail else "")
        print(line)
        _fails.append(msg)
    return bool(cond)


def need(cond, msg, detail=""):
    """硬断言：失败立即 SystemExit。"""
    if not cond:
        print("  [FAIL] %s%s" % (msg, ("  <- " + detail) if detail else ""))
        raise SystemExit(1)
    print("  [OK]   %s" % msg)
    return True


def report(exit_code=1):
    """打印汇总，有失败则退出非 0。"""
    if _fails:
        print("\n失败 %d 项：%s" % (len(_fails), ", ".join(_fails)))
        raise SystemExit(exit_code)
    print("\n全部通过")
    return 0


# --------------------------------------------------------------------------
# 面板布局复算（S() 缩放 + 设置卡片 / 确认卡片的几何）
# 自绘界面没有可查询的控件树，坐标只能两边各算一遍：这里的数字必须与
# src/ui/layout.cpp 的 ComputeSettings / ModalRect 保持一致。
# --------------------------------------------------------------------------
_bind(u.GetDpiForWindow, wintypes.UINT, HWND)


def scale_dpi(hwnd):
    """面板当前的 DPI（即源码里 S(v) = MulDiv(v, dpi, 96) 的那个 dpi）。"""
    try:
        d = u.GetDpiForWindow(hwnd)
    except Exception:
        d = 0
    return d or 96


def S(v, dpi=96):
    """逻辑像素 -> 物理像素，与 core/util.h 的 S() 同一算法（MulDiv 四舍五入）。"""
    return (int(v) * int(dpi) + 48) // 96


SET_ROW_HEIGHTS = [52, 52, 56, 64, 52, 52, 62]   # SET_ROW_* 的自然高度


shell32 = ctypes.WinDLL("shell32", use_last_error=True)
_bind(shell32.IsUserAnAdmin, wintypes.BOOL)


def is_elevated():
    """当前 python 进程是否有管理员权限。

    决定 SendInput 会不会被 UIPI 挡：提权的程序面板只收得下同权限进程的输入，
    测试脚本若跑在普通权限下，Win+V 根本到不了面板（这类假失败要先看这一项）。
    """
    try:
        return bool(shell32.IsUserAnAdmin())
    except Exception:
        return False


def settings_geom(hwnd):
    """面板上的齿轮按钮中心（客户区坐标）。

    设置本身是独立窗口（类名 WinVClipSettings），它的几何见 settings_win_geom——
    2026 年起设置不再是「盖在面板上的卡片」，别再按面板尺寸复算行位置。
    """
    dpi = scale_dpi(hwnd)
    W, _ = size(hwnd)
    btn = S(34, dpi)                    # HEAD_H：右上三个按钮是 HEAD_H 见方
    return {"gear": (W - btn * 3 // 2, btn // 2)}


def settings_win(timeout=3.0):
    """等并返回设置窗口（独立窗口）。没打开时返回 None。"""
    end = time.time() + timeout
    while time.time() < end:
        hs = find_class("WinVClipSettings", visible_only=False)
        if hs:
            return hs[0] if isinstance(hs, list) else hs
        time.sleep(0.05)
    return None


def settings_win_geom(sw):
    """设置窗口客户区里的行几何：卡片 = 整个客户区，无居中、无压缩。"""
    dpi = scale_dpi(sw)
    _, _, cw, ch = client_origin(sw)
    rows, top = [], S(48, dpi)          # headH = 48
    for v in SET_ROW_HEIGHTS:
        rows.append((top, top + S(v, dpi)))
        top += S(v, dpi)
    return {"w": cw, "h": ch, "x": 0, "y": 0, "rows": rows}


def modal_ok_point(hwnd, clear=False):
    """确认卡片上「确认」按钮的中心。

    卡片在**它所在的窗口客户区**里居中：面板里的删除确认用它、设置窗口里的
    清空确认也用它（两者客户区不同，所以必须传对应窗口的 hwnd）。
    """
    dpi = scale_dpi(hwnd)

    def s(v):
        return S(v, dpi)

    _, _, W, H = client_origin(hwnd)
    mw = s(300)
    mh = s(178) if clear else s(146)
    mx, my = (W - mw) // 2, (H - mh) // 2
    bw = (mw - s(40) - s(8)) // 2
    by = my + mh - s(20) - s(38)
    return (mx + s(20) + bw // 2, by + s(38) // 2)


__all__ = [
    # 环境
    "ROOT", "BUILD_EXE", "u", "U", "k32", "k", "WT", "os", "sys", "time",
    "subprocess", "ctypes", "wintypes",
    # 常量
    "PANEL_CLASS", "VK", "CF_UNICODETEXT", "CF_HDROP",
    "WM_MOUSEMOVE", "WM_LBUTTONDOWN", "WM_LBUTTONUP",
    "WM_RBUTTONDOWN", "WM_RBUTTONUP", "WM_GETTEXT", "WM_GETTEXTLENGTH",
    # 结构
    "KEYBDINPUT", "MOUSEINPUT", "INPUT", "GUITHREADINFO", "HWND", "ENUMPROC", "Rect",
    # 键鼠
    "key", "key_down", "key_up", "key_uni", "tap", "uni", "combo", "win_v",
    "winv", "win_vi",
    "post", "post_click", "real_click", "real_drag",
    # 剪贴板
    "set_clip", "get_clip", "clip_seq",
    # 窗口
    "cls", "title", "rect", "size", "fg", "fg_desc", "force_fg",
    "find_class", "find_title", "find_title_ex", "find_windows", "child_windows",
    "find_notepad", "panel", "wait_panel", "vis", "inside", "inside_win",
    "client_origin",
    # 控件
    "caret_of", "text_of", "edit_of", "edit_text", "focus_of",
    # 进程
    "exe_path", "start_notepad", "kill", "require_build",
    # 布局复算
    "scale_dpi", "S", "settings_geom", "settings_win", "settings_win_geom",
    "modal_ok_point", "SET_ROW_HEIGHTS", "is_elevated",
    # 断言
    "section", "chk", "need", "report",
]
