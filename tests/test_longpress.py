# -*- coding: utf-8 -*-
"""
长按 Win+V 的回归测试：面板只能切换一次，不能出现 Hide/Show 高速交替（闪烁）。

用法：先启动 winvclip.exe，然后
    python test_longpress.py            # 长按 1.5 秒
    python test_longpress.py hold 3     # 长按 3 秒
"""
# --- 路径基准：从 tests/ 回到项目根（P0 结构整理时注入） ---
import os as _os, sys as _sys
_os.chdir(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import ctypes, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
PANEL_CLASS = "WinVClipPanelUI"


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


def panel_hwnd():
    return u.FindWindowW(PANEL_CLASS, None)


def visible():
    h = panel_hwnd()
    return bool(h) and bool(u.IsWindowVisible(h))


def watch(ms, stop=None):
    """在 ms 毫秒内轮询面板可见性，返回状态变化次数与轨迹"""
    t0 = time.time()
    last = visible()
    flips = 0
    trace = []
    while (time.time() - t0) * 1000 < ms:
        if stop is not None and stop():
            break
        v = visible()
        if v != last:
            flips += 1
            trace.append((round((time.time() - t0) * 1000), v))
            last = v
        time.sleep(0.004)
    return flips, trace, last


def once():
    """点按一次 Win+V"""
    before = visible()
    key(0x5B); time.sleep(0.04)
    key(0x56); time.sleep(0.04)
    key(0x56, True); time.sleep(0.04)
    key(0x5B, True); time.sleep(0.25)
    return before, visible()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "hold"
    if not panel_hwnd():
        print("找不到面板窗口，请先启动 winvclip.exe")
        return 1

    if mode == "seq":
        # 连续点按三次：可见性应依次翻转
        for i in range(3):
            b, a = once()
            print(f"  第{i+1}次点按：{b} -> {a}")
        return 0

    if mode == "double":
        # Win 一直按住，长按 V 两次：应「显示 → 隐藏」，且各自只切换一次
        if visible():
            once(); time.sleep(0.2)
        print(f"起始可见 = {visible()}（Win 全程按住）")
        key(0x5B); time.sleep(0.03)
        for round_ in (1, 2):
            key(0x56)
            t0 = time.time()
            last = visible()
            flips = 1 if last else 0
            while (time.time() - t0) < 1.0:
                key(0x56)
                v = visible()
                if v != last:
                    flips += 1
                    last = v
                time.sleep(0.03)
            key(0x56, True); time.sleep(0.15)
            print(f"  第{round_}次长按：可见 = {visible()}，切换 {flips} 次")
        key(0x5B, True); time.sleep(0.2)
        print(f"Win 松开后可见 = {visible()}")
        return 0

    if mode == "once":
        b, a = once()
        print(f"点按一次 Win+V：可见 {b} -> {a}  ({'切换成功' if a != b else '没有切换'})")
        return 0

    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 1.5
    # 先确保起始状态是隐藏的，便于判断
    if visible():
        once()
        time.sleep(0.2)

    print(f"起始可见 = {visible()}，长按 Win+V {secs}s（重复速率 33/s，3ms 密集采样）…")
    # 用独立线程以 3ms 间隔采样，避免 30ms 采样漏掉"闪现一下又回去"
    import threading
    samples = []
    stop = threading.Event()

    def sampler():
        t0 = time.time()
        last = None
        while not stop.is_set():
            v = visible()
            if v != last:
                samples.append((round((time.time() - t0) * 1000), v))
                last = v
            time.sleep(0.003)

    key(0x5B); time.sleep(0.03)
    th = threading.Thread(target=sampler)
    th.start()
    time.sleep(0.01)
    t0 = time.time()
    key(0x56)
    while (time.time() - t0) < secs:
        key(0x56)
        time.sleep(0.03)
    key(0x56, True); time.sleep(0.05)
    key(0x5B, True); time.sleep(0.2)
    stop.set(); th.join()

    flips = max(0, len(samples) - 1)
    print(f"长按结束：可见 = {visible()}，过程中可见性变化 {flips} 次")
    print("轨迹(ms, visible):", samples)
    ok = flips <= 1 and len(samples) >= 1 and samples[-1][1] is True
    print("结果:", "通过 —— 只切换了一次，无闪烁" if ok else "失败 —— 仍在反复切换")

    # 长按之后必须还能正常点按关闭（验证按下状态已复位）
    b, a = once()
    print(f"随后点按一次：{b} -> {a}", "(能正常关闭，按下状态已复位)" if (b and not a) else "(异常：无法关闭)")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
