# -*- coding: utf-8 -*-
"""回归：表情 / 符号的两条插入通道。

  - 有文本插入符的目标（记事本、Edit/RichEdit/Office）：KEYEVENTF_UNICODE 直插，剪贴板全程不动；
  - 取不到插入符的目标（Chromium/Electron 页面，插入符在渲染进程里）：借剪贴板 + Ctrl+V，
    粘完把原剪贴板还原，且借用过程不进历史；
  - 第三个参数 shift：按住 Shift 点格子，任何目标都强制走粘贴通道（注入无效时的手动兜底）。

期望：目标拿到 emoji；`clip after insert` 仍是哨兵 ORIGINAL-ABC。
用法: python test_glyph_channel.py notepad|web|explorer [shift]
"""

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板
import ctypes, subprocess, sys, time, urllib.parse


TAG = "WINVCLIP-CLIP"


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
        win_v()
    win_v()
    if not vis(h):
        raise SystemExit("panel did not show")

    set_clip("ORIGINAL-ABC")
    time.sleep(0.5)
    post_click(h, 184, 17)          # 表情页签
    time.sleep(0.4)
    shift = len(sys.argv) > 2 and sys.argv[2] == "shift"
    if shift:
        key(0x10)              # 按住 Shift = 强制走剪贴板粘贴通道
        time.sleep(0.15)
    post_click(h, 40, 175)          # 第一格
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
