# -*- coding: utf-8 -*-
"""表情 / 符号页的滚动：滚轮与滚动条都要让格子真的位移，且不能误触别的动作。

为什么单列一个用例：网格页与历史列表走两套偏移量（g_gridScroll /
g_scrollY），踩过的坑都是「看着滚了、其实只是某一半动了」：
  · 滚轮改 g_gridScroll 却不重算布局 → 视图动了、滚动条的滑块钉在原地
    （用户侧原话：「滚动条用滚轮时无响应，但视图确实在滚动」）；
  · 网格页以前是一格滚轮直接跳 52px（一整排瞬间换掉），后来改成与列表同一套
    smoothstep 缓动，这里顺带守住「缓动走完后落点正确」。

判据全靠「点一格、看记事本多了什么」：字形页插入走文本插入符直插通道，
不碰剪贴板，所以剪贴板序列号可以拿来断言「这次操作没发生粘贴」。
  · 搜索框先打一个 "a"：搜索结果列表是稳定的静态顺序，而「最近使用」分类会
    因为插入而重排——拿它做基准等于让每次点击自己改期望值（踩过）。
  · 连点同一格两次必须先得到同一个字形：这是本用例的前置自检，不成立就 SKIP，
    别把「前置不成立」报成回归（历史里有置顶条目就是这么坑过 test_scroll）。
"""
import time

import wvclip as w
from wvclip import *  # noqa: F401,F403  统一键鼠/窗口/剪贴板/断言样板

CELL_X, CELL_Y = 40, 170      # 网格第一格的中心（488 高面板下稳定）


def show(h, tries=6):
    for _ in range(tries):
        if vis(h):
            return True
        win_v()
        for _ in range(40):
            if vis(h):
                return True
            time.sleep(0.05)
    return False


def pin(h):
    """点右上角图钉（临时固定）：不固定的话每次插入都会收起面板。"""
    W = w.size(h)[0]
    post_click(h, W - 85, 17)
    time.sleep(0.35)


def wheel(h, notches, down=True):
    """滚轮。delta 正 = 向上，所以「向下滚」要投负值。"""
    d = -120 if down else 120
    for _ in range(notches):
        u.PostMessageW(h, 0x020A, ((d & 0xFFFF) << 16), 0)      # WM_MOUSEWHEEL
        time.sleep(0.06)
    time.sleep(0.45)          # 缓动最长 220ms，等它走完再点


def paste_char(h, np, prev):
    """点一格，等记事本里多出东西，返回新增的那段文本。

    读文本必须等它稳定：注入是逐字符走的，正好读到「代理对只写了一半」的那
    一刻就会切出半个字符（实测 want='\\ud83d' got='😀' 的假失败就是这么来的）。
    """
    post_click(h, CELL_X, CELL_Y)
    for _ in range(40):
        time.sleep(0.05)
        t = edit_text(np)
        if len(t) > len(prev):
            time.sleep(0.15)
            t2 = edit_text(np)
            if len(t2) >= len(t):
                t = t2
            return t[len(prev):]
    return ""


def cp(s):
    """字形比较用：只取第一个完整码位。"""
    if not s:
        return ""
    if 0xD800 <= ord(s[0]) <= 0xDBFF and len(s) > 1:
        return s[:2]
    return s[:1]


def backspace(h):
    u.PostMessageW(h, 0x0100, 0x08, 0)        # WM_KEYDOWN VK_BACK
    time.sleep(0.2)


def enter_grid(h, np):
    """Tab 循环找到字形页（表情/符号）。用「点一格粘出的是短字形」认页。

    返回 (是否找到, 基准字形)。找到时搜索框里已经有一个 "a"。
    """
    for _ in range(4):
        u.PostMessageW(h, 0x0100, 0x09, 0)    # WM_KEYDOWN VK_TAB
        time.sleep(0.3)
        u.PostMessageW(h, 0x0102, ord("a"), 0)   # WM_CHAR：搜索，锁定稳定顺序
        time.sleep(0.5)
        prev = edit_text(np)
        got = paste_char(h, np, prev)
        if 0 < len(got) <= 8:                 # 字形（含代理对/组合序列）都很短
            return True, got
        backspace(h)                           # 不是字形页：清掉搜索串再来
    return False, ""


def drag(h, y0, y1):
    """在右缘滚动条上按住拖一段（客户区坐标）。"""
    W = w.size(h)[0]
    x = W - 6                                  # 命中带是 [W-10, W-1)
    u.PostMessageW(h, 0x0200, 0, (y0 << 16) | x)             # WM_MOUSEMOVE 先移过去
    u.PostMessageW(h, 0x0201, 1, (y0 << 16) | x)             # WM_LBUTTONDOWN
    step = 20 if y1 >= y0 else -20
    for y in range(y0 + step, y1 + step, step):
        u.PostMessageW(h, 0x0200, 1, (y << 16) | x)
        time.sleep(0.03)
    u.PostMessageW(h, 0x0202, 0, (y1 << 16) | x)             # WM_LBUTTONUP
    time.sleep(0.45)


def click_bar(h, y):
    """点轨道空白：滑块中心跳到光标处。"""
    W = w.size(h)[0]
    post_click(h, W - 6, y)
    time.sleep(0.45)


def main():
    proc = subprocess.Popen(["notepad.exe"])
    time.sleep(1.6)
    np = (u.FindWindowW(None, "无标题 - 记事本")
          or u.FindWindowW(None, "Untitled - Notepad"))
    if not np:
        proc.terminate()
        raise SystemExit("没找到记事本窗口")
    if not force_fg(np):
        proc.terminate()
        raise SystemExit("无法把记事本切到前台，测试不可信")
    time.sleep(0.3)

    h = panel()
    if not show(h):
        proc.terminate()
        raise SystemExit("面板没弹出")
    pin(h)
    if not vis(h):
        proc.terminate()
        raise SystemExit("没固定住面板（点在关闭按钮上了？）")

    ok, base = enter_grid(h, np)
    if not ok:
        proc.terminate()
        raise SystemExit("没切到字形页（面板尺寸/状态与本脚本假设不符）")

    # 前置自检：同一格连点两次必须得到同一个字形，否则「最近使用」在重排，测不准
    prev = edit_text(np)
    again = paste_char(h, np, prev)
    if again != base:
        proc.terminate()
        raise SystemExit("SKIP 同一格连点两次字形不同（分类在重排），本用例前置不成立")

    # 1) 滚轮：格子必须换掉一批
    section("滚轮滚动")
    wheel(h, 40, down=False)                  # 先滚到顶，从确定起点开始
    prev = edit_text(np)
    top = paste_char(h, np, prev)
    chk(top != "", "到顶后点第一格粘出了字形", "got=%r" % top)
    wheel(h, 3)
    prev = edit_text(np)
    down3 = paste_char(h, np, prev)
    chk(down3 != "", "滚 3 格后点第一格粘出了字形", "got=%r" % down3)
    chk(cp(down3) != cp(top), "滚 3 格后第一格换成了别的字形",
        "top=%r now=%r" % (top, down3))

    # 2) 滚回顶部：落点必须严丝合缝回到第一格（缓动收尾不能留下零头）
    wheel(h, 40, down=False)
    prev = edit_text(np)
    back = paste_char(h, np, prev)
    chk(cp(back) == cp(top), "滚回顶部后第一格与初始一致",
        "want=%r got=%r" % (top, back))

    # 3) 点轨道空白：滑块跟到光标处，内容大跨度位移，且不能是粘贴/改尺寸
    section("滚动条")
    seq0, win0 = clip_seq(), rect(h)
    click_bar(h, 430)
    chk(clip_seq() == seq0, "点轨道没有触发粘贴（剪贴板序列号未变）")
    chk(rect(h) == win0, "点轨道没有误入抓右缘改尺寸", "now=%r" % (rect(h),))
    prev = edit_text(np)
    far = paste_char(h, np, prev)
    chk(far != "", "点轨道后点第一格粘出了字形", "got=%r" % far)
    chk(far != top, "点轨道后内容确实位移了", "before=%r after=%r" % (top, far))

    # 4) 拖滑块：内容跟手、不触发行粘贴、不改窗口尺寸
    seq0, win0 = clip_seq(), rect(h)
    bar0 = rect(h)
    drag(h, 400, 60)
    chk(clip_seq() == seq0, "拖滚动条没有触发粘贴（剪贴板序列号未变）")
    chk(rect(h) == bar0, "拖滚动条没有误入抓右缘改尺寸", "now=%r" % (rect(h),))
    prev = edit_text(np)
    after = paste_char(h, np, prev)
    chk(after != "", "拖动后点第一格粘出了字形", "got=%r" % after)
    chk(after != far, "拖滚动条后内容确实位移了", "before=%r after=%r" % (far, after))

    pin(h)                                     # 取消固定
    u.PostMessageW(h, 0x0100, 0x1B, 0)         # Esc 收起
    time.sleep(0.3)
    proc.terminate()
    report(0)


main()
